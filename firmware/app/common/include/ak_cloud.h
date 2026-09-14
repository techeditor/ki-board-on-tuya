/**
 * @file ak_cloud.h
 * @brief Board-side Tuya IoT/cloud bring-up for the product firmware.
 *
 * The board-side cloud ASR (ak_asr.c -> ai_chat_init) depends on an initialized
 * Tuya IoT client + AI service context. Calling ai_chat_init WITHOUT that context
 * dereferences a NULL handle and hard-faults at boot (MemFault @ 0x8). This module
 * performs the FP-3 bring-up sequence the AI stack needs, in the required order:
 *
 *     ak_cloud_init()   // tal_kv + authorize + tuya_iot_init + netmgr
 *     ak_asr_init(...)  // ai_chat_init — now has a valid IoT/AI context
 *     ak_cloud_start()  // tuya_iot_start + a dedicated tuya_iot_yield task
 *
 * Gated by AK_ASR_BOARD_CLOUD (ak_features.h):
 *   - 0 (default): compiling STUBS. No IoT/cloud is brought up. v1 UART / v2 BLE
 *     profiles are entirely unaffected.
 *   - 1: the real backend. Requires ENABLE_AI_COMPONENTS (pulled in by the
 *     Kconfig 'select'), a device License (UUID/AuthKey — flashed, or the
 *     compile-time TUYA_OPENSDK_* fallback from the gitignored ak_tuya_authkey.h),
 *     Wi-Fi provisioning, and a PID bound to a Tuya AI agent.
 */

#ifndef AK_CLOUD_H
#define AK_CLOUD_H

#include "tuya_cloud_types.h"

/** Bring up the Tuya IoT client + netmgr (no-op in the stub). MUST be called
 *  BEFORE ak_asr_init() so ai_chat_init has a valid IoT/AI context. Returns
 *  OPRT_OK in the stub. */
OPERATE_RET ak_cloud_init(void);

/** Start the IoT task: tuya_iot_start + a dedicated thread running
 *  tuya_iot_yield (keepalive / MQTT), so the app's input/RX loop is never
 *  blocked. No-op in the stub. Call AFTER ak_asr_init(). */
OPERATE_RET ak_cloud_start(void);

/** True only when netmgr initialized successfully and owns the platform network/BLE stack. */
bool ak_cloud_network_ready(void);

/** True after valid Tuya activation data is loaded or a provisioning flow activates the device. */
bool ak_cloud_activated(void);

/** True only when the real backend is compiled in (AK_ASR_BOARD_CLOUD=1).
 *  The stub returns false so callers can log "cloud off". */
bool ak_cloud_compiled(void);

#endif /* AK_CLOUD_H */
