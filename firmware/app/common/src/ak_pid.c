/**
 * @file ak_pid.c
 * @brief Runtime Tuya PID rules (pure). See ak_pid.h.
 */

#include "ak_pid.h"

#include <string.h>

#include "ak_cred.h"

bool ak_pid_valid(const char *pid)
{
    return ak_cred_classify(pid, NULL, NULL).pid;
}

ak_pid_src_t ak_pid_resolve(const uint8_t *kv, size_t kv_len, const char *build, char out[AK_PID_LEN + 1U])
{
    out[0] = '\0';
    if (kv != NULL && kv_len == AK_PID_LEN) {
        char tmp[AK_PID_LEN + 1U];
        memcpy(tmp, kv, AK_PID_LEN);
        tmp[AK_PID_LEN] = '\0';
        if (ak_pid_valid(tmp)) {
            memcpy(out, tmp, sizeof(tmp));
            return AK_PID_SRC_KV;
        }
    }
    if (ak_pid_valid(build)) {
        memcpy(out, build, AK_PID_LEN + 1U);
        return AK_PID_SRC_BUILD;
    }
    return AK_PID_SRC_NONE;
}

ak_pid_verdict_t ak_pid_check_write(ak_rx_source_t source, const char *candidate, bool have_pid)
{
    if (source != AK_RX_SRC_UART) {
        return AK_PID_WRITE_SOURCE;
    }
    if (have_pid) {
        return AK_PID_WRITE_EXISTS;
    }
    return ak_pid_valid(candidate) ? AK_PID_WRITE_OK : AK_PID_WRITE_INVALID;
}

const char *ak_pid_verdict_str(ak_pid_verdict_t verdict)
{
    switch (verdict) {
    case AK_PID_WRITE_SOURCE:
        return "source";
    case AK_PID_WRITE_EXISTS:
        return "exists";
    case AK_PID_WRITE_INVALID:
        return "invalid";
    case AK_PID_WRITE_STORAGE:
        return "storage";
    case AK_PID_WRITE_OK:
    default:
        return "";
    }
}

ak_pid_src_t ak_pid_state_load(ak_pid_state_t *st, const ak_pid_kv_ops_t *kv, const char *build)
{
    uint8_t blob[AK_PID_LEN + 1U]; /* one byte more than a PID: a longer value is not one */
    size_t  len = 0U;
    memset(st, 0, sizeof(*st));
    if (kv == NULL || kv->get == NULL || kv->get(AK_PID_KV_KEY, blob, sizeof(blob), &len, kv->ctx) != 0) {
        len = 0U;
    }
    return ak_pid_resolve(len > 0U ? blob : NULL, len, build, st->pid);
}

bool ak_pid_state_known(const ak_pid_state_t *st)
{
    return st->pid[0] != '\0' || st->pending[0] != '\0';
}

ak_pid_verdict_t ak_pid_state_write(ak_pid_state_t *st, const ak_pid_kv_ops_t *kv, ak_rx_source_t source,
                                    const char *candidate)
{
    ak_pid_verdict_t v = ak_pid_check_write(source, candidate, ak_pid_state_known(st));
    if (v != AK_PID_WRITE_OK) {
        return v;
    }
    if (kv == NULL || kv->set == NULL || kv->get == NULL) {
        return AK_PID_WRITE_STORAGE;
    }
    if (kv->set(AK_PID_KV_KEY, (const uint8_t *)candidate, AK_PID_LEN, kv->ctx) != 0) {
        return AK_PID_WRITE_STORAGE; /* nothing locked; the key may hold junk, which boot ignores */
    }
    uint8_t back[AK_PID_LEN + 1U];
    size_t  len = 0U;
    if (kv->get(AK_PID_KV_KEY, back, sizeof(back), &len, kv->ctx) != 0 || len != AK_PID_LEN ||
        memcmp(back, candidate, AK_PID_LEN) != 0) {
        if (kv->del != NULL) {
            (void)kv->del(AK_PID_KV_KEY, kv->ctx); /* never leave a wrong value for the next boot */
        }
        return AK_PID_WRITE_STORAGE;
    }
    memcpy(st->pending, candidate, AK_PID_LEN + 1U);
    return AK_PID_WRITE_OK;
}
