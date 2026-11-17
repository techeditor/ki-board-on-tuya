/**
 * @file ak_kv.c
 * @brief Idempotent, guarded wrapper around TAL KV. See ak_kv.h.
 */

#include "ak_kv.h"

#include "tal_api.h"
#include "tal_kv.h"

static bool        s_tried = false;
static OPERATE_RET s_result = OPRT_COM_ERROR;

OPERATE_RET ak_kv_init(void)
{
    if (s_tried) {
        return s_result;
    }
    s_tried = true;
    /* Same seed/key as before the move, so stored data stays readable.
     * shortcut: these are the TuyaOpen sample values (FW-18); derive per-device
     * values before production. */
    s_result = tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    if (s_result != OPRT_OK) {
        PR_ERR("kv: init failed: %d; settings fall back to defaults", s_result);
    }
    return s_result;
}

bool ak_kv_ready(void)
{
    return s_tried && s_result == OPRT_OK;
}

OPERATE_RET ak_kv_get(const char *key, uint8_t **value, size_t *length)
{
    if (value != NULL) {
        *value = NULL;
    }
    if (length != NULL) {
        *length = 0;
    }
    if (!ak_kv_ready()) {
        return OPRT_COM_ERROR;
    }
    return tal_kv_get(key, value, length);
}

OPERATE_RET ak_kv_set(const char *key, const uint8_t *value, size_t length)
{
    return ak_kv_ready() ? tal_kv_set(key, value, length) : OPRT_COM_ERROR;
}

OPERATE_RET ak_kv_del(const char *key)
{
    return ak_kv_ready() ? tal_kv_del(key) : OPRT_COM_ERROR;
}

void ak_kv_free(uint8_t *value)
{
    if (value != NULL) {
        tal_kv_free(value);
    }
}
