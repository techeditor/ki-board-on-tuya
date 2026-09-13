/**
 * @file ak_reset_netcfg.c
 * @brief Three-fast-boot reset ported from FP-3 reset_netcfg.c.
 */

#include "ak_reset_netcfg.h"

#include "ak_features.h"
#include "tal_api.h"

#if AK_ASR_BOARD_CLOUD

#include "tuya_iot.h"

#define AK_RESET_NETCNT_KEY "rst_cnt"
#define AK_RESET_NETCNT_MAX 3U
#define AK_RESET_WINDOW_MS  5000U

static OPERATE_RET reset_count_read(uint8_t *count)
{
    uint8_t *read_buf = NULL;
    size_t   read_len = 0;
    OPERATE_RET rt;

    if (count == NULL) {
        return OPRT_INVALID_PARM;
    }
    *count = 0;
    rt = tal_kv_get(AK_RESET_NETCNT_KEY, &read_buf, &read_len);
    if (rt == OPRT_OK && read_buf != NULL && read_len > 0) {
        *count = read_buf[0];
    }
    if (read_buf != NULL) {
        tal_kv_free(read_buf);
    }
    return rt;
}

static OPERATE_RET reset_count_write(uint8_t count)
{
    return tal_kv_set(AK_RESET_NETCNT_KEY, &count, sizeof(count));
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

OPERATE_RET ak_reset_netcfg_start(void)
{
    OPERATE_RET rt = OPRT_OK;
    uint8_t count = 0;
    TIMER_ID timer = NULL;

    /* A missing counter is the normal first-boot case. */
    (void)reset_count_read(&count);
    if (count < UINT8_MAX) {
        count++;
    }
    TUYA_CALL_ERR_RETURN(reset_count_write(count));
    PR_NOTICE("netcfg reset boot counter: %u/%u", (unsigned)count,
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
    PR_NOTICE("three fast boots detected: clearing Wi-Fi/cloud binding; License is preserved");
    return tuya_iot_reset(tuya_iot_client_get());
}

#else

OPERATE_RET ak_reset_netcfg_start(void)
{
    return OPRT_OK;
}

OPERATE_RET ak_reset_netcfg_check(void)
{
    return OPRT_OK;
}

#endif
