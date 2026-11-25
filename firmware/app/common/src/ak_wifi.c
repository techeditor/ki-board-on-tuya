/**
 * @file ak_wifi.c
 * @brief Wi-Fi provisioning backend — stub + gated netmgr impl. See ak_wifi.h.
 */

#include "ak_wifi.h"

#include "ak_cloud.h"
#include "ak_cred.h"
#include "ak_features.h"
#include "ak_validate.h"
#include "ak_kv.h"
#include "ak_pid.h"
#include "tal_api.h"
#include "tal_wifi.h"

#include <stdio.h>
#include <string.h>

#define AK_WIFI_SSID_CAPACITY 33U

static ak_wifi_status_cb s_on_status = NULL;
static ak_wifi_state_t   s_current_state = AK_WIFI_DISCONNECTED;
static char              s_current_ssid[AK_WIFI_SSID_CAPACITY] = "";
static volatile bool     s_config_disconnect_pending = false;

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
    case AK_WIFI_RESTORED:
        return "restored";
    case AK_WIFI_FAILED:
    default:
        return "failed";
    }
}

const char *ak_license_result_str(ak_license_result_t r)
{
    switch (r) {
    case AK_LICENSE_WRITTEN_REBOOT_REQUIRED:
        return "written_reboot_required";
    case AK_LICENSE_REJECTED:
        return "rejected";
    case AK_LICENSE_FAILED:
    default:
        return "failed";
    }
}

#if AK_ASR_BOARD_CLOUD
static int8_t s_license_present_cache = -1; /* -1 unknown, 0 missing, 1 present */

/* ── Real backend (Tuya netmgr) ─────────────────────────────────────────────
 * netmgr_conn_set(NETCONN_AUTO, NETCONN_CMD_SSID_PSWD, {ssid,pswd}) applies the
 * credentials and starts connecting + cloud activation. Link transitions are
 * reported through a netmgr status callback, translated to ak_wifi states. */

#include "netmgr.h"
#include "netconn_wifi.h"
#include "tal_wifi.h"
#include "tuya_authorize.h"
#include "tuya_iot.h"
#include "tuya_config.h"

/* KV key holding the License that a confirmed overwrite replaced. */
#define AK_LICENSE_BACKUP_KEY "ak_lic_bak"

/* Wi-Fi rollback (FW-12): the last working configuration is kept in RAM for
 * AK_WIFI_ROLLBACK_MS after a new one is applied, and put back if the new
 * network never links up. Written by the app thread, cleared by the netmgr
 * callback on LINK_UP; a race can at worst restore a network that just came
 * up, which then reconnects to the old, known-good AP. */
static netconn_wifi_info_t s_rollback_info;
static volatile bool       s_rollback_armed;
static uint32_t            s_rollback_deadline_ms;

/* TuyaOpen's current netconn_wifi backend exports this persistence helper but
 * does not declare it in netconn_wifi.h. Use the SDK-owned implementation so
 * its "netinfo" storage format and factory-reset behavior stay aligned. */
extern OPERATE_RET __netconn_wifi_info_set(netconn_wifi_info_t *info);
extern OPERATE_RET __netconn_wifi_info_get(netconn_wifi_info_t *info);

/* ── FR-15: remembered networks + automatic reconnect ─────────────────────── */
#define AK_WIFI_KV_KEY      "ak_wifis"
#define AK_WIFI_SCAN_MAX    24U
#define AK_WIFI_STOP_MS     2500U /* let the Tuya reconnect loop stop before scanning */
#define AK_WIFI_SCAN_MAX_MS 15000U

typedef enum {
    ROAM_IDLE = 0, /* linked, or waiting out the grace period */
    ROAM_STOPPING, /* Tuya reconnect loop closed, radio settling */
    ROAM_SCANNING,
    ROAM_TRYING,  /* one remembered network is being joined */
    ROAM_BACKOFF, /* every candidate failed */
} roam_state_t;

