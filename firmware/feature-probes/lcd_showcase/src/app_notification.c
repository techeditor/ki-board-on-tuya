#include "app_notification.h"
#include <string.h>

void notif_store_init(notification_store_t *store) {
    memset(store, 0, sizeof(*store));
}

// Find slot by ID. Returns index or -1.
static int find_by_id(const notification_store_t *store, const char *id) {
    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        if (store->items[i].active && strcmp(store->items[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// Find first inactive slot. Returns index or -1.
static int find_free_slot(const notification_store_t *store) {
    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        if (!store->items[i].active) {
            return i;
        }
    }
    return -1;
}

// Find oldest active slot (lowest seq). Returns index or -1.
static int find_oldest(const notification_store_t *store) {
    int oldest = -1;
    uint32_t min_seq = UINT32_MAX;
    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        if (store->items[i].active && store->items[i].seq < min_seq) {
            min_seq = store->items[i].seq;
            oldest = i;
        }
    }
    return oldest;
}

static void write_slot(notification_t *slot, const char *id,
                        const char *project, const char *message,
                        uint32_t seq) {
    strncpy(slot->id, id, NOTIF_MAX_ID_LEN - 1);
    slot->id[NOTIF_MAX_ID_LEN - 1] = '\0';
    strncpy(slot->project, project, NOTIF_MAX_PROJ_LEN - 1);
    slot->project[NOTIF_MAX_PROJ_LEN - 1] = '\0';
    strncpy(slot->message, message, NOTIF_MAX_MSG_LEN - 1);
    slot->message[NOTIF_MAX_MSG_LEN - 1] = '\0';
    slot->seq = seq;
    slot->active = true;
}

notification_store_result_t notif_store_add(
    notification_store_t *store, const char *id,
    const char *project, const char *message) {
    uint32_t seq = store->next_seq++;

    // Update existing?
    int idx = find_by_id(store, id);
    if (idx >= 0) {
        write_slot(&store->items[idx], id, project, message, seq);
        return NOTIF_STORE_UPDATED;
    }

    // Find free slot
    idx = find_free_slot(store);
    if (idx >= 0) {
        write_slot(&store->items[idx], id, project, message, seq);
        store->count++;
        return NOTIF_STORE_ADDED;
    }

    // Full — drop oldest
    idx = find_oldest(store);
    if (idx >= 0) {
        write_slot(&store->items[idx], id, project, message, seq);
        // count stays the same (replaced one)
        return NOTIF_STORE_ADDED;
    }

    return NOTIF_STORE_ERROR; // Should never happen
}

int notif_store_dismiss(notification_store_t *store, const char *id) {
    int idx = find_by_id(store, id);
    if (idx < 0) return -1;

    memset(&store->items[idx], 0, sizeof(notification_t));
    store->count--;
    return 0;
}

void notif_store_clear(notification_store_t *store) {
    notif_store_init(store);
}

int notif_store_count(const notification_store_t *store) {
    return store->count;
}

const notification_t *notif_store_get(const notification_store_t *store, int index) {
    if (index < 0 || index >= NOTIF_MAX_COUNT) return NULL;
    if (!store->items[index].active) return NULL;
    return &store->items[index];
}
