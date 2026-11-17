/**
 * @file ak_rx.h
 * @brief Per-source line assembler + strict JSON field access for inbound
 *        JSONL (PC -> board). Pure, host-testable.
 *
 * Security model (firmware-review FW-01 / FW-06):
 *   - every transport owns its own reader, and each delivered line carries the
 *     source it was assembled from, so bytes from two links can never be
 *     spliced into one line;
 *   - a line is accepted only if it is exactly one well-formed JSON object;
 *   - fields are looked up as members of one object only (never by substring
 *     search), duplicate keys reject the object, escapes are fully decoded,
 *     and a value that does not fit its buffer or exceeds its range is an
 *     error, never silently truncated or wrapped.
 */

#ifndef AK_RX_H
#define AK_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest accepted line (bytes, excluding '\n'). Sized for the largest
 * Companion frame (display: chat_slot + 31 B agent + 15 B state + 127 B text,
 * with JSON escapes and envelope) with headroom. Longer lines are dropped. */
#define AK_RX_LINE_MAX 512U

/* BLE lines are sealed by ak_ble_sec: '~' + base64(ctr 8 + plaintext + tag 16).
 * The BLE reader accepts lines long enough to carry a plaintext frame of
 * AK_RX_LINE_MAX - 1 bytes (717 chars); UART keeps AK_RX_LINE_MAX. */
#define AK_RX_SEALED_LINE_MAX 720U

/* Longest accepted frame type name, including the terminator. */
#define AK_RX_TYPE_MAX 24U

typedef enum {
    AK_RX_SRC_UART = 0, /* UART0 / USB: the physically attached Companion */
    AK_RX_SRC_BLE,      /* BLE GATT command characteristic */
} ak_rx_source_t;

typedef void (*ak_rx_line_cb)(const char *line, ak_rx_source_t source);

typedef struct {
    char           buf[AK_RX_SEALED_LINE_MAX];
    size_t         len;
    size_t         cap;           /* buffer bytes for this source, incl. NUL */
    bool           discard;       /* current line is oversized or contains NUL */
    ak_rx_source_t source;        /* fixed at init; stamped on every line */
    uint32_t       dropped_lines; /* oversized / NUL lines dropped so far */
} ak_rx_reader_t;

void ak_rx_reader_init(ak_rx_reader_t *reader, ak_rx_source_t source);

/** Drop any partial line (link went down or was handed over). */
void ak_rx_reader_reset(ak_rx_reader_t *reader);

/**
 * Feed raw bytes received on this reader's source. For each complete
 * '\n'-terminated line, invokes cb with a NUL-terminated string (CR/LF
 * stripped) and the reader's source. Oversized lines and lines containing a
 * NUL byte are dropped whole (never truncated-and-delivered).
 */
void ak_rx_reader_feed(ak_rx_reader_t *reader, const uint8_t *data, size_t n, ak_rx_line_cb cb);

/* ───────────────────────── Strict JSON access ───────────────────────── */

typedef enum {
    AK_JSON_OK = 0,
    AK_JSON_ABSENT,  /* key is not a member of the object */
    AK_JSON_INVALID, /* malformed, wrong type, duplicate, out of range, or does not fit */
} ak_json_rc_t;

/** True when `json` is exactly one well-formed JSON object (whitespace allowed). */
bool ak_json_is_object(const char *json);

/**
 * Read member `key` of the object at `obj` as a string. Escapes (including
 * \uXXXX and surrogate pairs) are decoded to UTF-8; \u0000 is rejected.
 * Returns AK_JSON_INVALID when the value is not a string or does not fit
 * `out` (out is then set to "").
 */
ak_json_rc_t ak_json_get_str(const char *obj, const char *key, char *out, size_t out_size);

/**
 * Like ak_json_get_str, but a value longer than `out` is clipped at a UTF-8
 * character boundary instead of rejected. Only for display-only text (LCD
 * labels) where clipping is harmless; never for identifiers or credentials.
 */
ak_json_rc_t ak_json_get_str_clip(const char *obj, const char *key, char *out, size_t out_size);

/**
 * Read member `key` as an unsigned integer in [0, max]. Only plain decimal
 * digits are accepted (no sign, fraction, exponent or leading zeros).
 */
ak_json_rc_t ak_json_get_uint(const char *obj, const char *key, uint32_t max, uint32_t *out);

/** Point *member at member `key`, which must itself be an object. */
ak_json_rc_t ak_json_get_object(const char *obj, const char *key, const char **member);

/** Read member `key` as a JSON boolean (`true` / `false` only). */
ak_json_rc_t ak_json_get_bool(const char *obj, const char *key, bool *out);

/** Point *member at member `key`, which must itself be an array. */
ak_json_rc_t ak_json_get_array(const char *obj, const char *key, const char **member);

/**
 * Point *elem at element `index` (0-based) of the array at `arr` (as returned
 * by ak_json_get_array). AK_JSON_ABSENT when the array has fewer elements,
 * AK_JSON_INVALID when `arr` is not a well-formed array.
 */
ak_json_rc_t ak_json_array_at(const char *arr, size_t index, const char **elem);

/* ───────────────────────── Frame envelope ───────────────────────── */

typedef struct {
    char        type[AK_RX_TYPE_MAX];
    uint32_t    seq;     /* 0 when absent */
    const char *payload; /* the payload object, or "{}" when absent */
} ak_rx_frame_t;

/**
 * Parse the protocol envelope of one line: the line must be one JSON object
 * with `"v":1`, a top-level string `type` of [a-z0-9_], an optional top-level
 * `seq`, and an optional `payload` object. Only the top-level `type` counts;
 * a `type` nested in the payload is ordinary data. Returns false on any
 * violation; the frame must then be dropped.
 */
bool ak_rx_parse_frame(const char *line, ak_rx_frame_t *frame);

/**
 * True for commands that change provisioning state or expose credentials
 * (wifi, license, license_read, reset_netcfg, ble_pair). They are accepted only from
 * the UART source; see ak_rx_source_allows.
 */
bool ak_rx_type_is_sensitive(const char *type);

/** Source policy: sensitive commands only from UART; everything else from any source. */
bool ak_rx_source_allows(const char *type, ak_rx_source_t source);

#endif /* AK_RX_H */