static ak_wifi_store_t  s_store;
static bool             s_store_loaded;
static uint32_t         s_revision;
static uint8_t          s_blob[AK_WIFI_STORE_BLOB_MAX];
static char             s_forgotten[AK_WIFI_STORE_SSID_MAX + 1U]; /* deleted, still linked */
static volatile bool    s_record_pending;
static uint32_t         s_record_deadline_ms;
static volatile bool    s_roam_allowed;
static roam_state_t     s_roam;
static uint32_t         s_roam_deadline_ms;
static uint32_t         s_down_since_ms;
static bool             s_down_timing;
static uint16_t         s_tried;
static uint8_t          s_round;
static int              s_try_idx;
static ak_wifi_scan_t   s_scan[AK_WIFI_SCAN_MAX];
static volatile uint8_t s_scan_n;
static volatile uint8_t s_scan_state; /* 0 idle, 1 running, 2 done */
static bool             s_sec_lookup_pending;
static bool             s_sec_scan_started;
static ak_wifi_attempt_t s_attempt; /* "connecting" for an SDK join/rejoin, ak_wifi_store.h */

/* Show "connecting" for a round of the roamer (once per round, not per frame). */
static void roam_report_connecting(void)
{
    ak_wifi_attempt_clear(&s_attempt); /* the roamer reports the end itself */
    if (s_current_state != AK_WIFI_CONNECTING) {
        report_status(AK_WIFI_CONNECTING, s_current_ssid);
    }
}

static void store_save(void)
{
    size_t      n  = ak_wifi_store_serialize(&s_store, s_blob, sizeof(s_blob));
    OPERATE_RET rt = n > 0U ? ak_kv_set(AK_WIFI_KV_KEY, s_blob, n) : OPRT_COM_ERROR;
    memset(s_blob, 0, sizeof(s_blob));
    if (rt != OPRT_OK) {
        PR_WARN("ak_wifi: saving the network list failed: %d", rt);
    }
    s_revision++;
}

/* Load the list once the KV is up. A list that fails its version or CRC check
 * is dropped as a whole; the single configuration the SDK stored (pre-FR-15
 * firmware, or the fallback after a bad list) then becomes record 1. */
static void store_load(void)
{
    if (s_store_loaded || !ak_kv_ready()) {
        return;
    }
    s_store_loaded = true;
    ak_wifi_store_init(&s_store);
    uint8_t *blob = NULL;
    size_t   len  = 0U;
    /* The network the SDK is configured for (in use): never trimmed away. */
    netconn_wifi_info_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    bool have_legacy = __netconn_wifi_info_get(&legacy) == OPRT_OK;
    if (ak_kv_get(AK_WIFI_KV_KEY, &blob, &len) == OPRT_OK && blob != NULL) {
        ak_wifi_blob_result_t r =
            ak_wifi_store_parse_keep(&s_store, blob, len, have_legacy && legacy.ssid[0] != '\0' ? legacy.ssid : NULL);
        if (r == AK_WIFI_BLOB_TRIMMED) {
            PR_NOTICE("ak_wifi: saved list cut to the %u most recent networks", (unsigned)AK_WIFI_STORE_MAX);
            store_save();
        } else if (r != AK_WIFI_BLOB_OK) {
            PR_WARN("ak_wifi: saved network list is invalid (%d); starting empty", (int)r);
        }
        ak_kv_free(blob);
    }
    if (s_store.count == 0U) {
        if (have_legacy && ak_wifi_store_migrate_legacy(&s_store, legacy.ssid, legacy.pswd)) {
            PR_NOTICE("ak_wifi: stored network \"%s\" is now saved network 1", legacy.ssid);
            store_save();
        }
    }
    memset(&legacy, 0, sizeof(legacy));
}

static void scan_job(void *arg)
{
    (void)arg;
    AP_IF_S *aps = NULL;
    uint32_t num = 0U;
    uint8_t  n   = 0U;
    if (tal_wifi_all_ap_scan(&aps, &num) == OPRT_OK && aps != NULL) {
        for (uint32_t i = 0U; i < num && n < AK_WIFI_SCAN_MAX; i++) {
            size_t len = aps[i].s_len;
            if (len == 0U || len > AK_WIFI_STORE_SSID_MAX) {
                continue;
            }
            memset(&s_scan[n], 0, sizeof(s_scan[n]));
            memcpy(s_scan[n].ssid, aps[i].ssid, len);
            s_scan[n].rssi = aps[i].rssi;
            s_scan[n].sec  = aps[i].security;
            n++;
        }
        (void)tal_wifi_release_ap(aps);
    }
    s_scan_n     = n;
    s_scan_state = 2U;
}

static void scan_start(void)
{
    if (s_scan_state == 1U) {
        return;
    }
    s_scan_n     = 0U;
    s_scan_state = 1U;
    if (tal_workq_schedule(WORKQ_SYSTEM, scan_job, NULL) != OPRT_OK) {
        s_scan_state = 2U; /* nothing found; the caller handles an empty scan */
    }
}

