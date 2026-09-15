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

#include <stdio.h>
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
#include "mqtt_bind.h"
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
#define PROJECT_VERSION "0.0.0-dev"
#endif

#define AK_CLOUD_TASK_STACK 4096

static tuya_iot_client_t  s_ai_client;
static tuya_iot_license_t s_license;
static THREAD_HANDLE      s_iot_thread      = NULL;
static bool               s_iot_ready       = false;
static bool               s_netmgr_ready    = false;
static volatile bool      s_cloud_activated = false;
static volatile bool      s_provisioning    = false;
static ak_cloud_activation_cb s_activation_callback = NULL;
static char               s_binding_qr_url[160];
static const char        *s_activation_state = "unknown";

void ak_cloud_set_activation_callback(ak_cloud_activation_cb callback)
{
    s_activation_callback = callback;
}

static void notify_activation(const char *state, const char *qr_url)
{
    s_activation_state = state == NULL ? "unknown" : state;
    if (qr_url == NULL || qr_url[0] == '\0') {
        s_binding_qr_url[0] = '\0';
    }
    if (s_activation_callback != NULL) {
        s_activation_callback(s_activation_state, qr_url == NULL ? "" : qr_url);
    }
}

const char *ak_cloud_activation_state(void)
{
    return s_activation_state;
}

const char *ak_cloud_binding_qr_url(void)
{
    return s_binding_qr_url;
}

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
        s_cloud_activated = false;
        s_provisioning    = true;
        notify_activation("pairing", "");
        PR_NOTICE("cloud: bind start (device awaiting Wi-Fi provisioning)");
#if defined(ENABLE_COMP_AI_AUDIO) && (ENABLE_COMP_AI_AUDIO == 1)
        /* Networking prompt tone (联网提示音) — same as FP-3's BIND_START. */
        ai_audio_player_alert(AI_AUDIO_ALERT_NETWORK_CFG);
#endif
        break;
    case TUYA_EVENT_DIRECT_MQTT_CONNECTED:
        /* Official TuyaOpen MQTT-bind flow: Companion renders this URL as a QR
         * code, then SmartLife scanning delivers region/token/regist_key over
         * direct MQTT. Never include AuthKey in the URL or UART0 frame. */
        (void)snprintf(s_binding_qr_url, sizeof(s_binding_qr_url),
                       "https://smartapp.tuya.com/s/p?p=%s&uuid=%s&v=2.0",
                       TUYA_PRODUCT_ID,
                       s_license.uuid == NULL ? "" : s_license.uuid);
        notify_activation("scan_qr", s_binding_qr_url);
        PR_NOTICE("cloud: direct MQTT ready; waiting for SmartLife QR scan");
        break;
    case TUYA_EVENT_BIND_TOKEN_ON:
        notify_activation("token_received", "");
        PR_NOTICE("cloud: binding token received; activating device");
        break;
    case TUYA_EVENT_ACTIVATE_SUCCESSED:
        s_cloud_activated = true;
        s_provisioning    = false;
        notify_activation("activated", "");
        PR_NOTICE("cloud: activation succeeded; AK BLE takeover is now allowed");
        break;
    case TUYA_EVENT_MQTT_CONNECTED:
        notify_activation("cloud_ready", "");
        /* No prompt tone here — FP-3 does not play one on MQTT_CONNECTED, and the
         * SDK fires this event on every (re)connect during boot, which caused the
         * "network connected" tone to play twice. Only BIND_START plays a tone. */
        PR_NOTICE("cloud: MQTT connected (device online, cloud ASR ready)");
        break;
    case TUYA_EVENT_MQTT_DISCONNECT:
        notify_activation("cloud_disconnected", "");
        PR_NOTICE("cloud: MQTT disconnected (device offline)");
        break;
    case TUYA_EVENT_TIMESTAMP_SYNC:
        tal_event_publish("app.time.sync", NULL);
        break;
    case TUYA_EVENT_RESET:
        /* App-initiated unbind / factory reset requested (matches FP-3). Device
         * application data is cleared by the SDK; nothing extra to persist here.
         * Show the pairing hint immediately — the reboot into SoftAP follows. */
        s_cloud_activated = false;
        s_provisioning    = true;
        notify_activation("resetting", "");
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

    s_iot_ready       = false;
    s_netmgr_ready    = false;
    s_cloud_activated = false;

    /* KV + core services the IoT stack depends on. A partial initialization
     * must not be reported as cloud-ready: later netmgr/IoT calls otherwise
     * fail opaquely or spin forever. */
    rt = tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tal_kv_init failed: %d", rt);
        return rt;
    }
    rt = tal_sw_timer_init();
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tal_sw_timer_init failed: %d", rt);
        return rt;
    }
    rt = tal_workq_init();
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tal_workq_init failed: %d", rt);
        return rt;
    }
    rt = tal_time_service_init();
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tal_time_service_init failed: %d", rt);
        return rt;
    }
    rt = tuya_authorize_init();
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tuya_authorize_init failed: %d", rt);
        return rt;
    }

    /* FP-3 recovery path: three physical power cycles inside five seconds clear
     * only Tuya activation/network data. UUID/AuthKey authorization is stored
     * separately. */
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
    s_cloud_activated = tuya_iot_activated(&s_ai_client);
    notify_activation(s_cloud_activated ? "activated" : "pairing", "");

    /* Network manager: Wi-Fi plus Tuya BLE/SoftAP provisioning. AK's own GATT
     * transport must not take over the shared BLE callbacks until activation
     * succeeds; the app enforces that boundary with ak_cloud_activated(). */
    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
    rt = netmgr_init(type);
    if (rt != OPRT_OK) {
        PR_ERR("cloud: netmgr_init failed: %d", rt);
        return rt;
    }
    s_netmgr_ready = true;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    rt = netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                         &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
    if (rt != OPRT_OK) {
        PR_ERR("cloud: configure Tuya BLE/SoftAP netcfg failed: %d", rt);
        return rt;
    }

    /* Wi-Fi's SDK backend normally registers only App-driven BLE/AP token
     * acquisition. Register MQTT bind as a second provider so credentials sent
     * over trusted UART can bring the radio online before cloud activation. */
    rt = tuya_iot_token_get_port_register(&s_ai_client, mqtt_bind_token_get);
    if (rt != OPRT_OK) {
        PR_ERR("cloud: register MQTT bind token provider failed: %d", rt);
        return rt;
    }
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
    OPERATE_RET rt;
    OPERATE_RET last_yield_error = OPRT_OK;
    (void)arg;

    rt = tuya_iot_start(&s_ai_client);
    if (rt != OPRT_OK) {
        PR_ERR("cloud: tuya_iot_start failed: %d; IoT task stopped", rt);
        tal_thread_delete(s_iot_thread);
        s_iot_thread = NULL;
        return;
    }
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    /* FP-3 keeps Wi-Fi fully awake unless its optional app_lowpower module is
     * enabled; the product does not enable that module. */
    tkl_wifi_set_lp_mode(0, 0);
