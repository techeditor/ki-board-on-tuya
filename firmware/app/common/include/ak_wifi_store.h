/**
 * @file ak_wifi_store.h
 * @brief Remembered Wi-Fi networks (FR-15): the list, its LRU rule, the stored
 *        blob format and the reconnect order. Pure logic, no SDK calls, so it
 *        is host-testable; ak_wifi.c owns persistence and the radio.
 *
 * - At most AK_WIFI_STORE_MAX networks. A network is recorded after it got an
 *   IP address. Connecting again to a stored SSID updates its password and
 *   moves it to "most recently connected"; a new SSID in a full list replaces
 *   the least recently connected one (LRU).
 * - The blob is versioned and CRC-protected. A blob that fails any check is
 *   discarded as a whole (the list restarts empty).
 * - Passwords stay inside this module's RAM copy and the encrypted KV blob.
 *   Nothing here formats a password for a log or a protocol frame.
 */

#ifndef AK_WIFI_STORE_H
#define AK_WIFI_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AK_WIFI_STORE_MAX       8U /* 2026-11-19: 10 -> 8; docs, Companion text and the Wi-Fi help page follow this */
/* Firmware before that stored up to this many; a stored blob of that size must
 * still be read (and trimmed to AK_WIFI_STORE_MAX), not discarded. */
#define AK_WIFI_STORE_LEGACY_MAX 10U
#define AK_WIFI_STORE_SSID_MAX  32U
#define AK_WIFI_STORE_PASS_MAX  64U
#define AK_WIFI_STORE_VERSION   1U
#define AK_WIFI_STORE_BLOB_MAX  (8U + AK_WIFI_STORE_LEGACY_MAX * (7U + AK_WIFI_STORE_SSID_MAX + AK_WIFI_STORE_PASS_MAX) + 4U)

/* Security type as reported by the scan (WF_AP_AUTH_MODE_E: 0 open, 1 WEP,
 * 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA3); 0xFF when it was never seen. */
#define AK_WIFI_SEC_UNKNOWN 0xFFU

typedef struct {
    char     ssid[AK_WIFI_STORE_SSID_MAX + 1U];
    char     pass[AK_WIFI_STORE_PASS_MAX + 1U];
    uint8_t  sec;
    uint32_t seq; /* last successful connection; larger = more recent */
} ak_wifi_rec_t;

typedef struct {
    ak_wifi_rec_t rec[AK_WIFI_STORE_MAX];
    uint8_t       count;
    uint32_t      next_seq;
} ak_wifi_store_t;

typedef enum {
    AK_WIFI_STORE_INVALID = 0, /* empty or too long SSID, password too long */
    AK_WIFI_STORE_ADDED,
    AK_WIFI_STORE_UPDATED,  /* same SSID: password / security / recency refreshed */
    AK_WIFI_STORE_REPLACED, /* list was full: the least recently connected one is gone */
} ak_wifi_store_result_t;

typedef enum {
    AK_WIFI_BLOB_OK = 0,
    AK_WIFI_BLOB_TRIMMED, /* valid, but held more than AK_WIFI_STORE_MAX: the oldest were dropped; save it back */
    AK_WIFI_BLOB_BAD_FORMAT, /* magic, length or field out of range */
    AK_WIFI_BLOB_BAD_VERSION,
    AK_WIFI_BLOB_BAD_CRC,
} ak_wifi_blob_result_t;

void ak_wifi_store_init(ak_wifi_store_t *s);

/** Index of the SSID, or -1. */
int ak_wifi_store_find(const ak_wifi_store_t *s, const char *ssid);

/** A connection with an IP address succeeded: add or refresh `ssid`. A
 *  `sec` of AK_WIFI_SEC_UNKNOWN keeps a known security type. When the list is
 *  full and the SSID is new, the record with the smallest `seq` is replaced.
 *  If `replaced_ssid` is not NULL it receives the dropped SSID. */
ak_wifi_store_result_t ak_wifi_store_touch(ak_wifi_store_t *s, const char *ssid, const char *pass, uint8_t sec,
                                           char replaced_ssid[AK_WIFI_STORE_SSID_MAX + 1U]);

