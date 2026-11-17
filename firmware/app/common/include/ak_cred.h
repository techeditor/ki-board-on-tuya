/**
 * @file ak_cred.h
 * @brief "Is this credential configured?" — booleans only (host-testable).
 *
 * The Credentials page and system_info only ever learn SET / NOT SET. No
 * function here copies, returns or logs a credential value.
 */

#ifndef AK_CRED_H
#define AK_CRED_H

#include <stdbool.h>

#define AK_CRED_PID_LEN 16U

typedef struct {
    bool pid;
    bool uuid;
    bool authkey;
} ak_cred_status_t;

/**
 * Pure: PID = 16 chars of [a-z0-9]; UUID = 20 (or legacy 16) and AuthKey = 32
 * chars of [A-Za-z0-9]. The ak_tuya_authkey.h.example placeholders (all 'x',
 * UUID "uuid" + 'x') count as NOT SET. NULL counts as NOT SET.
 */
ak_cred_status_t ak_cred_classify(const char *pid, const char *uuid, const char *authkey);

/** Reads the PID and the SDK authorization area and classifies them (firmware only). */
ak_cred_status_t ak_cred_read_status(void);

#endif /* AK_CRED_H */