#endif
    rt = ak_reset_netcfg_check();
    if (rt != OPRT_OK) {
        PR_WARN("cloud: netcfg reset check failed: %d", rt);
    }
    for (;;) {
        rt = tuya_iot_yield(&s_ai_client);
        if (rt != OPRT_OK) {
            if (rt != last_yield_error) {
                PR_WARN("cloud: tuya_iot_yield failed: %d; retrying", rt);
                last_yield_error = rt;
            }
            tal_system_sleep(100U);
        } else {
            last_yield_error = OPRT_OK;
        }
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

bool ak_cloud_network_ready(void)
{
    return s_netmgr_ready;
}

bool ak_cloud_activated(void)
{
    return s_cloud_activated;
}

bool ak_cloud_provisioning(void)
{
    return s_provisioning;
}

OPERATE_RET ak_cloud_enter_pairing(void)
{
    if (!s_iot_ready) {
        PR_ERR("cloud: enter_pairing called before a successful ak_cloud_init");
        return OPRT_COM_ERROR;
    }
    /* Clear Wi-Fi/cloud binding and reboot into pairing. The License
     * (UUID/AuthKey) lives in separate authorization storage and is preserved.
     * The SDK fires TUYA_EVENT_RESET then TUYA_EVENT_RESET_COMPLETE, and our
     * handler reboots on complete — the same path as three fast power cycles. */
    s_provisioning = true;
    PR_NOTICE("cloud: companion requested enter-pairing; clearing Wi-Fi/cloud binding (License preserved)");
    return tuya_iot_reset(&s_ai_client);
}

bool ak_cloud_compiled(void)
{
    return true;
}

#else /* !AK_ASR_BOARD_CLOUD — stub */

void ak_cloud_set_activation_callback(ak_cloud_activation_cb callback)
{
    (void)callback;
}

const char *ak_cloud_activation_state(void)
{
    return "disabled";
}

const char *ak_cloud_binding_qr_url(void)
{
    return "";
}

OPERATE_RET ak_cloud_init(void)
{
    return OPRT_OK;
}

OPERATE_RET ak_cloud_start(void)
{
    return OPRT_OK;
}

bool ak_cloud_network_ready(void)
{
    return false;
}

bool ak_cloud_activated(void)
{
    return false;
}

bool ak_cloud_provisioning(void)
{
    return false;
}

OPERATE_RET ak_cloud_enter_pairing(void)
{
    /* No cloud/provisioning in this profile — nothing to reset. */
    return OPRT_NOT_SUPPORTED;
}

bool ak_cloud_compiled(void)
{
    return false;
}

#endif /* AK_ASR_BOARD_CLOUD */