/* Remember the network the device is linked to. Reads the netmgr's current
 * configuration, so networks written by the Companion, by Tuya provisioning
 * and by the reconnect logic are all covered. Needs an IP address. */
static void record_current_link(uint32_t now_ms)
{
    netconn_wifi_info_t info;
    NW_IP_S             ip;
    memset(&info, 0, sizeof(info));
    memset(&ip, 0, sizeof(ip));
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info) != OPRT_OK || info.ssid[0] == '\0') {
        s_record_pending = false;
        return;
    }
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_IP, &ip) != OPRT_OK || ip.nwipstr[0] == '\0' ||
        strcmp(ip.nwipstr, "0.0.0.0") == 0) {
        if ((int32_t)(now_ms - s_record_deadline_ms) >= 0) {
            s_record_pending = false; /* never got an address: not a working network */
        }
        return;
    }
    s_record_pending = false;
    if (s_forgotten[0] != '\0') {
        if (strcmp(s_forgotten, info.ssid) == 0) {
            memset(&info, 0, sizeof(info));
            return; /* deleted by the user; stays linked only until it drops */
        }
        s_forgotten[0] = '\0';
    }
    int     idx = ak_wifi_store_find(&s_store, info.ssid);
    uint8_t sec = AK_WIFI_SEC_UNKNOWN;
    for (uint8_t i = 0U; i < s_scan_n; i++) {
        if (strcmp(s_scan[i].ssid, info.ssid) == 0) {
            sec = s_scan[i].sec;
        }
    }
    if (idx >= 0 && s_store.rec[idx].seq == s_store.next_seq && strcmp(s_store.rec[idx].pass, info.pswd) == 0) {
        memset(&info, 0, sizeof(info));
        return; /* already the most recent record: no flash write */
    }
    char                   dropped[AK_WIFI_STORE_SSID_MAX + 1U];
    ak_wifi_store_result_t r = ak_wifi_store_touch(&s_store, info.ssid, info.pswd, sec, dropped);
    if (r != AK_WIFI_STORE_INVALID) {
        store_save();
        PR_NOTICE("ak_wifi: saved network \"%s\" (%s)%s%s", info.ssid, r == AK_WIFI_STORE_ADDED ? "new" : "updated",
                  r == AK_WIFI_STORE_REPLACED ? ", replaced " : "", r == AK_WIFI_STORE_REPLACED ? dropped : "");
        int at = ak_wifi_store_find(&s_store, info.ssid);
        if (at >= 0 && s_store.rec[at].sec == AK_WIFI_SEC_UNKNOWN) {
            s_sec_lookup_pending = true;
            s_sec_scan_started   = false;
        }
    }
    memset(&info, 0, sizeof(info));
}

/* One background scan after a link-up fills in the security type. */
static void sec_lookup_tick(void)
{
    if (!s_sec_scan_started) {
        if (s_scan_state != 1U) {
            scan_start();
            s_sec_scan_started = true;
        }
        return;
    }
    if (s_scan_state != 2U) {
        return;
    }
    s_sec_lookup_pending = false;
    s_sec_scan_started   = false;
    s_scan_state         = 0U;
    int idx              = ak_wifi_store_find(&s_store, s_current_ssid);
    if (idx < 0 || s_store.rec[idx].sec != AK_WIFI_SEC_UNKNOWN) {
        return;
    }
    for (uint8_t i = 0U; i < s_scan_n; i++) {
        if (strcmp(s_scan[i].ssid, s_current_ssid) == 0) {
            s_store.rec[idx].sec = s_scan[i].sec;
            store_save();
            return;
        }
    }
}

static void roam_reset(void)
{
    s_roam        = ROAM_IDLE;
    s_down_timing = false;
    s_tried       = 0U;
    s_round       = 0U;
}

static bool roam_possible(void)
{
    tuya_iot_client_t *client = tuya_iot_client_get();
    return s_roam_allowed && client != NULL && client->is_activated && s_store.count > 0U && !s_rollback_armed &&
           !s_config_disconnect_pending;
}

