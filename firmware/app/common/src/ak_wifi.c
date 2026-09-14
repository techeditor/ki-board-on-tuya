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
#include "tuya_authorize.h"
#include "tuya_iot.h"

static void __netmgr_status_cb(netmgr_type_e type, netmgr_status_e status)
{
    (void)type;
    switch (status) {
    case NETMGR_LINK_UP:
    case NETMGR_LINK_UP_SWITH:
        report_status(AK_WIFI_CONNECTED, s_current_ssid);
        break;
    case NETMGR_LINK_DOWN:
    default:
        report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
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

    if (ssid == NULL || ssid[0] == '\0') {
        report_status(AK_WIFI_FAILED, "");
        return;
    }
    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
    if (pass) {
        strncpy(info.pswd, pass, sizeof(info.pswd) - 1);
    }
    report_status(AK_WIFI_CONNECTING, info.ssid);
    OPERATE_RET rt = netmgr_conn_set(NETCONN_AUTO, NETCONN_CMD_SSID_PSWD, &info);
    if (rt != OPRT_OK) {
        PR_ERR("ak_wifi: netmgr set SSID_PSWD failed: %d", rt);
        report_status(AK_WIFI_FAILED, s_current_ssid);
    }
    /* On success the connect result arrives asynchronously via the netmgr
     * status callback (__netmgr_status_cb). */
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
    return AK_LICENSE_WRITTEN;
}

bool ak_license_present(void)
{
    tuya_iot_license_t lic = {0};
    if (tuya_authorize_read(&lic) != OPRT_OK) {
        return false;
    }
    return lic.uuid != NULL && lic.uuid[0] != '\0';
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

#endif /* AK_ASR_BOARD_CLOUD */

const char *ak_wifi_current_ssid(void)
{
    return s_current_ssid;
}

ak_wifi_state_t ak_wifi_current_state(void)
{
    return s_current_state;
}
