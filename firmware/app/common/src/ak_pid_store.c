/**
 * @file ak_pid_store.c
 * @brief Runtime Tuya PID storage (KV) and the effective value. See ak_pid.h.
 *
 * The rules live in ak_pid.c (ak_pid_state_*, host tested); this file only
 * connects them to the device KV.
 */

#include "ak_pid.h"

#include <string.h>

#include "ak_kv.h"
#include "tal_api.h"
#include "tuya_config.h"

/* tuya_iot_init keeps the pointer it is given, so the effective PID lives in
 * a static buffer that is filled once at boot and never changes afterwards. */
static ak_pid_state_t s_state;
static bool           s_loaded;

static int kv_set(const char *key, const uint8_t *value, size_t len, void *ctx)
{
    (void)ctx;
    return ak_kv_set(key, value, len) == OPRT_OK ? 0 : -1;
}

static int kv_get(const char *key, uint8_t *out, size_t cap, size_t *len, void *ctx)
{
    (void)ctx;
    uint8_t *blob = NULL;
    size_t   n    = 0U;
    if (ak_kv_get(key, &blob, &n) != OPRT_OK || blob == NULL) {
        return -1;
    }
    *len = n < cap ? n : cap; /* a longer value is clipped to cap: still not a 16-char PID */
    memcpy(out, blob, *len);
    ak_kv_free(blob);
    return 0;
}

static int kv_del(const char *key, void *ctx)
{
    (void)ctx;
    return ak_kv_del(key) == OPRT_OK ? 0 : -1;
}

static const ak_pid_kv_ops_t k_ops = {kv_set, kv_get, kv_del, NULL};

void ak_pid_load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded         = true;
    ak_pid_src_t src = ak_pid_state_load(&s_state, &k_ops, TUYA_PRODUCT_ID);
    PR_NOTICE("pid: %s", src == AK_PID_SRC_KV      ? "set at runtime (KV)"
                         : src == AK_PID_SRC_BUILD ? "compile-time value"
                                                   : "not set; Tuya cloud stays off until a PID is written over USB");
}

const char *ak_pid_get(void)
{
    return s_state.pid;
}

bool ak_pid_known(void)
{
    return ak_pid_state_known(&s_state);
}

bool ak_pid_pending_restart(void)
{
    return s_state.pid[0] == '\0' && s_state.pending[0] != '\0';
}

ak_pid_verdict_t ak_pid_write(ak_rx_source_t source, const char *candidate)
{
    ak_pid_verdict_t v = ak_pid_state_write(&s_state, &k_ops, source, candidate);
    if (v == AK_PID_WRITE_OK) {
        PR_NOTICE("pid: written and verified; restart to apply");
    } else if (v == AK_PID_WRITE_STORAGE) {
        PR_ERR("pid: saving to KV failed; nothing was locked");
    }
    return v;
}