/** Delete one network. True when it existed. */
bool ak_wifi_store_remove(ak_wifi_store_t *s, const char *ssid);

void ak_wifi_store_clear(ak_wifi_store_t *s);

/** Upgrade path: the single configuration the SDK stored before FR-15
 *  becomes the first record, but only into an empty list. True when added. */
bool ak_wifi_store_migrate_legacy(ak_wifi_store_t *s, const char *ssid, const char *pass);

/** Record indices, most recently connected first. Returns the count. */
size_t ak_wifi_store_by_recency(const ak_wifi_store_t *s, uint8_t out[AK_WIFI_STORE_MAX]);

/** Serialize to `buf` (AK_WIFI_STORE_BLOB_MAX bytes are always enough).
 *  Returns the length, 0 when `cap` is too small. */
size_t ak_wifi_store_serialize(const ak_wifi_store_t *s, uint8_t *buf, size_t cap);

/** Parse a blob. Any failure leaves `s` empty and reports why. A blob with
 *  more than AK_WIFI_STORE_MAX records (older firmware) keeps the most recently
 *  connected ones and reports AK_WIFI_BLOB_TRIMMED. */
ak_wifi_blob_result_t ak_wifi_store_parse(ak_wifi_store_t *s, const uint8_t *buf, size_t len);

/** As ak_wifi_store_parse; when trimming, the record named `keep_ssid` (the
 *  network in use, may be NULL) is kept even when it is among the oldest.
 *  Not reentrant (one static scratch list). */
ak_wifi_blob_result_t ak_wifi_store_parse_keep(ak_wifi_store_t *s, const uint8_t *buf, size_t len,
                                               const char *keep_ssid);

/* ── Reconnect planning ─────────────────────────────────────────────────── */

typedef struct {
    char   ssid[AK_WIFI_STORE_SSID_MAX + 1U];
    int8_t rssi;
    uint8_t sec;
} ak_wifi_scan_t;

/** Stored networks seen in `scan`, strongest signal first (a tie goes to the
 *  more recently connected one). Records whose bit is set in `tried` (bit =
 *  record index) are skipped. Returns how many indices were written. */
size_t ak_wifi_store_plan(const ak_wifi_store_t *s, const ak_wifi_scan_t *scan, size_t scan_n, uint16_t tried,
                          uint8_t *out, size_t cap);

/** Most recently connected record not in `tried`, or -1. Used when the scan
 *  finds none of the stored networks (hidden SSID, scan failure). */
int ak_wifi_store_mru(const ak_wifi_store_t *s, uint16_t tried);

/** Wait before reconnect round `round` (0-based) after every network failed. */
uint32_t ak_wifi_roam_backoff_ms(uint8_t round);

/* ── "Connecting" while the SDK retries on its own ─────────────────────────
 * TuyaOpen's netconn_wifi joins the saved network at boot (activated devices)
 * and rejoins it after a drop, retrying for ever with no event per attempt:
 * netmgr publishes only status changes. So the "connecting" shown for such an
 * attempt ends on link-up, when the roamer takes over, or after
 * AK_WIFI_CONNECTING_UI_MS, whichever comes first; then it reads disconnected.
 * Longer than AK_WIFI_ROAM_GRACE_MS, so a roaming round follows without a gap. */
#define AK_WIFI_CONNECTING_UI_MS 30000U

typedef struct {
    bool     armed;
    uint32_t deadline_ms;
} ak_wifi_attempt_t;

/** The link is down (boot, or it just dropped). `sdk_retrying`: the SDK is
 *  trying a known network by itself. True = show "connecting" (and the
 *  attempt is armed), false = show "disconnected". */
bool ak_wifi_attempt_down(ak_wifi_attempt_t *a, bool sdk_retrying, uint32_t now_ms);

/** Link up, or another owner (a user connect, the roamer) reports from now on. */
void ak_wifi_attempt_clear(ak_wifi_attempt_t *a);

/** True once when an armed attempt ran out: report "disconnected" now. */
bool ak_wifi_attempt_expired(ak_wifi_attempt_t *a, uint32_t now_ms);

#endif /* AK_WIFI_STORE_H */
