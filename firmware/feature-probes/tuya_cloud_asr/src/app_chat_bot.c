/**
 * @file app_chat_bot.c
 * @brief app_chat_bot module is used to
 * @version 0.1
 * @date 2025-03-25
 */

#include "tal_api.h"

#include "netmgr.h"

#include "ai_chat_main.h"
#include "app_chat_bot.h"
#include "app_lowpower.h"

/* FP-3: surface ASR text on UART0 as protocol-v1 JSONL (companion/observe). */
#include <string.h>
#include "ak_statuslcd.h"
#include "ak_uart.h"

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "tkl_wifi.h"
#endif

#if defined(ENABLE_PRINTER) && (ENABLE_PRINTER == 1)
#include "app_printer.h"
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define PRINTF_FREE_HEAP_TTIME (10 * 1000)
/* Net-status refresh cadence. Under idle low-power, stretch it way out: in ULP the screen is
   usually suspended and a 1s timer would keep waking the CPU for a refresh nobody sees. */
#if defined(ENABLE_APP_LOWPOWER) && (ENABLE_APP_LOWPOWER == 1)
#define DISP_NET_STATUS_TIME (30 * 1000)
#else
#define DISP_NET_STATUS_TIME (1 * 1000)
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************const declaration********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/
static TIMER_ID sg_printf_heap_tm;

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static AI_UI_WIFI_STATUS_E sg_wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
static TIMER_ID            sg_disp_status_tm;
#endif

/***********************************************************
***********************function define**********************
***********************************************************/
#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
extern void app_ui_action_register(void);
#endif

static void __printf_free_heap_tm_cb(TIMER_ID timer_id, void *arg)
{
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    uint32_t free_heap       = tal_system_get_free_heap_size();
    uint32_t free_psram_heap = tal_psram_get_free_heap_size();
    PR_INFO("Free heap size:%d, Free psram heap size:%d", free_heap, free_psram_heap);
#else
    uint32_t free_heap = tal_system_get_free_heap_size();
    PR_INFO("Free heap size:%d", free_heap);
#endif
}

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
static void __display_net_status_update(void)
{
    AI_UI_WIFI_STATUS_E wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    netmgr_status_e     net_status  = NETMGR_LINK_DOWN;

    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &net_status);
    if (net_status == NETMGR_LINK_UP) {
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
        // get rssi
        int8_t rssi = 0;
#ifndef PLATFORM_T5
        // BUG: Getting RSSI causes a crash on T5 platform
        tkl_wifi_station_get_conn_ap_rssi(&rssi);
#endif
        if (rssi >= -60) {
            wifi_status = AI_UI_WIFI_STATUS_GOOD;
        } else if (rssi >= -70) {
            wifi_status = AI_UI_WIFI_STATUS_FAIR;
        } else {
            wifi_status = AI_UI_WIFI_STATUS_WEAK;
        }
#else
        wifi_status = AI_UI_WIFI_STATUS_GOOD;
#endif
    } else {
        wifi_status = AI_UI_WIFI_STATUS_DISCONNECTED;
    }

    if (wifi_status != sg_wifi_status) {
        sg_wifi_status = wifi_status;
        ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&wifi_status, sizeof(AI_UI_WIFI_STATUS_E));
    }
}

static void __display_status_tm_cb(TIMER_ID timer_id, void *arg)
{
    __display_net_status_update();
}

#endif

/* ------------------------------------------------------------ FP-3 status LCD */
static char     s_fp3_asr[80] = "(waiting for speech)";
static uint32_t s_fp3_asr_ms  = 0;
static uint32_t s_fp3_count   = 0;
static TIMER_ID s_fp3_lcd_tm  = NULL;

static bool __fp3_net_up(void)
{
    netmgr_status_e st = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &st);
    return st == NETMGR_LINK_UP;
}

static void __fp3_draw_lcd(void)
{
    char line[48];
    bool up;

    if (!ak_statuslcd_ready()) {
        return;
    }
    up = __fp3_net_up();
    ak_statuslcd_title("FP-3 cloud ASR", up ? AK_COL_GREEN : AK_COL_ORANGE);

    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 4, "Hold button = talk", AK_COL_CYAN, AK_COL_BLACK, 2);
    (void)snprintf(line, sizeof(line), "Net: %s", up ? "UP" : "DOWN (pair WiFi)");
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 30, line, up ? AK_COL_GREEN : AK_COL_RED, AK_COL_BLACK, 1);

    (void)snprintf(line, sizeof(line), "ASR #%u  %ums", (unsigned)s_fp3_count, (unsigned)s_fp3_asr_ms);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 48, line, AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_gfx_text(6, AK_STATUSLCD_CONTENT_Y + 66, s_fp3_asr, AK_COL_YELLOW, AK_COL_BLACK, 2);

    ak_gfx_text(6, AK_STATUSLCD_H - 14, "full UTF-8 text on UART0 JSONL", AK_COL_GRAY, AK_COL_BLACK, 1);
    ak_statuslcd_flush();
}

/* Copy ASR text into the status buffer, ASCII best-effort: the 8x8 status font
   is ASCII-only, so CJK bytes render as '.'; the full UTF-8 text still goes out
   on UART0 JSONL. Enough to judge on-board that ASR fired and (for English) what. */
static void __fp3_set_asr_text(const AI_NOTIFY_TEXT_T *t)
{
    uint32_t i, n = 0;

    if (t == NULL || t->data == NULL) {
        return;
    }
    for (i = 0; i < t->datalen && n < sizeof(s_fp3_asr) - 1; i++) {
        unsigned char c = (unsigned char)t->data[i];
        s_fp3_asr[n++]  = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    s_fp3_asr[n] = '\0';
    s_fp3_asr_ms = t->timeindex;
    s_fp3_count++;
}

static void __fp3_lcd_tm_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    __fp3_draw_lcd();
}

