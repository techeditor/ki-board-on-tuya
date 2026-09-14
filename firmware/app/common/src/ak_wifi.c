/**
 * @file ak_wifi.c
 * @brief Wi-Fi provisioning backend — stub + gated netmgr impl. See ak_wifi.h.
 */

#include "ak_wifi.h"

#include "ak_features.h"
#include "tal_api.h"

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

#include <string.h>

static ak_wifi_status_cb s_on_status = NULL;
static char              s_ssid[WIFI_SSID_LEN + 1] = "";

static void __netmgr_status_cb(netmgr_type_e type, netmgr_status_e status)
{
    (void)type;
    if (s_on_status == NULL) {
        return;
    }
    switch (status) {
    case NETMGR_LINK_UP:
    case NETMGR_LINK_UP_SWITH:
        s_on_status(AK_WIFI_CONNECTED, s_ssid);
        break;
    case NETMGR_LINK_DOWN:
    default:
        s_on_status(AK_WIFI_DISCONNECTED, s_ssid);
        break;
    }
}

OPERATE_RET ak_wifi_init(ak_wifi_status_cb on_status)
{
    s_on_status = on_status;
    /* Route netmgr link transitions into our status callback. */
    void (*cb)(netmgr_type_e, netmgr_status_e) = __netmgr_status_cb;
    (void)netmgr_conn_set(NETCONN_AUTO, NETCONN_CMD_SET_STATUS_CB, (void *)&cb);

    /* netmgr may already be online before this callback is registered (cloud
     * bring-up runs first), so publish its current state immediately too. */
    netmgr_status_e status = NETMGR_LINK_DOWN;
    if (netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status) == OPRT_OK) {
        __netmgr_status_cb(NETCONN_AUTO, status);
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
        if (s_on_status) {
            s_on_status(AK_WIFI_FAILED, "");
        }
        return;
    }
    memset(&info, 0, sizeof(info));
    strncpy(info.ssid, ssid, sizeof(info.ssid) - 1);
    if (pass) {
        strncpy(info.pswd, pass, sizeof(info.pswd) - 1);
    }
    strncpy(s_ssid, info.ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    if (s_on_status) {
        s_on_status(AK_WIFI_CONNECTING, s_ssid);
    }
    OPERATE_RET rt = netmgr_conn_set(NETCONN_AUTO, NETCONN_CMD_SSID_PSWD, &info);
    if (rt != OPRT_OK) {
        PR_ERR("ak_wifi: netmgr set SSID_PSWD failed: %d", rt);
        if (s_on_status) {
            s_on_status(AK_WIFI_FAILED, s_ssid);
        }
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

static ak_wifi_status_cb s_on_status = NULL;

OPERATE_RET ak_wifi_init(ak_wifi_status_cb on_status)
{
    s_on_status = on_status;
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
    if (s_on_status) {
        s_on_status(AK_WIFI_FAILED, ssid ? ssid : "");
    }
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
