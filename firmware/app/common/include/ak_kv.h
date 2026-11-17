/**
 * @file ak_kv.h
 * @brief The app's only door to TAL KV.
 *
 * TuyaOpen's tal_kv is littlefs-backed: any tal_kv_get/set/del before
 * tal_kv_init dereferences an unmounted filesystem (lfs->cfg == NULL) and
 * MemFaults (seen on hardware as a boot loop right after the banner). The
 * SDK's tal_kv_init is also not idempotent (it creates a new mutex every
 * call). So:
 *   - ak_kv_init() runs once at the very top of user_main, in every build
 *     profile, and later calls (ak_cloud_init) are no-ops;
 *   - every accessor here fails cleanly when the KV is not ready, so callers
 *     fall back to their defaults instead of crashing;
 *   - nothing else in firmware/app calls tal_kv_* (check_boot_order.py).
 */

#ifndef AK_KV_H
#define AK_KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tuya_cloud_types.h"

/** Mount the KV once. Safe to call again; returns the first call's result. */
OPERATE_RET ak_kv_init(void);

bool ak_kv_ready(void);

/** As tal_kv_get; OPRT_COM_ERROR (and *value = NULL) when the KV is not ready. */
OPERATE_RET ak_kv_get(const char *key, uint8_t **value, size_t *length);

OPERATE_RET ak_kv_set(const char *key, const uint8_t *value, size_t length);

OPERATE_RET ak_kv_del(const char *key);

/** Free a buffer from ak_kv_get (NULL is fine). */
void ak_kv_free(uint8_t *value);

#endif /* AK_KV_H */
