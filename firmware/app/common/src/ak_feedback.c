/**
 * @file ak_feedback.c
 * @brief Probe feedback on UART0 (COM6) — see ak_feedback.h.
 */

#include "ak_feedback.h"

#include "ak_protocol.h"
#include "ak_uart.h"

/* Monotonic sequence shared across log/heartbeat lines from this probe. */
static uint32_t s_seq;
static BOOL_T   s_ready;

OPERATE_RET ak_feedback_init(void)
{
    OPERATE_RET rt = ak_uart_init();
    s_ready = (rt == OPRT_OK) ? TRUE : FALSE;
    return rt;
}

static void ak_feedback_emit(const char *frame, int length)
{
    if (s_ready && length > 0) {
        ak_uart_write(frame, (uint32_t)length);
    }
}

void ak_feedback_status(const char *probe, const char *state)
{
    char frame[192];
    ak_feedback_emit(frame, ak_proto_status(frame, sizeof(frame), probe, state));
}

void ak_feedback_ready(const char *probe)
{
    ak_feedback_status(probe, "ready");
}

void ak_feedback_event(const char *tag, const char *msg)
{
    char frame[256];
    ak_feedback_emit(frame, ak_proto_log(frame, sizeof(frame), ++s_seq, tag, msg));
}

void ak_feedback_heartbeat(uint32_t uptime_ms)
{
    char frame[128];
    ak_feedback_emit(frame, ak_proto_ping(frame, sizeof(frame), ++s_seq, uptime_ms));
}
