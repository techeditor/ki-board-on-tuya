/**
 * @file ak_cred.c
 * @brief Pure credential classification. See ak_cred.h.
 */

#include "ak_cred.h"

#include <string.h>

#include "ak_validate.h"

/* The public template fills every field with 'x' (UUID: "uuid" + 'x'). A
 * value of that shape was never replaced, so it is not configured. */
static bool is_template_placeholder(const char *s)
{
    const char *p = s;
    if (strncmp(p, "uuid", 4) == 0) {
        p += 4;
    }
    if (*p == '\0') {
        return false;
    }
    for (; *p != '\0'; p++) {
        if (*p != 'x') {
            return false;
        }
    }
    return true;
}

static bool pid_valid(const char *pid)
{
    if (pid == NULL || strlen(pid) != AK_CRED_PID_LEN) {
        return false;
    }
    for (const char *c = pid; *c != '\0'; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))) {
            return false;
        }
    }
    return true;
}

ak_cred_status_t ak_cred_classify(const char *pid, const char *uuid, const char *authkey)
{
    ak_cred_status_t st;
    st.pid     = pid_valid(pid) && !is_template_placeholder(pid);
    st.uuid    = ak_license_uuid_valid(uuid) && !is_template_placeholder(uuid);
    st.authkey = ak_license_authkey_valid(authkey) && !is_template_placeholder(authkey);
    return st;
}
