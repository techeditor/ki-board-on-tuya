/**
 * @file ak_wifi.c
 * @brief Wi-Fi provisioning backend — stub + gated netmgr impl. See ak_wifi.h.
 */

#include "ak_wifi.h"

#include "ak_features.h"
#include "tal_api.h"

#include <stdio.h>
#include <string.h>

#define AK_WIFI_SSID_CAPACITY 33U

static ak_wifi_status_cb s_on_status = NULL;
static ak_wifi_state_t   s_current_state = AK_WIFI_DISCONNECTED;
static char              s_current_ssid[AK_WIFI_SSID_CAPACITY] = "";
static volatile bool     s_config_disconnect_pending = false;
static int8_t            s_license_present_cache = -1; /* -1 unknown, 0 missing, 1 present */

static void report_status(ak_wifi_state_t state, const char *ssid)
{
    s_current_state = state;
    if (ssid != NULL && ssid != s_current_ssid) {
        (void)snprintf(s_current_ssid, sizeof(s_current_ssid), "%s", ssid);
    }
    if (s_on_status != NULL) {
        s_on_status(state, s_current_ssid);
    }
}

const char *ak_wifi_state_str(ak_wifi_state_t s)
{
    switch (s) {
    case AK_WIFI_CONNECTING:
        return "connecting";
    case AK_WIFI_CONNECTED:
        return "connected";
    case AK_WIFI_DISCONNECTED:
        return "disconnected";
    case AK_WIFI_FAILED:
    default:
        return "failed";
    }
}

const char *ak_license_result_str(ak_license_result_t r)
{
    switch (r) {
    case AK_LICENSE_WRITTEN:
        return "written";
    case AK_LICENSE_FAILED:
    default:
        return "failed";
    }
}

#if AK_ASR_BOARD_CLOUD

/* ── Real backend (Tuya netmgr) ─────────────────────────────────────────────
 * netmgr_conn_set(NETCONN_AUTO, NETCONN_CMD_SSID_PSWD, {ssid,pswd}) applies the
 * credentials and starts connecting + cloud activation. Link transitions are
 * reported through a netmgr status callback, translated to ak_wifi states. */

#include "netmgr.h"
#include "netconn_wifi.h"
#include "tal_wifi.h"
#include "tuya_authorize.h"
#include "tuya_iot.h"
#if AK_ENABLE_LICENSE_DEBUG_READ
#include "tuya_config.h"
#endif

/* TuyaOpen's current netconn_wifi backend exports this persistence helper but
 * does not declare it in netconn_wifi.h. Use the SDK-owned implementation so
 * its "netinfo" storage format and factory-reset behavior stay aligned. */
extern OPERATE_RET __netconn_wifi_info_set(netconn_wifi_info_t *info);

static void __netmgr_status_cb(netmgr_type_e type, netmgr_status_e status)
{
    (void)type;
    switch (status) {
    case NETMGR_LINK_UP:
    case NETMGR_LINK_UP_SWITH:
        s_config_disconnect_pending = false;
        report_status(AK_WIFI_CONNECTED, s_current_ssid);
        break;
    case NETMGR_LINK_DOWN:
    default:
        /* Reconfiguring while already linked requires one deliberate disconnect
         * before TuyaOpen will reconnect with the newly copied credentials.
         * Keep the UI in connecting for that expected edge; later failures are
         * reported as disconnected normally. */
        if (s_config_disconnect_pending) {
            s_config_disconnect_pending = false;
            report_status(AK_WIFI_CONNECTING, s_current_ssid);
        } else {
            report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
        }
        break;
    }
}

/* netconn_wifi does not implement NETCONN_CMD_SET_STATUS_CB in the current
 * TuyaOpen SDK. Subscribe to netmgr's public link-status event instead. */
static int __netmgr_link_status_event(void *data)
{
    if (data != NULL) {
        __netmgr_status_cb(NETCONN_WIFI, *(netmgr_status_e *)data);
    }
    return 0;
}

OPERATE_RET ak_wifi_init(ak_wifi_status_cb on_status)
{
    s_on_status = on_status;

    /* Subscribe before querying so a link-up transition cannot fall into the
     * gap between the initial status read and event registration. */
    OPERATE_RET rt = tal_event_subscribe(EVENT_LINK_STATUS_CHG, "ak_wifi",
                                         __netmgr_link_status_event,
                                         SUBSCRIBE_TYPE_NORMAL);
    if (rt != OPRT_OK) {
        PR_WARN("ak_wifi: subscribe link status failed: %d", rt);
    }

    /* Cloud bring-up may already have loaded credentials and connected before
     * this module starts. Restore the persisted target name before reporting
     * the initial link state; the password stays local and is never emitted. */
    netconn_wifi_info_t wifi_info = {0};
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &wifi_info) == OPRT_OK) {
        (void)snprintf(s_current_ssid, sizeof(s_current_ssid), "%s", wifi_info.ssid);
    }

    netmgr_status_e status = NETMGR_LINK_DOWN;
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status) == OPRT_OK) {
        __netmgr_status_cb(NETCONN_WIFI, status);
    } else {
        report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
    }
    return OPRT_OK;
}

bool ak_wifi_available(void)
{
    return true;
}

