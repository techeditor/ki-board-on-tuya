/**
 * @file test_phase2.c
 * @brief Host unit tests for the pure P2-1 modules: notification store
 *        (app_notification) and the input->intent mapper (input_map).
 *        No SDK or hardware required.
 */

#include <stdio.h>
#include <string.h>

#include "app_notification.h"
#include "input_map.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                                   \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---------------- notification store ---------------- */

static void test_notif_add_dismiss_clear(void)
{
    printf("test_notif_add_dismiss_clear\n");
    notification_store_t store;
    notif_store_init(&store);

    CHECK(notif_store_count(&store) == 0, "empty on init");
    CHECK(notif_store_add(&store, "a", "proj", "m1") == NOTIF_STORE_ADDED, "add a");
    CHECK(notif_store_add(&store, "b", "proj", "m2") == NOTIF_STORE_ADDED, "add b");
    CHECK(notif_store_count(&store) == 2, "count 2");

    /* Same id updates in place, no count change. */
    CHECK(notif_store_add(&store, "a", "proj", "m1b") == NOTIF_STORE_UPDATED, "update a");
    CHECK(notif_store_count(&store) == 2, "count still 2");

    CHECK(notif_store_dismiss(&store, "a") == 0, "dismiss a");
    CHECK(notif_store_count(&store) == 1, "count 1");
    CHECK(notif_store_dismiss(&store, "a") == -1, "dismiss missing is idempotent");

    notif_store_clear(&store);
    CHECK(notif_store_count(&store) == 0, "cleared");
}

static void test_notif_full_drops_oldest(void)
{
    printf("test_notif_full_drops_oldest\n");
    notification_store_t store;
    notif_store_init(&store);

    char id[8];
    for (int i = 0; i < NOTIF_MAX_COUNT; i++) {
        snprintf(id, sizeof(id), "n%d", i);
        notif_store_add(&store, id, "p", "m");
    }
    CHECK(notif_store_count(&store) == NOTIF_MAX_COUNT, "full");

    /* Adding one more must not overflow and must drop the oldest ("n0"). */
    CHECK(notif_store_add(&store, "extra", "p", "m") == NOTIF_STORE_ADDED, "add when full");
    CHECK(notif_store_count(&store) == NOTIF_MAX_COUNT, "count capped");
    CHECK(notif_store_dismiss(&store, "n0") == -1, "oldest n0 was dropped");
    CHECK(notif_store_dismiss(&store, "extra") == 0, "newest extra present");
}

/* ---------------- input mapper ---------------- */

static ak_event_t map_key(const char *id, ak_input_event_t ev, uint32_t held, int *ok)
{
    ak_event_t out;
    *ok = input_map_translate(false, id, ev, held, &out) ? 1 : 0;
    return out;
}

static ak_event_t map_nav(const char *id, ak_input_event_t ev, uint32_t held, int *ok)
{
    ak_event_t out;
    *ok = input_map_translate(true, id, ev, held, &out) ? 1 : 0;
    return out;
}

static void test_input_map(void)
{
    printf("test_input_map\n");
    int        ok;
    ak_event_t e;

    /* Short press = UP with held < long threshold. */
    e = map_key("k1", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_STATUS_CYCLE && e.dir == +1, "k1 short -> status +1");

    e = map_key("k2", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_NOTIF_ADD, "k2 short -> notif add");

    e = map_key("k3", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_NOTIF_DISMISS_LATEST, "k3 short -> dismiss");

    e = map_key("k3", AK_INPUT_EVENT_LONG, 900, &ok);
    CHECK(ok && e.type == AK_EVT_NOTIF_CLEAR, "k3 long -> clear");

    e = map_key("k4", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_CONNECT_TOGGLE, "k4 short -> connect toggle");

    e = map_key("k5", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_ONESHOT_DEMO, "k5 short -> oneshot");

    e = map_nav("up", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_ANIM_CYCLE && e.dir == -1, "nav up -> anim -1");

    e = map_nav("down", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_ANIM_CYCLE && e.dir == +1, "nav down -> anim +1");

    e = map_nav("left", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_STATUS_CYCLE && e.dir == -1, "nav left -> status -1");

    e = map_nav("right", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_STATUS_CYCLE && e.dir == +1, "nav right -> status +1");

    e = map_nav("center", AK_INPUT_EVENT_UP, 100, &ok);
    CHECK(ok && e.type == AK_EVT_SLEEP_TOGGLE, "nav center -> sleep toggle");

    /* DOWN never produces an intent; a long hold on a short-only key does not. */
    (void)map_key("k1", AK_INPUT_EVENT_DOWN, 0, &ok);
    CHECK(!ok, "DOWN -> no intent");
    (void)map_key("k1", AK_INPUT_EVENT_UP, 900, &ok);
    CHECK(!ok, "k1 long-hold UP -> no short intent");
}

int main(void)
{
    printf("== phase2 agent_display host tests ==\n");
    test_notif_add_dismiss_clear();
    test_notif_full_drops_oldest();
    test_input_map();

    if (g_failures == 0) {
        printf("ALL PASSED\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
