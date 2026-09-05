// firmware/main/notification.h
#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <stdbool.h>
#include <stdint.h>

#define NOTIF_MAX_COUNT    8
#define NOTIF_MAX_ID_LEN   48
#define NOTIF_MAX_PROJ_LEN 32
#define NOTIF_MAX_MSG_LEN  64

typedef struct {
    char id[NOTIF_MAX_ID_LEN];
    char project[NOTIF_MAX_PROJ_LEN];
    char message[NOTIF_MAX_MSG_LEN];
    uint32_t seq;
    bool active;
} notification_t;

typedef struct {
    notification_t items[NOTIF_MAX_COUNT];
    int count;
    uint32_t next_seq;
} notification_store_t;

typedef enum {
    NOTIF_STORE_ERROR = -1,
    NOTIF_STORE_ADDED = 0,
    NOTIF_STORE_UPDATED = 1,
} notification_store_result_t;

void notif_store_init(notification_store_t *store);

// Returns ADDED for a new ID, UPDATED for an existing ID, or ERROR.
// If full, drops oldest to make room and returns ADDED.
notification_store_result_t notif_store_add(
    notification_store_t *store, const char *id,
    const char *project, const char *message);

// Returns 0 if found and removed, -1 if not found (idempotent).
int notif_store_dismiss(notification_store_t *store, const char *id);

void notif_store_clear(notification_store_t *store);

int notif_store_count(const notification_store_t *store);

// Returns NULL if index out of range or slot inactive.
const notification_t *notif_store_get(const notification_store_t *store, int index);

#endif // NOTIFICATION_H