void ak_wifi_connect(const char *ssid, const char *pass)
{
    netconn_wifi_info_t info;
    netmgr_status_e     status_before = NETMGR_LINK_DOWN;

    if (ssid == NULL || ssid[0] == '\0') {
        report_status(AK_WIFI_FAILED, "");
        return;
    }
    (void)netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status_before);
    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
    if (pass) {
        strncpy(info.pswd, pass, sizeof(info.pswd) - 1);
    }

    /* NETCONN_CMD_SSID_PSWD only updates RAM in this SDK. Persist through the
     * backend helper first so an activated device can reconnect after reboot;
     * EVENT_RESET deletes the same record during three-boot provisioning reset. */
    OPERATE_RET rt = __netconn_wifi_info_set(&info);
    if (rt != OPRT_OK) {
        PR_ERR("ak_wifi: persist SSID_PSWD failed: %d", rt);
        report_status(AK_WIFI_FAILED, info.ssid);
        return;
    }

    bool was_linked = status_before == NETMGR_LINK_UP ||
                      status_before == NETMGR_LINK_UP_SWITH;
    s_config_disconnect_pending = was_linked;
    report_status(AK_WIFI_CONNECTING, info.ssid);
    rt = netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info);
    if (rt != OPRT_OK) {
        s_config_disconnect_pending = false;
        PR_ERR("ak_wifi: netmgr set SSID_PSWD failed: %d", rt);
        report_status(AK_WIFI_FAILED, s_current_ssid);
        return;
    }

    /* TuyaOpen's __netconn_wifi_connect() returns immediately when its internal
     * state is already LINKUP. That leaves the old radio link alive, emits no
     * new LINK_UP event, and strands Companion at "connecting". Disconnect
     * after copying the new credentials: the SDK's disconnect event transitions
     * LINKUP -> READY and schedules a reconnect using the new values. */
    if (was_linked) {
        rt = tal_wifi_station_disconnect();
        if (rt != OPRT_OK) {
            s_config_disconnect_pending = false;
            PR_ERR("ak_wifi: disconnect before credential reconnect failed: %d", rt);
            report_status(AK_WIFI_FAILED, s_current_ssid);
        }
    }
    /* Otherwise the connect result arrives asynchronously via the netmgr status
     * callback. The deliberate LINK_DOWN above remains "connecting"; the next
     * real failure is reported as disconnected, and LINK_UP reports connected. */
}

ak_license_result_t ak_license_write(const char *uuid, const char *authkey)
{
    if (uuid == NULL || uuid[0] == '\0' || authkey == NULL || authkey[0] == '\0') {
        return AK_LICENSE_FAILED;
    }
    OPERATE_RET rt = tuya_authorize_write(uuid, authkey);
    if (rt != OPRT_OK) {
        PR_ERR("ak_license: tuya_authorize_write failed: %d", rt);
        return AK_LICENSE_FAILED;
    }
    PR_NOTICE("ak_license: UUID/AuthKey written to storage");
    s_license_present_cache = 1;
    return AK_LICENSE_WRITTEN;
}

bool ak_license_present(void)
{
    if (s_license_present_cache >= 0) {
        return s_license_present_cache == 1;
    }

    tuya_iot_license_t lic = {0};
    if (tuya_authorize_read(&lic) != OPRT_OK) {
        s_license_present_cache = 0;
        return false;
    }
    bool present = lic.uuid != NULL && lic.uuid[0] != '\0';
    s_license_present_cache = present ? 1 : 0;
    return present;
}

bool ak_license_debug_read(ak_license_debug_info_t *info)
{
    if (info == NULL) {
        return false;
    }
    memset(info, 0, sizeof(*info));
#if AK_ENABLE_LICENSE_DEBUG_READ
    tuya_iot_license_t lic = {0};
    if (tuya_authorize_read(&lic) == OPRT_OK) {
        info->source = "flashed";
    } else {
        lic.uuid     = TUYA_OPENSDK_UUID;
        lic.authkey  = TUYA_OPENSDK_AUTHKEY;
        info->source = "compile_time";
    }
    if (lic.uuid == NULL || lic.uuid[0] == '\0' || lic.authkey == NULL || lic.authkey[0] == '\0') {
        info->source = "missing";
        return false;
    }

    (void)snprintf(info->uuid, sizeof(info->uuid), "%s", lic.uuid);
    size_t      authkey_len = strlen(lic.authkey);
    const char *tail        = authkey_len > 4U ? lic.authkey + authkey_len - 4U : lic.authkey;
    (void)snprintf(info->authkey_tail, sizeof(info->authkey_tail), "%s", tail);
    return true;
#else
    info->source = "disabled";
    return false;
#endif
}

#else /* !AK_ASR_BOARD_CLOUD — stub (no wifi stack in this profile) */

OPERATE_RET ak_wifi_init(ak_wifi_status_cb on_status)
{
    s_on_status = on_status;
    report_status(AK_WIFI_DISCONNECTED, "");
    return OPRT_OK;
}

bool ak_wifi_available(void)
{
    return false;
}

void ak_wifi_connect(const char *ssid, const char *pass)
{
    (void)pass;
    /* No wifi stack in this build profile — report failure so the companion
     * shows why (needs the cloud-ASR/wifi profile flashed). */
    report_status(AK_WIFI_FAILED, ssid ? ssid : "");
}

ak_license_result_t ak_license_write(const char *uuid, const char *authkey)
{
    (void)uuid;
    (void)authkey;
    /* No authorize stack in this profile. */
    return AK_LICENSE_FAILED;
}

bool ak_license_present(void)
{
    return false;
}

bool ak_license_debug_read(ak_license_debug_info_t *info)
{
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
        info->source = "unavailable";
    }
    return false;
}

#endif /* AK_ASR_BOARD_CLOUD */

const char *ak_wifi_current_ssid(void)
{
    return s_current_ssid;
}

ak_wifi_state_t ak_wifi_current_state(void)
{
    return s_current_state;
}
