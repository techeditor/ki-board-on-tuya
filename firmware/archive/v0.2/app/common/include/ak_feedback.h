/**
 * @file ak_feedback.h
 * @brief Probe feedback on UART0 (CH340C -> COM6), JSONL protocol v1.
 *
 * A thin convenience layer over ak_uart (UART0 @115200) + ak_protocol so that
 * bring-up / feature-probe firmware can report status and events on the SAME
 * port the companion / serial monitor watches (COM6), instead of only on the
 * Tuya debug log (UART1) or the on-board LCD.
 *
 * All emitters are best-effort and non-blocking beyond the UART write; a failed
 * init simply makes the emit calls no-ops so a probe never hangs on feedback.
 */

#ifndef AK_FEEDBACK_H
#define AK_FEEDBACK_H

#include <stdint.h>

#include "tuya_cloud_types.h"

/** Open UART0 @115200 8N1 for feedback. Safe to call once at boot. */
OPERATE_RET ak_feedback_init(void);

/** Emit {"type":"status","payload":{"probe","state"}}. */
void ak_feedback_status(const char *probe, const char *state);

/** Shortcut for ak_feedback_status(probe, "ready") — emit at end of boot. */
void ak_feedback_ready(const char *probe);

/** Emit {"type":"log","payload":{"tag","msg"}} for a probe-specific event. */
void ak_feedback_event(const char *tag, const char *msg);

/** Emit {"type":"ping","payload":{"uptime_ms"}} as a liveness heartbeat. */
void ak_feedback_heartbeat(uint32_t uptime_ms);

#endif /* AK_FEEDBACK_H */
