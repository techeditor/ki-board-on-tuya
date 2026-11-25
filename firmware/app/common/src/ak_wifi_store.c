/**
 * @file ak_wifi_store.c
 * @brief Remembered Wi-Fi networks. See ak_wifi_store.h.
 *
 * Blob layout (little endian):
 *   'A' 'W' | version u8 | count u8 | next_seq u32
 *   count x { seq u32 | sec u8 | ssid_len u8 | pass_len u8 | ssid | pass }
 *   crc32 u32 over everything before it
 */

#include "ak_wifi_store.h"

#include <string.h>

static const uint32_t k_backoff_ms[] = {15000U, 30000U, 60000U, 120000U, 300000U};

void ak_wifi_store_init(ak_wifi_store_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}

void ak_wifi_store_clear(ak_wifi_store_t *s)
{
    ak_wifi_store_init(s);
}

int ak_wifi_store_find(const ak_wifi_store_t *s, const char *ssid)
{
    if (s == NULL || ssid == NULL) {
        return -1;
    }
    for (uint8_t i = 0U; i < s->count; i++) {
        if (strcmp(s->rec[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

ak_wifi_store_result_t ak_wifi_store_touch(ak_wifi_store_t *s, const char *ssid, const char *pass, uint8_t sec,
                                           char replaced_ssid[AK_WIFI_STORE_SSID_MAX + 1U])
{
    if (replaced_ssid != NULL) {
        replaced_ssid[0] = '\0';
    }
    if (s == NULL || ssid == NULL || pass == NULL) {
        return AK_WIFI_STORE_INVALID;
    }
    size_t ssid_len = strlen(ssid);
    if (ssid_len == 0U || ssid_len > AK_WIFI_STORE_SSID_MAX || strlen(pass) > AK_WIFI_STORE_PASS_MAX) {
        return AK_WIFI_STORE_INVALID;
    }

    ak_wifi_store_result_t result = AK_WIFI_STORE_UPDATED;
    int                    idx    = ak_wifi_store_find(s, ssid);
    if (idx < 0) {
        if (s->count < AK_WIFI_STORE_MAX) {
            idx = (int)s->count++;
            result = AK_WIFI_STORE_ADDED;
        } else {
            idx = 0;
            for (uint8_t i = 1U; i < s->count; i++) {
                if (s->rec[i].seq < s->rec[idx].seq) {
                    idx = (int)i;
                }
            }
            if (replaced_ssid != NULL) {
                memcpy(replaced_ssid, s->rec[idx].ssid, sizeof(s->rec[idx].ssid));
            }
            result = AK_WIFI_STORE_REPLACED;
        }
        memset(&s->rec[idx], 0, sizeof(s->rec[idx]));
        memcpy(s->rec[idx].ssid, ssid, ssid_len);
        s->rec[idx].sec = AK_WIFI_SEC_UNKNOWN;
    }
    memset(s->rec[idx].pass, 0, sizeof(s->rec[idx].pass));
    memcpy(s->rec[idx].pass, pass, strlen(pass));
    if (sec != AK_WIFI_SEC_UNKNOWN) {
        s->rec[idx].sec = sec;
    }
    s->rec[idx].seq = ++s->next_seq;
    return result;
}

bool ak_wifi_store_remove(ak_wifi_store_t *s, const char *ssid)
{
    int idx = ak_wifi_store_find(s, ssid);
    if (idx < 0) {
        return false;
    }
    for (uint8_t i = (uint8_t)idx; (uint8_t)(i + 1U) < s->count; i++) {
        s->rec[i] = s->rec[i + 1U];
    }
    memset(&s->rec[s->count - 1U], 0, sizeof(s->rec[0]));
    s->count--;
    return true;
}

bool ak_wifi_store_migrate_legacy(ak_wifi_store_t *s, const char *ssid, const char *pass)
{
    if (s == NULL || s->count != 0U || ssid == NULL || ssid[0] == '\0') {
        return false;
    }
    return ak_wifi_store_touch(s, ssid, pass != NULL ? pass : "", AK_WIFI_SEC_UNKNOWN, NULL) == AK_WIFI_STORE_ADDED;
}

size_t ak_wifi_store_by_recency(const ak_wifi_store_t *s, uint8_t out[AK_WIFI_STORE_MAX])
{
    if (s == NULL || out == NULL) {
        return 0U;
    }
    for (uint8_t i = 0U; i < s->count; i++) {
        out[i] = i;
    }
    /* Insertion sort, descending seq: at most AK_WIFI_STORE_MAX items. */
    for (uint8_t i = 1U; i < s->count; i++) {
        uint8_t v = out[i];
        uint8_t j = i;
        while (j > 0U && s->rec[out[j - 1U]].seq < s->rec[v].seq) {
            out[j] = out[j - 1U];
            j--;
        }
        out[j] = v;
    }
    return s->count;
}

/* ── Blob ───────────────────────────────────────────────────────────────── */

static uint32_t crc32_calc(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t ak_wifi_store_serialize(const ak_wifi_store_t *s, uint8_t *buf, size_t cap)
{
    if (s == NULL || buf == NULL || s->count > AK_WIFI_STORE_MAX) {
        return 0U;
    }
    size_t need = 8U + 4U;
    for (uint8_t i = 0U; i < s->count; i++) {
        need += 7U + strlen(s->rec[i].ssid) + strlen(s->rec[i].pass);
    }
    if (need > cap) {
        return 0U;
    }
    size_t n = 0U;
    buf[n++] = 'A';
    buf[n++] = 'W';
    buf[n++] = (uint8_t)AK_WIFI_STORE_VERSION;
    buf[n++] = s->count;
    put_u32(&buf[n], s->next_seq);
    n += 4U;
    for (uint8_t i = 0U; i < s->count; i++) {
        const ak_wifi_rec_t *r  = &s->rec[i];
        size_t               sl = strlen(r->ssid);
        size_t               pl = strlen(r->pass);
        put_u32(&buf[n], r->seq);
        n += 4U;
        buf[n++] = r->sec;
        buf[n++] = (uint8_t)sl;
        buf[n++] = (uint8_t)pl;
        memcpy(&buf[n], r->ssid, sl);
        n += sl;
        memcpy(&buf[n], r->pass, pl);
        n += pl;
    }
    put_u32(&buf[n], crc32_calc(buf, n));
    return n + 4U;
}

ak_wifi_blob_result_t ak_wifi_store_parse(ak_wifi_store_t *s, const uint8_t *buf, size_t len)
{
    return ak_wifi_store_parse_keep(s, buf, len, NULL);
}

/* Scratch for a blob written by firmware that allowed AK_WIFI_STORE_LEGACY_MAX records. */
static ak_wifi_rec_t s_parse_rec[AK_WIFI_STORE_LEGACY_MAX];

ak_wifi_blob_result_t ak_wifi_store_parse_keep(ak_wifi_store_t *s, const uint8_t *buf, size_t len,
                                               const char *keep_ssid)
{
    if (s == NULL) {
        return AK_WIFI_BLOB_BAD_FORMAT;
    }
    ak_wifi_store_init(s);
    if (buf == NULL || len < 12U || len > AK_WIFI_STORE_BLOB_MAX || buf[0] != 'A' || buf[1] != 'W') {
        return AK_WIFI_BLOB_BAD_FORMAT;
    }
    if (get_u32(&buf[len - 4U]) != crc32_calc(buf, len - 4U)) {
        return AK_WIFI_BLOB_BAD_CRC;
    }
    if (buf[2] != (uint8_t)AK_WIFI_STORE_VERSION) {
        return AK_WIFI_BLOB_BAD_VERSION;
    }
    uint8_t count = buf[3];
    if (count > AK_WIFI_STORE_LEGACY_MAX) {
        return AK_WIFI_BLOB_BAD_FORMAT;
    }
    ak_wifi_store_t tmp;
    ak_wifi_store_init(&tmp);
    memset(s_parse_rec, 0, sizeof(s_parse_rec));
    tmp.next_seq = get_u32(&buf[4]);
    size_t pos   = 8U;
    size_t end   = len - 4U;
    for (uint8_t i = 0U; i < count; i++) {
        if (pos + 7U > end) {
            return AK_WIFI_BLOB_BAD_FORMAT;
        }
        ak_wifi_rec_t *r  = &s_parse_rec[i];
        r->seq            = get_u32(&buf[pos]);
        r->sec            = buf[pos + 4U];
        size_t sl         = buf[pos + 5U];
        size_t pl         = buf[pos + 6U];
        pos += 7U;
        if (sl == 0U || sl > AK_WIFI_STORE_SSID_MAX || pl > AK_WIFI_STORE_PASS_MAX || pos + sl + pl > end ||
            r->seq > tmp.next_seq || memchr(&buf[pos], '\0', sl) != NULL || memchr(&buf[pos + sl], '\0', pl) != NULL) {
            return AK_WIFI_BLOB_BAD_FORMAT;
        }
        memcpy(r->ssid, &buf[pos], sl);
        memcpy(r->pass, &buf[pos + sl], pl);
        pos += sl + pl;
        for (uint8_t j = 0U; j < i; j++) {
            if (strcmp(s_parse_rec[j].ssid, r->ssid) == 0) {
                return AK_WIFI_BLOB_BAD_FORMAT; /* duplicate SSID */
            }
        }
    }
    if (pos != end) {
        return AK_WIFI_BLOB_BAD_FORMAT;
    }
    /* Over the limit (older firmware): drop the least recently connected,
     * but never the network in use. */
    uint8_t n = count;
    while (n > AK_WIFI_STORE_MAX) {
        int drop = -1;
        for (uint8_t i = 0U; i < n; i++) {
            if (keep_ssid != NULL && strcmp(s_parse_rec[i].ssid, keep_ssid) == 0) {
                continue;
            }
            if (drop < 0 || s_parse_rec[i].seq < s_parse_rec[drop].seq) {
                drop = (int)i;
            }
        }
        for (uint8_t i = (uint8_t)drop; (uint8_t)(i + 1U) < n; i++) {
            s_parse_rec[i] = s_parse_rec[i + 1U];
        }
        n--;
    }
    for (uint8_t i = 0U; i < n; i++) {
        tmp.rec[i] = s_parse_rec[i];
    }
    memset(s_parse_rec, 0, sizeof(s_parse_rec)); /* passwords do not stay in the scratch */
    tmp.count = n;
    *s        = tmp;
    return n < count ? AK_WIFI_BLOB_TRIMMED : AK_WIFI_BLOB_OK;
}

/* ── Reconnect planning ─────────────────────────────────────────────────── */

size_t ak_wifi_store_plan(const ak_wifi_store_t *s, const ak_wifi_scan_t *scan, size_t scan_n, uint16_t tried,
                          uint8_t *out, size_t cap)
{
    if (s == NULL || scan == NULL || out == NULL) {
        return 0U;
    }
    int8_t  best_rssi[AK_WIFI_STORE_MAX];
    uint8_t order[AK_WIFI_STORE_MAX];
    size_t  n = 0U;
    for (uint8_t i = 0U; i < s->count && n < cap; i++) {
        if ((tried & (uint16_t)(1U << i)) != 0U) {
            continue;
        }
        bool seen = false;
        for (size_t k = 0U; k < scan_n; k++) {
            if (strcmp(scan[k].ssid, s->rec[i].ssid) == 0 && (!seen || scan[k].rssi > best_rssi[n])) {
                best_rssi[n] = scan[k].rssi;
                seen         = true;
            }
        }
        if (seen) {
            order[n++] = i;
        }
    }
    /* Strongest first; equal signal: more recently connected first. */
    for (size_t i = 1U; i < n; i++) {
        uint8_t v  = order[i];
        int8_t  vr = best_rssi[i];
        size_t  j  = i;
        while (j > 0U && (best_rssi[j - 1U] < vr || (best_rssi[j - 1U] == vr && s->rec[order[j - 1U]].seq < s->rec[v].seq))) {
            order[j]     = order[j - 1U];
            best_rssi[j] = best_rssi[j - 1U];
            j--;
        }
        order[j]     = v;
        best_rssi[j] = vr;
    }
    for (size_t i = 0U; i < n; i++) {
        out[i] = order[i];
    }
    return n;
}

int ak_wifi_store_mru(const ak_wifi_store_t *s, uint16_t tried)
{
    int best = -1;
    if (s == NULL) {
        return -1;
    }
    for (uint8_t i = 0U; i < s->count; i++) {
        if ((tried & (uint16_t)(1U << i)) == 0U && (best < 0 || s->rec[i].seq > s->rec[best].seq)) {
            best = (int)i;
        }
    }
    return best;
}

uint32_t ak_wifi_roam_backoff_ms(uint8_t round)
{
    size_t last = sizeof(k_backoff_ms) / sizeof(k_backoff_ms[0]) - 1U;
    return k_backoff_ms[round < last ? round : last];
}

bool ak_wifi_attempt_down(ak_wifi_attempt_t *a, bool sdk_retrying, uint32_t now_ms)
{
    a->armed       = sdk_retrying;
    a->deadline_ms = now_ms + AK_WIFI_CONNECTING_UI_MS;
    return sdk_retrying;
}

void ak_wifi_attempt_clear(ak_wifi_attempt_t *a)
{
    a->armed = false;
}

bool ak_wifi_attempt_expired(ak_wifi_attempt_t *a, uint32_t now_ms)
{
    if (!a->armed || (int32_t)(now_ms - a->deadline_ms) < 0) {
        return false;
    }
    a->armed = false;
    return true;
}