static void roam_apply(int idx)
{
    netconn_wifi_info_t info;
    memset(&info, 0, sizeof(info));
    memcpy(info.ssid, s_store.rec[idx].ssid, sizeof(info.ssid) - 1U);
    memcpy(info.pswd, s_store.rec[idx].pass, sizeof(info.pswd) - 1U);
    (void)__netconn_wifi_info_set(&info); /* a reboot resumes with the last tried network */
    PR_NOTICE("ak_wifi: trying saved network \"%s\"", info.ssid);
    ak_wifi_attempt_clear(&s_attempt);
    report_status(AK_WIFI_CONNECTING, info.ssid);
    OPERATE_RET rt = netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &info);
    if (rt != OPRT_OK) {
        PR_ERR("ak_wifi: reconnect set failed: %d", rt);
    }
    memset(&info, 0, sizeof(info));
}

/* Pick the next candidate from the scan, or start the back-off. */
static void roam_next(uint32_t now_ms)
{
    uint8_t plan[AK_WIFI_STORE_MAX];
    int     idx = -1;
    if (ak_wifi_store_plan(&s_store, s_scan, s_scan_n, s_tried, plan, sizeof(plan)) > 0U) {
        idx = plan[0];
    } else if (s_tried == 0U) {
        /* No stored network in the scan (hidden SSID, scan failure): try the
         * most recent one once, blind. shortcut: other hidden networks are
         * never tried; they only work when they are the most recent one. */
        idx = ak_wifi_store_mru(&s_store, 0U);
    }
    if (idx >= 0) {
        s_try_idx          = idx;
        s_roam             = ROAM_TRYING;
        s_roam_deadline_ms = now_ms + AK_WIFI_TRY_TIMEOUT_MS;
        roam_apply(idx);
        return;
    }
    s_roam             = ROAM_BACKOFF;
    s_roam_deadline_ms = now_ms + ak_wifi_roam_backoff_ms(s_round);
    PR_NOTICE("ak_wifi: no saved network could be joined; next try in %u s",
              (unsigned)(ak_wifi_roam_backoff_ms(s_round) / 1000U));
    if (s_round < 250U) {
        s_round++;
    }
    s_tried = 0U;
    report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
}

static void roam_tick(uint32_t now_ms)
{
    store_load();
    if (s_record_pending) {
        record_current_link(now_ms);
    }
    bool linked = s_current_state == AK_WIFI_CONNECTED;
    if (s_sec_lookup_pending && linked) {
        sec_lookup_tick();
    }
    /* A network deleted while linked: once the link drops, stop the Tuya
     * reconnect loop (it would rejoin the deleted network) and move on. */
    if (s_forgotten[0] != '\0' && !linked) {
        s_forgotten[0] = '\0';
        (void)netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_CLOSE, NULL);
        roam_reset();
        s_down_timing   = true;
        s_down_since_ms = now_ms - AK_WIFI_ROAM_GRACE_MS;
    }
    if (linked) {
        if (s_roam != ROAM_IDLE || s_down_timing) {
            roam_reset();
        }
        return;
    }
    if (!roam_possible()) {
        if (s_roam != ROAM_IDLE || s_down_timing) {
            roam_reset();
        }
        return;
    }
    switch (s_roam) {
    case ROAM_IDLE:
        if (!s_down_timing) {
            s_down_timing   = true;
            s_down_since_ms = now_ms;
        } else if ((int32_t)(now_ms - s_down_since_ms) >= (int32_t)AK_WIFI_ROAM_GRACE_MS) {
            PR_NOTICE("ak_wifi: link is down; trying the saved networks");
            roam_report_connecting();
            (void)netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_CLOSE, NULL);
            s_roam             = ROAM_STOPPING;
            s_roam_deadline_ms = now_ms + AK_WIFI_STOP_MS;
            s_tried            = 0U;
        }
        break;
    case ROAM_STOPPING:
        if ((int32_t)(now_ms - s_roam_deadline_ms) >= 0 && s_scan_state != 1U) {
            scan_start();
            s_roam             = ROAM_SCANNING;
            s_roam_deadline_ms = now_ms + AK_WIFI_SCAN_MAX_MS;
        }
        break;
    case ROAM_SCANNING:
        if (s_scan_state == 2U || (int32_t)(now_ms - s_roam_deadline_ms) >= 0) {
            if (s_scan_state != 2U) {
                s_scan_n = 0U; /* scan never returned: fall back to the most recent network */
            }
            s_scan_state = 0U;
            roam_next(now_ms);
        }
        break;
    case ROAM_TRYING:
        if ((int32_t)(now_ms - s_roam_deadline_ms) >= 0) {
            PR_NOTICE("ak_wifi: saved network \"%s\" did not link up", s_store.rec[s_try_idx].ssid);
            s_tried |= (uint16_t)(1U << s_try_idx);
            roam_next(now_ms);
        }
        break;
    case ROAM_BACKOFF:
        if ((int32_t)(now_ms - s_roam_deadline_ms) >= 0 && s_scan_state != 1U) {
            roam_report_connecting(); /* the next round starts */
            scan_start();
            s_roam             = ROAM_SCANNING;
            s_roam_deadline_ms = now_ms + AK_WIFI_SCAN_MAX_MS;
        }
        break;
    default:
        roam_reset();
        break;
    }
}

