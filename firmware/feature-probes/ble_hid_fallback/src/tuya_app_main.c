/**
 * @file tuya_app_main.c
 * @brief FP-6 ble-hid-fallback (T1 spike) — standard BLE HID keyboard bring-up.
 *
 * GOAL (compile+link spike): prove the TuyaOpen T5AI toolchain can build and
 * link a COMPLIANT BLE HID (HOGP / HID-over-GATT) keyboard using the vendored
 * native NimBLE host — the thing Tuya's TAL command-service layer (fixed
 * 0x1910, no descriptor field) cannot express. The spike success bar is
 * "tos.py build => BUILD SUCCESS"; real pairing/typing is verified on hardware
 * by the user later (spec tasks T3/T5).
 *
 * ISOLATION ARCHITECTURE (the crux — see design.md D11):
 *   This file includes ONLY tal_api.h + the plain-C bridge header ak_ble_hid.h.
 *   It NEVER includes a nimble headers (nimble/...). All native NimBLE HOGP code (GATT DB,
 *   Security Manager, advertising, Input Report notify) lives in ak_ble_hid.c,
 *   which in turn includes ONLY nimble headers (nimble/...)s and NEVER a TAL header. Because
 *   the two header worlds never share a translation unit, the D10 spike's two
 *   blockers cannot occur:
 *     #1 LIST_HEAD macro collision (tuya_list.h 1-arg vs tuya_ble_linkqueue.h
 *        2-arg) — neither TU sees both definitions.
 *     #2 host-private tuya_ble_mempool.h missing — resolved by adding
 *        nimble/host to ak_ble_hid.c's include path in the probe CMakeLists
 *        (no SDK edit).
 *
 * SPIKE SCOPING (design D8-R4): host-stack bring-up is delegated to
 * tal_ble_bt_init() — the SAME proven path FP-2 uses — and ak_ble_hid_init()
 * adds the HID service + SM config + native advertising on top inside the
 * TAL_BLE_STACK_INIT event. Replicating tkl_bluetooth.c's private init
 * sequence is left to spec task T2 and would risk the double-init hazard R4
 * warns about. This spike proves the RISKY part — that the native HID GATT +
 * descriptor + SM symbols are present and linkable in the T5AI build.
 *
 * A tiny status LCD shows advertise/connect/paired state so the board is
 * readable without a serial cable, per project convention.
 */

#include <stdio.h>
#include <string.h>

#include "ak_ble_hid.h"
#include "ak_gfx.h"
#include "ak_input.h"
#include "ak_statuslcd.h"
#include "ak_feedback.h"

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tkl_output.h"

#define APP_TASK_STACK_SIZE (6U * 1024U)
#define POLL_INTERVAL_MS    5U
#define BLE_HID_NAME        "AK-KB-HID"

/* ------------------------------------------------------------ state */

static THREAD_HANDLE s_app_thread = NULL;

/* ------------------------------------------------------------ status LCD */

static void draw_status(void)
{
    const char *state;
    uint16_t    bar;

    if (!ak_statuslcd_ready()) {
        return;
    }
    if (ak_ble_hid_ready()) {
        state = "PAIRED";
        bar   = AK_COL_GREEN;
    } else if (ak_ble_hid_connected()) {
        state = "CONNECTED";
        bar   = AK_COL_DKBLUE;
    } else {
        state = "ADVERTISING";
        bar   = AK_COL_ORANGE;
    }
    ak_statuslcd_title("FP-6 ble-hid-fallback", bar);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 4, "HID " BLE_HID_NAME, AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 18, state, AK_COL_YELLOW, AK_COL_BLACK, 2);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 44, "svc 0x1812  appr 0x03C1", AK_COL_CYAN, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_H - 12, "k1=type  k2=Enter  nav=switch", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* ------------------------------------------------------------ BLE events (TAL) */

static void ble_event_cb(TAL_BLE_EVT_PARAMS_T *event)
{
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case TAL_BLE_STACK_INIT:
        if (event->ble_event.init == 0) {
            PR_NOTICE("BLE host up; registering native HID service");
            /* All native NimBLE HOGP setup happens behind the plain-C bridge —
             * this file never touches a nimble header. */
            ak_ble_hid_init();
            ak_feedback_ready("ble_hid_fallback");
            ak_feedback_status("ble_hid_fallback", "advertising");
            draw_status();
        } else {
            PR_ERR("BLE init failed: %d", event->ble_event.init);
        }
        break;
    default:
        /* HID connection/security is handled by the native GAP event callback
         * registered inside ak_ble_hid.c; TAL's peripheral events are unused. */
        break;
    }
}

