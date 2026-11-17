/**
 * @file ak_rx.c
 * @brief Per-source line assembler + strict JSON field access (host-testable).
 */

#include "ak_rx.h"

#include <string.h>

/* Nesting allowed below the frame object (payload values are flat today). */
#define AK_JSON_MAX_DEPTH 8
/* Member names longer than this can never match a key we look up. */
#define AK_JSON_NAME_MAX 32U

/* ───────────────────────── Line assembler ───────────────────────── */

void ak_rx_reader_init(ak_rx_reader_t *reader, ak_rx_source_t source)
{
    reader->len           = 0;
    reader->cap           = source == AK_RX_SRC_BLE ? AK_RX_SEALED_LINE_MAX : AK_RX_LINE_MAX;
    reader->discard       = false;
    reader->source        = source;
    reader->dropped_lines = 0;
}

void ak_rx_reader_reset(ak_rx_reader_t *reader)
{
    reader->len     = 0;
    reader->discard = false;
}

void ak_rx_reader_feed(ak_rx_reader_t *reader, const uint8_t *data, size_t n, ak_rx_line_cb cb)
{
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];

        if (c == '\n') {
            if (reader->discard) {
                reader->dropped_lines++;
            } else if (reader->len > 0) {
                reader->buf[reader->len] = '\0';
                if (cb != NULL) {
                    cb(reader->buf, reader->source);
                }
            }
            reader->len     = 0;
            reader->discard = false;
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\0') {
            reader->discard = true; /* would hide the rest of the line from the parser */
            continue;
        }
        if (reader->len < reader->cap - 1U) {
            reader->buf[reader->len++] = c;
        } else {
            reader->discard = true; /* drop the rest of this oversized line */
        }
    }
}

/* ───────────────────────── JSON scanning ───────────────────────── */

typedef struct {
    char  *out;
    size_t cap;
    size_t len;
    bool   overflow;
} ak_sink_t;

static void sink_put(ak_sink_t *s, const char *bytes, size_t n)
{
    if (s == NULL || s->out == NULL || s->overflow) {
        return;
    }
    if (s->len + n >= s->cap) { /* keep one byte for the terminator */
        s->overflow = true;
        return;
    }
    memcpy(s->out + s->len, bytes, n);
    s->len += n;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static bool hex4(const char *p, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char     c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80U) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800U) {
        out[0] = (char)(0xC0U | (cp >> 6));
        out[1] = (char)(0x80U | (cp & 0x3FU));
        return 2;
    }
    if (cp < 0x10000U) {
        out[0] = (char)(0xE0U | (cp >> 12));
        out[1] = (char)(0x80U | ((cp >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (cp & 0x3FU));
        return 3;
    }
    out[0] = (char)(0xF0U | (cp >> 18));
    out[1] = (char)(0x80U | ((cp >> 12) & 0x3FU));
    out[2] = (char)(0x80U | ((cp >> 6) & 0x3FU));
    out[3] = (char)(0x80U | (cp & 0x3FU));
    return 4;
}

/* Scan a string starting at the opening quote, decoding into sink (may be
 * NULL). Returns the position just past the closing quote, or NULL if the
 * string is malformed. */
static const char *scan_string(const char *p, ak_sink_t *sink)
{
    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p != '"') {
        unsigned char c = (unsigned char)*p;
        if (c == '\0' || c < 0x20U) {
            return NULL; /* unterminated, or raw control character */
        }
        if (c != '\\') {
            sink_put(sink, p, 1);
            p++;
            continue;
        }
        p++; /* backslash */
        char simple = 0;
        switch (*p) {
        case '"':
            simple = '"';
            break;
        case '\\':
            simple = '\\';
            break;
        case '/':
            simple = '/';
            break;
        case 'b':
            simple = '\b';
            break;
        case 'f':
            simple = '\f';
            break;
        case 'n':
            simple = '\n';
            break;
        case 'r':
            simple = '\r';
            break;
        case 't':
            simple = '\t';
            break;
        case 'u': {
            uint32_t cp;
            if (!hex4(p + 1, &cp)) {
                return NULL;
            }
            p += 5;
            if (cp >= 0xD800U && cp <= 0xDBFFU) {
                uint32_t lo;
                if (p[0] != '\\' || p[1] != 'u' || !hex4(p + 2, &lo) || lo < 0xDC00U || lo > 0xDFFFU) {
                    return NULL; /* high surrogate without its low half */
                }
                p += 6;
                cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
            } else if (cp >= 0xDC00U && cp <= 0xDFFFU) {
                return NULL; /* lone low surrogate */
            }
            if (cp == 0U) {
                return NULL; /* would truncate the C string */
            }
            char   enc[4];
            size_t n = utf8_encode(cp, enc);
            sink_put(sink, enc, n);
            continue;
        }
        default:
            return NULL;
        }
        sink_put(sink, &simple, 1);
        p++;
    }
    return p + 1;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *skip_number(const char *p)
{
    if (*p == '-') {
        p++;
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (is_digit(*p)) {
            p++;
        }
    } else {
        return NULL;
    }
    if (*p == '.') {
        p++;
        if (!is_digit(*p)) {
            return NULL;
        }
        while (is_digit(*p)) {
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') {
            p++;
        }
        if (!is_digit(*p)) {
            return NULL;
        }
        while (is_digit(*p)) {
            p++;
        }
    }
    return p;
}

static const char *skip_value(const char *p, int depth);

static const char *skip_object(const char *p, int depth)
{
    p = skip_ws(p + 1); /* past '{' */
    if (*p == '}') {
        return p + 1;
    }
    for (;;) {
        p = scan_string(p, NULL);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p != ':') {
            return NULL;
        }
        p = skip_value(skip_ws(p + 1), depth + 1);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            return p + 1;
        }
        return NULL;
    }
}