void ak_wifi_set_roam_allowed(bool allowed)
{
    s_roam_allowed = allowed;
}

size_t ak_wifi_saved_list(ak_wifi_saved_t out[AK_WIFI_STORE_MAX])
{
    store_load();
    uint8_t order[AK_WIFI_STORE_MAX];
    size_t  n = ak_wifi_store_by_recency(&s_store, order);
    for (size_t i = 0U; i < n; i++) {
        const ak_wifi_rec_t *r = &s_store.rec[order[i]];
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].ssid, r->ssid, sizeof(out[i].ssid));
        out[i].sec     = r->sec;
        out[i].current = s_current_state == AK_WIFI_CONNECTED && strcmp(r->ssid, s_current_ssid) == 0;
    }
    return n;
}

uint32_t ak_wifi_saved_revision(void)
{
    return s_revision;
}

/* The deleted network is the one in use: remove the SDK's copy too, so a
 * reboot does not bring it back, and keep the link until it drops. */
static void forget_link(const char *ssid)
{
    if (strcmp(ssid, s_current_ssid) != 0) {
        return;
    }
    (void)ak_kv_del("netinfo");
    if (s_current_state == AK_WIFI_CONNECTED) {
        (void)snprintf(s_forgotten, sizeof(s_forgotten), "%s", ssid);
    } else {
        (void)netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_CLOSE, NULL);
        roam_reset();
        s_down_timing   = true;
        s_down_since_ms = tal_system_get_millisecond() - AK_WIFI_ROAM_GRACE_MS;
        if (s_attempt.armed) { /* the SDK no longer tries the deleted network */
            ak_wifi_attempt_clear(&s_attempt);
            report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
        }
    }
}

bool ak_wifi_saved_forget(const char *ssid)
{
    store_load();
    if (ssid == NULL || !ak_wifi_store_remove(&s_store, ssid)) {
        return false;
    }
    store_save();
    forget_link(ssid);
    PR_NOTICE("ak_wifi: forgot saved network \"%s\"", ssid);
    return true;
}

void ak_wifi_saved_clear(void)
{
    store_load();
    char cur[AK_WIFI_STORE_SSID_MAX + 1U];
    (void)snprintf(cur, sizeof(cur), "%s", s_current_ssid);
    ak_wifi_store_clear(&s_store);
    store_save();
    forget_link(cur);
    PR_NOTICE("ak_wifi: forgot all saved networks");
}