/* ------------------------------------------------------------ input -> keys */

static void on_input(ak_input_kind_t kind, const char *id, ak_input_event_t event, uint32_t held_ms)
{
    const char *action = ak_input_event_str(event);
    (void)held_ms;

    if (action[0] == '\0') {
        return;
    }
    /* Spike demo mapping (design D6): a key -> send text, another -> Enter. */
    if (kind == AK_INPUT_KIND_KEY) {
        if (strcmp(id, "k1") == 0) {
            ak_ble_hid_send_text("hello kiro");
            ak_feedback_event("hid", "report sent: text 'hello kiro'");
        } else if (strcmp(id, "k2") == 0) {
            ak_ble_hid_send_key(0, 0x28); /* Enter */
            ak_feedback_event("hid", "report sent: Enter");
        } else if (strcmp(id, "k3") == 0) {
            ak_ble_hid_send_key(0, 0x2A); /* Backspace */
            ak_feedback_event("hid", "report sent: Backspace");
        }
    } else if (kind == AK_INPUT_KIND_NAV) {
        if (strcmp(id, "left") == 0 || strcmp(id, "right") == 0) {
            ak_ble_hid_send_key(0x01, 0x2B); /* LeftCtrl+Tab: switch conversation */
            ak_feedback_event("hid", "report sent: Ctrl+Tab (switch)");
        }
    }
    draw_status();
}

/* ------------------------------------------------------------ main */

static void user_main(void)
{
    OPERATE_RET rt;

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 2048, (TAL_LOG_OUTPUT_CB)tkl_log_output);
    PR_NOTICE("======== Agentic Keyboard FP-6: ble-hid-fallback (T1 spike) ========");
    PR_NOTICE("Built: %s %s", __DATE__, __TIME__);

    /* UART0 (COM6) JSONL feedback. Feedback lives only in this TU (which
     * includes tal_api.h); ak_ble_hid.c stays nimble-only per the isolation
     * architecture. */
    ak_feedback_init();

    rt = ak_input_init();
    if (rt != OPRT_OK) {
        PR_ERR("Input init failed: %d", rt);
    }
    rt = ak_statuslcd_init();
    if (rt != OPRT_OK) {
        PR_ERR("Status LCD init failed: %d (continuing headless)", rt);
    }
    draw_status();

    tal_kv_init(&(tal_kv_cfg_t){.seed = "agentic-fp6", .key = "ble-hid-fallback"});
    tal_sw_timer_init();
    tal_workq_init();

    /* Host-stack bring-up delegated to TAL (spike scoping). The native HID
     * service + SM config are added in ble_event_cb on TAL_BLE_STACK_INIT via
     * the plain-C bridge. */
    rt = tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, ble_event_cb);
    if (rt != OPRT_OK) {
        PR_ERR("BLE init failed: %d", rt);
        return;
    }

    while (1) {
        uint32_t now = tal_system_get_millisecond();
        ak_input_poll(now, on_input);

        /* Native GAP/SM events live in ak_ble_hid.c (nimble-only TU), so detect
         * connect/pair transitions by polling the plain-C bridge accessors and
         * report the edge on UART0. */
        static bool     s_was_conn   = false;
        static bool     s_was_paired = false;
        static uint32_t s_last_hb    = 0;
        bool            conn         = ak_ble_hid_connected();
        bool            paired       = ak_ble_hid_ready();
        if (conn != s_was_conn) {
            s_was_conn = conn;
            ak_feedback_event("hid", conn ? "central connected" : "disconnected");
            if (!conn) {
                ak_feedback_status("ble_hid_fallback", "advertising");
            }
            draw_status();
        }
        if (paired != s_was_paired) {
            s_was_paired = paired;
            if (paired) {
                ak_feedback_event("hid", "paired / bonded");
                ak_feedback_status("ble_hid_fallback", "paired");
            }
            draw_status();
        }
        if ((uint32_t)(now - s_last_hb) >= 5000U) {
            s_last_hb = now;
            ak_feedback_heartbeat(now);
        }

        tal_system_sleep(POLL_INTERVAL_MS);
    }
}

static void app_thread(void *arg)
{
    (void)arg;
    user_main();
    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thread_cfg = {
        .stackDepth = APP_TASK_STACK_SIZE,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "fp6_ble_hid",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &thread_cfg);
}
