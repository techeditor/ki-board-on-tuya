/**
 * @file ak_reset_netcfg.c
 * @brief Three-fast-boot reset ported from FP-3 reset_netcfg.c.
 */

#include "ak_reset_netcfg.h"

#include "ak_features.h"
#include "ak_kv.h"
#include "ak_window_marker.h"
#include "tal_api.h"

#if AK_ASR_BOARD_CLOUD

#include "tuya_iot.h"

#define AK_RESET_NETCNT_KEY "rst_cnt"
#define AK_RESET_NETCNT_MAX 3U
#define AK_RESET_WINDOW_MS  5000U

static OPERATE_RET reset_count_read(uint8_t *count)
{
    uint8_t    *read_buf = NULL;
    size_t      read_len = 0;
    OPERATE_RET rt;

    if (count == NULL) {
        return OPRT_INVALID_PARM;
    }
    *count = 0;
    rt = ak_kv_get(AK_RESET_NETCNT_KEY, &read_buf, &read_len);
    if (rt == OPRT_OK) {
        if (read_buf != NULL && read_len == sizeof(*count) &&
            read_buf[0] <= AK_RESET_NETCNT_MAX) {
            *count = read_buf[0];
        } else {
            PR_WARN("netcfg reset boot counter invalid (len=%u); treating as zero",
                    (unsigned)read_len);
            rt = OPRT_COM_ERROR;
        }
    }
    if (read_buf != NULL) {
        ak_kv_free(read_buf);
    }
    return rt;
}

static OPERATE_RET reset_count_write(uint8_t count)
{
    return ak_kv_set(AK_RESET_NETCNT_KEY, &count, sizeof(count));
}

static void reset_count_timer_cb(TIMER_ID timer_id, void *arg)
{
    (void)timer_id;
    (void)arg;
    (void)reset_count_write(0);
    PR_DEBUG("netcfg reset boot counter cleared");
}

static OPERATE_RET reset_event_clear_cb(void *data)
{
    (void)data;
    (void)reset_count_write(0);
    return OPRT_OK;
}

/* One-shot KV marker: "open the pairing window on the next boot". Written
 * only right before a reset or restart the user confirmed on the device
 * (three fast power cycles, reset_netcfg accepted inside an open window, or
 * the pairing-mode confirm). The next boot consumes it, so it cannot persist. */
#define AK_PROV_REOPEN_KEY "ak_prov_rb"

OPERATE_RET ak_reset_netcfg_mark_reopen(void)
{
    uint8_t marker[AK_WINDOW_MARKER_LEN];
    ak_window_marker_encode(marker);
    return ak_kv_set(AK_PROV_REOPEN_KEY, marker, sizeof(marker));
}

bool ak_reset_netcfg_take_reopen(void)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    bool     reopen = false;

    if (ak_kv_get(AK_PROV_REOPEN_KEY, &buf, &len) != OPRT_OK) {
        return false;
    }
    reopen = ak_window_marker_valid(buf, len);
    if (buf != NULL) {
        ak_kv_free(buf);
    }
    /* Consume it either way; a malformed marker counts as absent. */
    (void)ak_kv_del(AK_PROV_REOPEN_KEY);
    return reopen;
}

OPERATE_RET ak_reset_netcfg_start(void)
{
    OPERATE_RET        rt = OPRT_OK;
    uint8_t            count = 0;
    TIMER_ID           timer = NULL;
    char              *reason_text = NULL;
    TUYA_RESET_REASON_E reason = tal_system_get_reset_reason(&reason_text);

    PR_NOTICE("boot reset reason: %d%s%s%s", (int)reason,
              reason_text != NULL ? " (" : "",
              reason_text != NULL ? reason_text : "",
              reason_text != NULL ? ")" : "");

    /* Software, watchdog and fault reboots are not user power cycles. Clear a
     * stale partial sequence so an initialization crash cannot accidentally
     * factory-reset Wi-Fi/cloud state on its third retry. */
    if (reason != TUYA_RESET_REASON_POWERON &&
        reason != TUYA_RESET_REASON_EXTERNAL) {
        (void)reset_count_write(0);
        PR_NOTICE("netcfg reset boot counter ignored for non-physical reset");
        return OPRT_OK;
    }

    /* A missing or malformed counter is the normal first-boot value zero. */
    (void)reset_count_read(&count);
    if (count < AK_RESET_NETCNT_MAX) {
        count++;
    }
    TUYA_CALL_ERR_RETURN(reset_count_write(count));
    PR_NOTICE("netcfg reset physical boot counter: %u/%u", (unsigned)count,
              (unsigned)AK_RESET_NETCNT_MAX);

    TUYA_CALL_ERR_RETURN(tal_sw_timer_create(reset_count_timer_cb, NULL, &timer));
    return tal_sw_timer_start(timer, AK_RESET_WINDOW_MS, TAL_TIMER_ONCE);
}

OPERATE_RET ak_reset_netcfg_check(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint8_t count = 0;

    (void)reset_count_read(&count);
    if (count < AK_RESET_NETCNT_MAX) {
        return OPRT_OK;
    }

    TUYA_CALL_ERR_LOG(tal_event_subscribe(EVENT_RESET, "ak_reset_netcfg",
                                          reset_event_clear_cb, SUBSCRIBE_TYPE_NORMAL));
    TUYA_CALL_ERR_LOG(reset_count_write(0));
    TUYA_CALL_ERR_LOG(ak_reset_netcfg_mark_reopen());
    PR_NOTICE("three fast physical boots detected: clearing Wi-Fi/cloud binding; License is preserved");
    return tuya_iot_reset(tuya_iot_client_get());
}

#else

OPERATE_RET ak_reset_netcfg_start(void)
{
    return OPRT_OK;
}

OPERATE_RET ak_reset_netcfg_mark_reopen(void)
{
    return OPRT_NOT_SUPPORTED;
}

bool ak_reset_netcfg_take_reopen(void)
{
    return false;
}

OPERATE_RET ak_reset_netcfg_check(void)
{
    return OPRT_OK;
}

#endif
