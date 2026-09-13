/**
 * @file ak_cloud.c
 * @brief Board-side Tuya IoT/cloud bring-up — stub + gated real impl.
 *
 * See ak_cloud.h. AK_ASR_BOARD_CLOUD selects the implementation at build time.
 * The real path ports the FP-3 (tuya_cloud_asr) IoT init sequence so that
 * ak_asr.c's ai_chat_init() runs against a valid Tuya IoT / AI-service context.
 */

#include "ak_cloud.h"

#include "ak_features.h"
#include "tal_api.h"

#if AK_ASR_BOARD_CLOUD

/* ── Real backend (Tuya IoT client + netmgr) ────────────────────────────────
 * Ported from firmware/feature-probes/tuya_cloud_asr/src/tuya_main.c. Only the
 * IoT bring-up is ported here; capture (K4 push-to-talk) is driven by ak_asr.c
 * via tuya_ai_input_start/stop, and the recognized-text -> `asr` frame path is
 * ak_asr.c's on_text callback (registered by tuya_app_main). No ai_chat_button
 * device is registered — K4 stays owned by ak_input. */

#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "netmgr.h"
#include "tuya_authorize.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif

/* License + PID wiring. The real UUID/AuthKey/PID live in the GITIGNORED header
 * ak_tuya_authkey.h (copy ak_tuya_authkey.h.example -> ak_tuya_authkey.h and
 * fill it, or flash a License so tuya_authorize_read() supplies it at runtime;
 * the TUYA_OPENSDK_* below are only the compile-time fallback). */
#include "tuya_config.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "0.1.0"
#endif

#define AK_CLOUD_TASK_STACK 4096

static tuya_iot_client_t  s_ai_client;
static tuya_iot_license_t s_license;
static THREAD_HANDLE      s_iot_thread = NULL;
static bool               s_iot_ready  = false;

/* netmgr link state -> tuya_iot network_check hook. */
static bool __network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status != NETMGR_LINK_DOWN;
}

/* Minimal Tuya IoT event handler. The keyboard use case only needs online/
 * offline + bind lifecycle for logging; DP reporting / OTA / QR / audio UI from
 * the FP-3 demo are intentionally omitted. */
static void __iot_event_handler(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    (void)client;
    if (event == NULL) {
        return;
    }
    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_NOTICE("cloud: bind start (device awaiting Wi-Fi provisioning)");
        break;
    case TUYA_EVENT_MQTT_CONNECTED:
        PR_NOTICE("cloud: MQTT connected (device online, cloud ASR ready)");
        break;
    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_NOTICE("cloud: MQTT disconnected (device offline)");
        break;
    case TUYA_EVENT_TIMESTAMP_SYNC:
        tal_event_publish("app.time.sync", NULL);
        break;
    default:
        break;
    }
}

OPERATE_RET ak_cloud_init(void)
{
    OPERATE_RET rt;

    /* KV + core services the IoT stack depends on (idempotent on this platform;
     * mirrors the FP-3 user_main init order). */
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();
    tal_time_service_init();
    tuya_authorize_init();

    /* Prefer a flashed License; fall back to the compile-time OPENSDK values. */
    if (OPRT_OK != tuya_authorize_read(&s_license)) {
        s_license.uuid    = TUYA_OPENSDK_UUID;
        s_license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("cloud: no flashed License; using compile-time TUYA_OPENSDK_* fallback. "
                "Flash a real UUID/AuthKey or fill ak_tuya_authkey.h.");
    }

    rt = tuya_iot_init(&s_ai_client, &(const tuya_iot_config_t){
                                         .software_ver  = PROJECT_VERSION,
                                         .productkey    = TUYA_PRODUCT_ID,
                                         .uuid          = s_license.uuid,
                                         .authkey       = s_license.authkey,
                                         .event_handler = __iot_event_handler,
                                         .network_check = __network_check,
                                     });
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tuya_iot_init failed: %d", rt);
        return rt;
    }

    /* Network manager: Wi-Fi (BLE + SoftAP netcfg). */
    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
    netmgr_init(type);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                    &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
#endif

    s_iot_ready = true;
    PR_NOTICE("cloud: tuya_iot_init + netmgr up (PID=%s)", TUYA_PRODUCT_ID);
    return OPRT_OK;
}

/* Dedicated IoT task: tuya_iot_start once, then loop tuya_iot_yield for MQTT
 * keepalive. Kept off the app's input/RX loop so key latency is unaffected. */
static void __iot_task(void *arg)
{
    (void)arg;
    tuya_iot_start(&s_ai_client);
    for (;;) {
        tuya_iot_yield(&s_ai_client);
    }
}

OPERATE_RET ak_cloud_start(void)
{
    if (!s_iot_ready) {
        PR_ERR("cloud: ak_cloud_start called before a successful ak_cloud_init");
        return OPRT_COM_ERROR;
    }
    THREAD_CFG_T cfg = {
        .stackDepth = AK_CLOUD_TASK_STACK,
        .priority   = THREAD_PRIO_2,
        .thrdname   = "ak_cloud",
    };
    return tal_thread_create_and_start(&s_iot_thread, NULL, NULL, __iot_task, NULL, &cfg);
}

bool ak_cloud_compiled(void)
{
    return true;
}

#else /* !AK_ASR_BOARD_CLOUD — stub */

OPERATE_RET ak_cloud_init(void)
{
    return OPRT_OK;
}

OPERATE_RET ak_cloud_start(void)
{
    return OPRT_OK;
}

bool ak_cloud_compiled(void)
{
    return false;
}

#endif /* AK_ASR_BOARD_CLOUD */