static const char *skip_array(const char *p, int depth)
{
    p = skip_ws(p + 1); /* past '[' */
    if (*p == ']') {
        return p + 1;
    }
    for (;;) {
        p = skip_value(p, depth + 1);
        if (p == NULL) {
            return NULL;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == ']') {
            return p + 1;
        }
        return NULL;
    }
}

static const char *skip_value(const char *p, int depth)
{
    if (depth > AK_JSON_MAX_DEPTH) {
        return NULL;
    }
    switch (*p) {
    case '{':
        return skip_object(p, depth);
    case '[':
        return skip_array(p, depth);
    case '"':
        return scan_string(p, NULL);
    case 't':
        return strncmp(p, "true", 4) == 0 ? p + 4 : NULL;
    case 'f':
        return strncmp(p, "false", 5) == 0 ? p + 5 : NULL;
    case 'n':
        return strncmp(p, "null", 4) == 0 ? p + 4 : NULL;
    default:
        return skip_number(p);
    }
}

bool ak_json_is_object(const char *json)
{
    if (json == NULL) {
        return false;
    }
    const char *p = skip_ws(json);
    if (*p != '{') {
        return false;
    }
    p = skip_value(p, 0);
    return p != NULL && *skip_ws(p) == '\0';
}

/* Walk the members of the object at `obj` (validating every member) and
 * point *value at the value of `key`. A duplicate key is an error: first- and
 * last-wins parsers would disagree about it. */