static void __netmgr_status_cb(netmgr_type_e type, netmgr_status_e status)
{
    (void)type;
    switch (status) {
    case NETMGR_LINK_UP:
    case NETMGR_LINK_UP_SWITH:
        s_config_disconnect_pending = false;
        s_rollback_armed            = false;
        s_record_deadline_ms        = tal_system_get_millisecond() + 10000U;
        s_record_pending            = true; /* recorded from the app loop, not from this callback */
        ak_wifi_attempt_clear(&s_attempt);
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
            /* The link dropped: the SDK rejoins the same network by itself
             * (netconn_wifi LINKUP -> READY -> connect), except one the user
             * deleted while linked. */
            bool retry = ak_wifi_attempt_down(&s_attempt, s_forgotten[0] == '\0', tal_system_get_millisecond());
            report_status(retry ? AK_WIFI_CONNECTING : AK_WIFI_DISCONNECTED, s_current_ssid);
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
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status) == OPRT_OK &&
        (status == NETMGR_LINK_UP || status == NETMGR_LINK_UP_SWITH)) {
        __netmgr_status_cb(NETCONN_WIFI, status);
    } else {
        /* Not linked yet. On an activated device netconn_wifi_open has just
         * started joining the saved network (netinfo); say so instead of
         * "disconnected" until it links up or the attempt runs out. */
        tuya_iot_client_t *client = tuya_iot_client_get();
        bool               trying = ak_cloud_network_ready() && client != NULL && client->is_activated &&
                      s_current_ssid[0] != '\0';
        report_status(ak_wifi_attempt_down(&s_attempt, trying, tal_system_get_millisecond()) ? AK_WIFI_CONNECTING
                                                                                              : AK_WIFI_DISCONNECTED,
                      s_current_ssid);
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
    if (!ak_cloud_network_ready()) {
        /* netmgr never started (no product ID, ak_cloud_init stopped early). */
        PR_WARN("ak_wifi: network stack is off (no product ID?); Wi-Fi not applied");
        report_status(AK_WIFI_FAILED, ssid);
        return;
    }
    (void)netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status_before);

    /* A command from the user ends any automatic reconnect round, and
     * re-adding a network that was deleted while linked makes it a normal one. */
    roam_reset();
    ak_wifi_attempt_clear(&s_attempt); /* this connect reports its own end */
    s_forgotten[0] = '\0';

    /* Keep the current configuration so a wrong password cannot strand the
     * device offline. Only a previously used network is worth restoring. */
    s_rollback_armed = false;
    memset(&s_rollback_info, 0, sizeof(s_rollback_info));
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &s_rollback_info) != OPRT_OK ||
        s_rollback_info.ssid[0] == '\0') {
        memset(&s_rollback_info, 0, sizeof(s_rollback_info));
    }

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
    if (s_rollback_info.ssid[0] != '\0') {
        s_rollback_deadline_ms = tal_system_get_millisecond() + AK_WIFI_ROLLBACK_MS;
        s_rollback_armed       = true;
    }
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

static void rollback_tick(uint32_t now_ms)
{
    if (!s_rollback_armed || (int32_t)(s_rollback_deadline_ms - now_ms) > 0) {
        return;
    }
    s_rollback_armed = false;
    PR_WARN("ak_wifi: new network did not link up in %u s; restoring the previous one",
            (unsigned)(AK_WIFI_ROLLBACK_MS / 1000U));
    netmgr_status_e status = NETMGR_LINK_DOWN;
    (void)netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_STATUS, &status);
    bool linked = status == NETMGR_LINK_UP || status == NETMGR_LINK_UP_SWITH;
    if (__netconn_wifi_info_set(&s_rollback_info) == OPRT_OK &&
        netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_SSID_PSWD, &s_rollback_info) == OPRT_OK) {
        if (linked) {
            s_config_disconnect_pending = true;
            (void)tal_wifi_station_disconnect();
        }
        report_status(AK_WIFI_RESTORED, s_rollback_info.ssid);
    } else {
        PR_ERR("ak_wifi: restoring the previous network failed");
        report_status(AK_WIFI_FAILED, s_current_ssid);
    }
    memset(&s_rollback_info, 0, sizeof(s_rollback_info));
}

void ak_wifi_tick(uint32_t now_ms)
{
    rollback_tick(now_ms);
    roam_tick(now_ms);
    if (ak_wifi_attempt_expired(&s_attempt, now_ms) && s_current_state == AK_WIFI_CONNECTING) {
        PR_NOTICE("ak_wifi: no link after %u s; showing disconnected (the SDK keeps retrying)",
                  (unsigned)(AK_WIFI_CONNECTING_UI_MS / 1000U));
        report_status(AK_WIFI_DISCONNECTED, s_current_ssid);
    }
}

ak_cred_status_t ak_cred_read_status(void)
{
    /* tuya_authorize_read points into SDK-owned buffers; only booleans leave
     * this function. */
    tuya_iot_license_t lic = {0};
    ak_cred_status_t   st;
    if (tuya_authorize_read(&lic) != OPRT_OK) {
        st = ak_cred_classify(ak_pid_get(), NULL, NULL);
    } else {
        st = ak_cred_classify(ak_pid_get(), lic.uuid, lic.authkey);
    }
    st.pid = st.pid || ak_pid_pending_restart(); /* written, applies after the restart */
    return st;
}

