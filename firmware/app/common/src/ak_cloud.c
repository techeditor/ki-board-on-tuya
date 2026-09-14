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
#include "ak_reset_netcfg.h"
#include "tal_api.h"

#if AK_ASR_BOARD_CLOUD

/* ── Real backend (Tuya IoT client + netmgr) ────────────────────────────────
 * Ported from firmware/feature-probes/tuya_cloud_asr/src/tuya_main.c. Only the
 * IoT bring-up is ported here; K4 capture is routed by the app through the
 * official HOLD mode events in ak_asr.c, and recognized text reaches the app
 * through ak_asr.c's callback. K4 remains owned by ak_input so system/cloud
 * routing stays dynamic and no unconditional cloud hardware button is added. */

#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "netmgr.h"
#include "tuya_authorize.h"
#include "board_com_api.h"
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
#include "ai_audio_player.h"
#endif
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#include "tkl_wifi.h"
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
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
        /* Networking prompt tone (联网提示音) — same as FP-3's BIND_START. */
        ai_audio_player_alert(AI_AUDIO_ALERT_NETWORK_CFG);
#endif
        break;
    case TUYA_EVENT_MQTT_CONNECTED:
        /* No prompt tone here — FP-3 does not play one on MQTT_CONNECTED, and the
         * SDK fires this event on every (re)connect during boot, which caused the
         * "network connected" tone to play twice. Only BIND_START plays a tone. */
        PR_NOTICE("cloud: MQTT connected (device online, cloud ASR ready)");
        break;
    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_NOTICE("cloud: MQTT disconnected (device offline)");
        break;
    case TUYA_EVENT_TIMESTAMP_SYNC:
        tal_event_publish("app.time.sync", NULL);
        break;
    case TUYA_EVENT_RESET:
        /* App-initiated unbind / factory reset requested (matches FP-3). Device
         * application data is cleared by the SDK; nothing extra to persist here. */
        PR_NOTICE("cloud: device reset requested (type=%d)", (int)event->value.asInteger);
        break;
    case TUYA_EVENT_RESET_COMPLETE:
        /* Reset finished — reboot so the device comes back up unprovisioned and
         * re-enters pairing (matches FP-3's tal_system_reset). */
        PR_NOTICE("cloud: device reset complete — rebooting");
        tal_system_reset();
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

    /* FP-3 recovery path: three resets inside five seconds clear only Tuya
     * activation/network data. UUID/AuthKey authorization is stored separately. */
    rt = ak_reset_netcfg_start();
    if (rt != OPRT_OK) {
        PR_WARN("cloud: netcfg reset counter start failed: %d", rt);
    }

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
#if AK_TRANSPORT_BLE_GATT
    /* TAL exposes one BLE callback/advertiser. Tuya BLE netcfg and AK JSONL GATT
     * cannot own it simultaneously, so cloud+AK builds pair through the official
     * SoftAP netcfg path while BLE remains dedicated to the companion. */
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                    &(netcfg_args_t){.type = NETCFG_TUYA_WIFI_AP});
#else
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                    &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
#endif
#endif

    s_iot_ready = true;

    /* Register the board's onboard peripherals — critically the audio codec
     * ("audio_codec"), which ai_chat_init (called later from ak_asr_init) needs
     * for mic capture; without it ai_chat fails with "audio driver audio_codec
     * not exist" and K4 does nothing. Button/LED registration inside are
     * #if defined(BUTTON_NAME/LED_NAME) — the profile leaves those unset, so only
     * audio registers and ak_input / the WS2812 are untouched. Requires
     * CONFIG_ENABLE_AUDIO_CODECS=y in the build profile. */
    OPERATE_RET hw = board_register_hardware();
    if (hw != OPRT_OK) {
        PR_ERR("cloud: board_register_hardware failed: %d (mic may be unavailable)", hw);
    }

    PR_NOTICE("cloud: tuya_iot_init + netmgr + board HW up (PID=%s)", TUYA_PRODUCT_ID);
    return OPRT_OK;
}

/* Dedicated IoT task: tuya_iot_start once, then loop tuya_iot_yield for MQTT
 * keepalive. Kept off the app's input/RX loop so key latency is unaffected. */
static void __iot_task(void *arg)
{
    OPERATE_RET rt = OPRT_OK;
    (void)arg;
    tuya_iot_start(&s_ai_client);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    /* FP-3 keeps Wi-Fi fully awake unless its optional app_lowpower module is
     * enabled; the product does not enable that module. */
    tkl_wifi_set_lp_mode(0, 0);
#endif
    TUYA_CALL_ERR_LOG(ak_reset_netcfg_check());
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