static ak_json_rc_t find_member(const char *obj, const char *key, const char **value)
{
    if (obj == NULL || key == NULL) {
        return AK_JSON_INVALID;
    }
    const char *p = skip_ws(obj);
    if (*p != '{') {
        return AK_JSON_INVALID;
    }
    const char *found = NULL;
    p = skip_ws(p + 1);
    if (*p == '}') {
        return AK_JSON_ABSENT;
    }
    for (;;) {
        char      name[AK_JSON_NAME_MAX];
        ak_sink_t sink = {name, sizeof(name), 0, false};
        p = scan_string(p, &sink);
        if (p == NULL) {
            return AK_JSON_INVALID;
        }
        name[sink.len] = '\0';
        p = skip_ws(p);
        if (*p != ':') {
            return AK_JSON_INVALID;
        }
        p = skip_ws(p + 1);
        if (!sink.overflow && strcmp(name, key) == 0) {
            if (found != NULL) {
                return AK_JSON_INVALID;
            }
            found = p;
        }
        p = skip_value(p, 1);
        if (p == NULL) {
            return AK_JSON_INVALID;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        if (*p == '}') {
            break;
        }
        return AK_JSON_INVALID;
    }
    if (found == NULL) {
        return AK_JSON_ABSENT;
    }
    *value = found;
    return AK_JSON_OK;
}

/* Trim a clipped UTF-8 prefix back to the last complete character. */
static size_t utf8_clip_len(const char *s, size_t len)
{
    size_t lead = len;
    while (lead > 0 && ((unsigned char)s[lead - 1] & 0xC0U) == 0x80U) {
        lead--;
    }
    if (lead == 0) {
        return 0;
    }
    unsigned char c    = (unsigned char)s[lead - 1];
    size_t        need = 1;
    if ((c & 0xE0U) == 0xC0U) {
        need = 2;
    } else if ((c & 0xF0U) == 0xE0U) {
        need = 3;
    } else if ((c & 0xF8U) == 0xF0U) {
        need = 4;
    }
    return (lead - 1 + need <= len) ? len : lead - 1;
}

static ak_json_rc_t get_str(const char *obj, const char *key, char *out, size_t out_size, bool clip)
{
    if (out == NULL || out_size == 0) {
        return AK_JSON_INVALID;
    }
    out[0] = '\0';
    const char  *v  = NULL;
    ak_json_rc_t rc = find_member(obj, key, &v);
    if (rc != AK_JSON_OK) {
        return rc;
    }
    if (*v != '"') {
        return AK_JSON_INVALID;
    }
    ak_sink_t sink = {out, out_size, 0, false};
    if (scan_string(v, &sink) == NULL) {
        out[0] = '\0';
        return AK_JSON_INVALID;
    }
    if (sink.overflow) {
        if (!clip) {
            out[0] = '\0';
            return AK_JSON_INVALID;
        }
        sink.len = utf8_clip_len(out, sink.len);
    }
    out[sink.len] = '\0';
    return AK_JSON_OK;
}

ak_json_rc_t ak_json_get_str(const char *obj, const char *key, char *out, size_t out_size)
{
    return get_str(obj, key, out, out_size, false);
}

ak_json_rc_t ak_json_get_str_clip(const char *obj, const char *key, char *out, size_t out_size)
{
    return get_str(obj, key, out, out_size, true);
}

ak_json_rc_t ak_json_get_uint(const char *obj, const char *key, uint32_t max, uint32_t *out)
{
    const char  *p  = NULL;
    ak_json_rc_t rc = find_member(obj, key, &p);
    if (rc != AK_JSON_OK) {
        return rc;
    }
    if (!is_digit(*p) || (p[0] == '0' && is_digit(p[1]))) {
        return AK_JSON_INVALID;
    }
    uint64_t value = 0;
    while (is_digit(*p)) {
        value = value * 10U + (uint64_t)(*p - '0');
        if (value > (uint64_t)max) {
            return AK_JSON_INVALID;
        }
        p++;
    }
    if (*p == '.' || *p == 'e' || *p == 'E') {
        return AK_JSON_INVALID;
    }
    if (out != NULL) {
        *out = (uint32_t)value;
    }
    return AK_JSON_OK;
}

ak_json_rc_t ak_json_get_object(const char *obj, const char *key, const char **member)
{
    const char  *p  = NULL;
    ak_json_rc_t rc = find_member(obj, key, &p);
    if (rc != AK_JSON_OK) {
        return rc;
    }
    if (*p != '{') {
        return AK_JSON_INVALID;
    }
    if (member != NULL) {
        *member = p;
    }
    return AK_JSON_OK;
}

ak_json_rc_t ak_json_get_bool(const char *obj, const char *key, bool *out)
{
    const char  *p  = NULL;
    ak_json_rc_t rc = find_member(obj, key, &p);
    if (rc != AK_JSON_OK) {
        return rc;
    }
    bool value;
    if (strncmp(p, "true", 4) == 0) {
        value = true;
    } else if (strncmp(p, "false", 5) == 0) {
        value = false;
    } else {
        return AK_JSON_INVALID;
    }
    if (out != NULL) {
        *out = value;
    }
    return AK_JSON_OK;
}

ak_json_rc_t ak_json_get_array(const char *obj, const char *key, const char **member)
{
    const char  *p  = NULL;
    ak_json_rc_t rc = find_member(obj, key, &p);
    if (rc != AK_JSON_OK) {
        return rc;
    }
    if (*p != '[') {
        return AK_JSON_INVALID;
    }
    if (member != NULL) {
        *member = p;
    }
    return AK_JSON_OK;
}

ak_json_rc_t ak_json_array_at(const char *arr, size_t index, const char **elem)
{
    if (arr == NULL) {
        return AK_JSON_INVALID;
    }
    const char *p = skip_ws(arr);
    if (*p != '[') {
        return AK_JSON_INVALID;
    }
    p = skip_ws(p + 1);
    if (*p == ']') {
        return AK_JSON_ABSENT;
    }
    for (size_t i = 0;; i++) {
        const char *start = p;
        p                 = skip_value(p, 1);
        if (p == NULL) {
            return AK_JSON_INVALID;
        }
        if (i == index) {
            if (elem != NULL) {
                *elem = start;
            }
            return AK_JSON_OK;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        return *p == ']' ? AK_JSON_ABSENT : AK_JSON_INVALID;
    }
}

/* ───────────────────────── Frame envelope ───────────────────────── */

bool ak_rx_parse_frame(const char *line, ak_rx_frame_t *frame)
{
    if (line == NULL || frame == NULL) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->payload = "{}";
    if (!ak_json_is_object(line)) {
        return false;
    }
    uint32_t v = 0;
    if (ak_json_get_uint(line, "v", UINT32_MAX, &v) != AK_JSON_OK || v != 1U) {
        return false;
    }
    if (ak_json_get_str(line, "type", frame->type, sizeof(frame->type)) != AK_JSON_OK ||
        frame->type[0] == '\0') {
        return false;
    }
    for (const char *c = frame->type; *c != '\0'; c++) {
        if (!((*c >= 'a' && *c <= 'z') || is_digit(*c) || *c == '_')) {
            return false;
        }
    }
    if (ak_json_get_uint(line, "seq", UINT32_MAX, &frame->seq) == AK_JSON_INVALID) {
        return false;
    }
    const char  *payload = NULL;
    ak_json_rc_t rc      = ak_json_get_object(line, "payload", &payload);
    if (rc == AK_JSON_INVALID) {
        return false;
    }
    if (rc == AK_JSON_OK) {
        frame->payload = payload;
    }
    return true;
}

bool ak_rx_type_is_sensitive(const char *type)
{
    static const char *const sensitive[] = {"wifi", "license", "license_read", "reset_netcfg", "ble_pair"};
    if (type == NULL) {
        return true;
    }
    for (size_t i = 0; i < sizeof(sensitive) / sizeof(sensitive[0]); i++) {
        if (strcmp(type, sensitive[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Firmware update (docs/ota.md) and its debug probes: USB only, but not tied
 * to the pairing window (the device asks for its own K6 confirmation). */
static bool ak_rx_type_is_usb_only(const char *type)
{
    static const char *const usb_only[] = {"ota_begin", "ota_data", "ota_end", "ota_abort", "ota_probe",
                                           "probe_line"};
    for (size_t i = 0; i < sizeof(usb_only) / sizeof(usb_only[0]); i++) {
        if (strcmp(type, usb_only[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool ak_rx_source_allows(const char *type, ak_rx_source_t source)
{
    if (source == AK_RX_SRC_UART) {
        return true;
    }
    return !ak_rx_type_is_sensitive(type) && !ak_rx_type_is_usb_only(type);
}