/* Save the License being replaced: 1-byte UUID length, UUID, AuthKey. */
static OPERATE_RET license_backup_current(void)
{
    tuya_iot_license_t lic = {0};
    if (tuya_authorize_read(&lic) != OPRT_OK || !ak_license_uuid_valid(lic.uuid) ||
        !ak_license_authkey_valid(lic.authkey)) {
        return OPRT_OK; /* nothing valid to keep */
    }
    uint8_t blob[1U + AK_LICENSE_UUID_LEN + AK_LICENSE_KEY_LEN];
    size_t  uuid_len = strlen(lic.uuid);
    blob[0]          = (uint8_t)uuid_len;
    memcpy(&blob[1], lic.uuid, uuid_len);
    memcpy(&blob[1U + uuid_len], lic.authkey, AK_LICENSE_KEY_LEN);
    OPERATE_RET rt = ak_kv_set(AK_LICENSE_BACKUP_KEY, blob, 1U + uuid_len + AK_LICENSE_KEY_LEN);
    memset(blob, 0, sizeof(blob));
    return rt;
}

ak_license_result_t ak_license_write(const char *uuid, const char *authkey)
{
    if (!ak_license_uuid_valid(uuid) || !ak_license_authkey_valid(authkey)) {
        return AK_LICENSE_REJECTED;
    }
    /* FW-02: never lose the factory License silently. The caller has already
     * required a device-side overwrite permission when one exists. */
    if (ak_license_present()) {
        OPERATE_RET bk = license_backup_current();
        if (bk != OPRT_OK) {
            PR_ERR("ak_license: backup of the current License failed: %d; not overwriting", bk);
            return AK_LICENSE_FAILED;
        }
        PR_NOTICE("ak_license: previous License backed up");
    }
    OPERATE_RET rt = tuya_authorize_write(uuid, authkey);
    if (rt != OPRT_OK) {
        PR_ERR("ak_license: tuya_authorize_write failed: %d", rt);
        return AK_LICENSE_FAILED;
    }
    PR_NOTICE("ak_license: UUID/AuthKey written to storage; reboot to apply");
    s_license_present_cache = 1;
    return AK_LICENSE_WRITTEN_REBOOT_REQUIRED;
}

bool ak_license_present(void)
{
    if (s_license_present_cache >= 0) {
        return s_license_present_cache == 1;
    }
    ak_cred_status_t st      = ak_cred_read_status();
    bool             present = st.uuid && st.authkey;
    s_license_present_cache  = present ? 1 : 0;
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

void ak_wifi_tick(uint32_t now_ms)
{
    (void)now_ms;
}

/* No Wi-Fi stack in this profile: nothing is remembered or reconnected. */
void ak_wifi_set_roam_allowed(bool allowed)
{
    (void)allowed;
}

size_t ak_wifi_saved_list(ak_wifi_saved_t out[AK_WIFI_STORE_MAX])
{
    (void)out;
    return 0U;
}

uint32_t ak_wifi_saved_revision(void)
{
    return 0U;
}

bool ak_wifi_saved_forget(const char *ssid)
{
    (void)ssid;
    return false;
}

void ak_wifi_saved_clear(void)
{
}

ak_cred_status_t ak_cred_read_status(void)
{
    return ak_cred_classify(ak_pid_known() ? ak_pid_get() : NULL, NULL, NULL);
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

void ak_wifi_net_info(ak_wifi_net_info_t *out)
{
    memset(out, 0, sizeof(*out));
    /* The station MAC exists whether or not the link is up. */
    NW_MAC_S mac = {0};
    if (tal_wifi_get_mac(WF_STATION, &mac) == OPRT_OK) {
        bool nonzero = false, not_ff = false;
        for (size_t i = 0U; i < sizeof(mac.mac); i++) {
            nonzero = nonzero || mac.mac[i] != 0U;
            not_ff  = not_ff || mac.mac[i] != 0xFFU;
        }
        if (nonzero && not_ff) {
            memcpy(out->mac, mac.mac, sizeof(out->mac));
            out->mac_valid = true;
        }
    }
#if AK_ASR_BOARD_CLOUD
    NW_IP_S ip = {0};
    if (netmgr_conn_get(NETCONN_WIFI, NETCONN_CMD_IP, &ip) == OPRT_OK && ip.nwipstr[0] != '\0' &&
        strcmp(ip.nwipstr, "0.0.0.0") != 0) {
        (void)snprintf(out->ip, sizeof(out->ip), "%s", ip.nwipstr);
        out->ip_valid = true;
        int8_t rssi   = 0;
        if (tal_wifi_station_get_conn_ap_rssi(&rssi) == OPRT_OK) {
            out->rssi       = rssi;
            out->rssi_valid = true;
        }
    }
#endif
}