static void __fp3_emit_asr(const AI_NOTIFY_TEXT_T *t)
{
    /* {"v":1,"type":"asr","payload":{"text":"<escaped>","ms":<timeindex>}} */
    char     buf[512];
    int      n = 0;
    uint32_t i;

    if (t == NULL || t->data == NULL) {
        return;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "{\"v\":1,\"type\":\"asr\",\"payload\":{\"text\":\"");
    for (i = 0; i < t->datalen && n < (int)sizeof(buf) - 8; i++) {
        char c = t->data[i];
        if (c == '"' || c == '\\') {
            buf[n++] = '\\';
            buf[n++] = c;
        } else if ((unsigned char)c < 0x20) {
            buf[n++] = ' ';
        } else {
            buf[n++] = c;
        }
    }
    n += snprintf(buf + n, sizeof(buf) - n, "\",\"ms\":%u}}\n", (unsigned)t->timeindex);
    ak_uart_write(buf, (uint32_t)n);
}

static void __ai_chat_handle_event(AI_NOTIFY_EVENT_T *event)
{
    app_lowpower_feed_event(event); // power mgmt: activity / key-mode gate

    switch (event->type) {
    case AI_USER_EVT_ASR_OK:
        __fp3_emit_asr((const AI_NOTIFY_TEXT_T *)event->data);
        __fp3_set_asr_text((const AI_NOTIFY_TEXT_T *)event->data);
        __fp3_draw_lcd();
        break;
    case AI_USER_EVT_ASR_ERROR: {
        const char *s = "{\"v\":1,\"type\":\"asr\",\"payload\":{\"error\":\"asr_error\"}}\n";
        ak_uart_write(s, (uint32_t)strlen(s));
        strncpy(s_fp3_asr, "<asr error>", sizeof(s_fp3_asr) - 1);
        __fp3_draw_lcd();
    } break;
    case AI_USER_EVT_ASR_EMPTY: {
        const char *s = "{\"v\":1,\"type\":\"asr\",\"payload\":{\"error\":\"asr_empty\"}}\n";
        ak_uart_write(s, (uint32_t)strlen(s));
        strncpy(s_fp3_asr, "<empty>", sizeof(s_fp3_asr) - 1);
        __fp3_draw_lcd();
    } break;
#if defined(ENABLE_PRINTER) && (ENABLE_PRINTER == 1)
    case AI_USER_EVT_GENERATE_PICTURE:
    case AI_USER_EVT_GET_PICTURE_FROM_APP: {
#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
        app_print_img_from_album((const char *)event->data);
#endif
    } break;
#endif
    default:
        break;
    }
}

void app_chat_bot_set_online(BOOL_T online)
{
    app_lowpower_set_online(online);
}

OPERATE_RET app_chat_bot_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    (void)ak_uart_init(); /* FP-3: UART0 P10/P11 for ASR JSONL output */

    if (ak_statuslcd_init() != OPRT_OK) {
        PR_ERR("FP-3 status LCD init failed (continuing headless)");
    }
    __fp3_draw_lcd();

    AI_CHAT_MODE_CFG_T ai_chat_cfg = {
        .default_mode = AI_CHAT_MODE_HOLD, // push-to-talk; idle low-power gates on this mode
        .default_vol  = 70,
        .evt_cb       = __ai_chat_handle_event,
    };
    TUYA_CALL_ERR_RETURN(ai_chat_init(&ai_chat_cfg));

    app_lowpower_init();

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    app_ui_action_register();
#endif

#if defined(ENABLE_COMP_AI_VIDEO) && (ENABLE_COMP_AI_VIDEO == 1)
    TUYA_CALL_ERR_LOG(ai_video_init());
#endif

#if defined(ENABLE_COMP_AI_MCP) && (ENABLE_COMP_AI_MCP == 1)
    TUYA_CALL_ERR_RETURN(ai_mcp_init());
#endif

#if defined(ENABLE_COMP_AI_PICTURE) && (ENABLE_COMP_AI_PICTURE == 1)
    TUYA_CALL_ERR_RETURN(ai_picture_init());
#endif

    // Free heap size
    tal_sw_timer_create(__printf_free_heap_tm_cb, NULL, &sg_printf_heap_tm);
    tal_sw_timer_start(sg_printf_heap_tm, PRINTF_FREE_HEAP_TTIME, TAL_TIMER_CYCLE);

    /* FP-3: refresh the status LCD (net state) every 2s. */
    tal_sw_timer_create(__fp3_lcd_tm_cb, NULL, &s_fp3_lcd_tm);
    tal_sw_timer_start(s_fp3_lcd_tm, 2000, TAL_TIMER_CYCLE);

#if defined(ENABLE_COMP_AI_DISPLAY) && (ENABLE_COMP_AI_DISPLAY == 1)
    ai_ui_disp_msg(AI_UI_DISP_NETWORK, (uint8_t *)&sg_wifi_status, sizeof(AI_UI_WIFI_STATUS_E));

    ai_ui_disp_msg(AI_UI_DISP_STATUS, (uint8_t *)INITIALIZING, strlen(INITIALIZING));
    ai_ui_disp_msg(AI_UI_DISP_EMOTION, (uint8_t *)EMOJI_NEUTRAL, strlen(EMOJI_NEUTRAL));

    // display status update
    tal_sw_timer_create(__display_status_tm_cb, NULL, &sg_disp_status_tm);
    tal_sw_timer_start(sg_disp_status_tm, DISP_NET_STATUS_TIME, TAL_TIMER_CYCLE);
#endif

    return OPRT_OK;
}
