/**
 * @file ak_menu.c
 * @brief LCD menu state machine, frame parsing and key lights (pure, host-
 *        testable). See ak_menu.h.
 */

#include "ak_menu.h"

#include <string.h>

#include "ak_rx.h"

/* ───────────────────────── Parsing ───────────────────────── */

/* Identifier fields (id, kind, role, select): 1..15 bytes of [a-z0-9_]. */
static bool ident_ok(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (const char *c = s; *c != '\0'; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) {
            return false;
        }
    }
    return true;
}

static ak_menu_kind_t kind_from(const char *s)
{
    static const struct {
        const char    *name;
        ak_menu_kind_t kind;
    } k[] = {
        {"approval", AK_MENU_KIND_APPROVAL}, {"options", AK_MENU_KIND_OPTIONS}, {"question", AK_MENU_KIND_QUESTION},
        {"model", AK_MENU_KIND_MODEL},       {"mode", AK_MENU_KIND_MODE},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(s, k[i].name) == 0) {
            return k[i].kind;
        }
    }
    return AK_MENU_KIND_OPTIONS;
}

static uint8_t role_from(const char *s)
{
    static const struct {
        const char    *name;
        ak_menu_role_t role;
    } r[] = {
        {"confirm", AK_MENU_ROLE_CONFIRM},     {"cancel", AK_MENU_ROLE_CANCEL}, {"danger", AK_MENU_ROLE_DANGER},
        {"attention", AK_MENU_ROLE_ATTENTION}, {"voice", AK_MENU_ROLE_VOICE},
    };
    for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        if (strcmp(s, r[i].name) == 0) {
            return (uint8_t)r[i].role;
        }
    }
    return (uint8_t)AK_MENU_ROLE_NORMAL;
}

/* Read an identifier field. ABSENT stays ABSENT; anything not a valid
 * identifier (wrong type, too long, bad characters) is INVALID. */
static ak_json_rc_t get_ident(const char *obj, const char *key, char *out, size_t size)
{
    ak_json_rc_t rc = ak_json_get_str(obj, key, out, size);
    if (rc == AK_JSON_OK && !ident_ok(out)) {
        return AK_JSON_INVALID;
    }
    return rc;
}

/* Optional display text: absent -> "", wrong type -> false. */
static bool get_text(const char *obj, const char *key, char *out, size_t size)
{
    return ak_json_get_str_clip(obj, key, out, size) != AK_JSON_INVALID;
}

/* Optional bounded integer: absent leaves *out, out of range -> false. */
static bool get_num(const char *obj, const char *key, uint32_t max, uint32_t *out)
{
    return ak_json_get_uint(obj, key, max, out) != AK_JSON_INVALID;
}

static bool parse_item(const char *obj, ak_menu_item_t *it)
{
    char word[AK_MENU_ID_MAX + 1U];
    if (*obj != '{' || ak_json_get_str_clip(obj, "label", it->label, sizeof(it->label)) != AK_JSON_OK) {
        return false;
    }
    ak_json_rc_t rc = get_ident(obj, "role", word, sizeof(word));
    if (rc == AK_JSON_INVALID) {
        return false;
    }
    it->role = rc == AK_JSON_OK ? role_from(word) : (uint8_t)AK_MENU_ROLE_NORMAL;
    return ak_json_get_bool(obj, "needs_double_confirm", &it->double_confirm) != AK_JSON_INVALID &&
           ak_json_get_bool(obj, "checked", &it->checked) != AK_JSON_INVALID;
}

bool ak_menu_parse(const char *payload, ak_menu_page_t *out)
{
    char     word[AK_MENU_ID_MAX + 1U];
    uint32_t page = 1U, pages = 1U, def = 0U, timeout = 0U;

    if (payload == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (get_ident(payload, "id", out->id, sizeof(out->id)) != AK_JSON_OK ||
        get_ident(payload, "kind", word, sizeof(word)) != AK_JSON_OK) {
        return false;
    }
    out->kind = kind_from(word);
    if (!get_text(payload, "title", out->title, sizeof(out->title)) ||
        !get_text(payload, "ctx", out->ctx, sizeof(out->ctx)) ||
        !get_text(payload, "queue", out->queue, sizeof(out->queue)) ||
        !get_num(payload, "page", AK_MENU_PAGES_MAX, &page) || !get_num(payload, "pages", AK_MENU_PAGES_MAX, &pages) ||
        !get_num(payload, "default", AK_MENU_ITEMS_MAX - 1U, &def) ||
        !get_num(payload, "timeout_s", AK_MENU_TIMEOUT_MAX_S, &timeout) || page < 1U || pages < 1U ||
        page > pages) {
        return false;
    }
    ak_json_rc_t rc = get_ident(payload, "select", word, sizeof(word));
    if (rc == AK_JSON_INVALID || (rc == AK_JSON_OK && strcmp(word, "single") != 0 && strcmp(word, "multi") != 0)) {
        return false;
    }
    if (ak_json_get_bool(payload, "more", &out->more) == AK_JSON_INVALID) {
        return false;
    }
    out->multi     = rc == AK_JSON_OK && strcmp(word, "multi") == 0;
    out->page      = (uint8_t)page;
    out->pages     = (uint8_t)pages;
    out->def       = (uint8_t)def;
    out->timeout_s = (uint16_t)timeout;

    const char *arr = NULL;
    if (ak_json_get_array(payload, "items", &arr) != AK_JSON_OK) {
        return false;
    }
    for (size_t i = 0;; i++) {
        const char *elem = NULL;
        rc               = ak_json_array_at(arr, i, &elem);
        if (rc == AK_JSON_ABSENT) {
            break;
        }
        if (rc != AK_JSON_OK || i >= AK_MENU_PAGE_ITEMS_MAX || !parse_item(elem, &out->items[i])) {
            return false;
        }
        out->count = (uint8_t)(i + 1U);
    }
    return out->count > 0U;
}

bool ak_menu_parse_close(const char *payload, char id[AK_MENU_ID_MAX + 1U], char msg[AK_MENU_CLOSE_MSG_MAX + 1U])
{
    msg[0] = '\0';
    return get_ident(payload, "id", id, AK_MENU_ID_MAX + 1U) == AK_JSON_OK &&
           get_text(payload, "msg", msg, AK_MENU_CLOSE_MSG_MAX + 1U);
}

bool ak_menu_parse_notice(const char *payload, char text[AK_MENU_NOTICE_MAX + 1U], ak_notice_level_t *level)
{
    char word[AK_MENU_ID_MAX + 1U];
    *level = AK_NOTICE_INFO;
    if (ak_json_get_str_clip(payload, "text", text, AK_MENU_NOTICE_MAX + 1U) != AK_JSON_OK) {
        return false;
    }
    ak_json_rc_t rc = get_ident(payload, "level", word, sizeof(word));
    if (rc == AK_JSON_INVALID) {
        return false;
    }
    if (rc == AK_JSON_OK) {
        if (strcmp(word, "warn") == 0) {
            *level = AK_NOTICE_WARN;
        } else if (strcmp(word, "error") == 0) {
            *level = AK_NOTICE_ERROR;
        } else if (strcmp(word, "info") != 0) {
            return false;
        }
    }
    return true;
}

bool ak_menu_parse_parked(const char *payload, uint8_t *count)
{
    uint32_t n = 0U;
    if (ak_json_get_uint(payload, "count", AK_MENU_PARKED_MAX, &n) != AK_JSON_OK) {
        return false;
    }
    *count = (uint8_t)n;
    return true;
}

/* ───────────────────────── Model helpers ───────────────────────── */

static bool elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms)
{
    return (uint32_t)(now_ms - since_ms) >= span_ms;
}

static const ak_menu_item_t *item_at(const ak_menu_t *m, uint8_t page, uint8_t idx)
{
    if (page >= m->pages || idx >= m->pg[page].count) {
        return NULL;
    }
    return &m->pg[page].items[idx];
}

static unsigned total_items(const ak_menu_t *m)
{
    unsigned n = 0U;
    for (uint8_t p = 0U; p < m->pages; p++) {
        n += m->pg[p].count;
    }
    return n;
}

int ak_menu_global_index(const ak_menu_t *m, uint8_t page, uint8_t idx)
{
    if (item_at(m, page, idx) == NULL) {
        return -1;
    }
    int base = 0;
    for (uint8_t p = 0U; p < page; p++) {
        if (m->pg[p].count == 0U) {
            return -1;
        }
        base += m->pg[p].count;
    }
    return base + idx;
}

bool ak_menu_owns_keys(const ak_menu_t *m)
{
    return m->state == AK_MENU_OPEN && !m->hidden;
}

bool ak_menu_is_model(const ak_menu_t *m)
{
    return m->pg[0].kind == AK_MENU_KIND_MODEL || m->pg[0].kind == AK_MENU_KIND_MODE;
}

bool ak_menu_enter_allowed(const ak_menu_t *m)
{
    return AK_MENU_APPROVAL_ENTER || m->pg[0].kind != AK_MENU_KIND_APPROVAL;
}

bool ak_menu_item_ticked(const ak_menu_t *m, uint8_t page, uint8_t idx)
{
    const ak_menu_item_t *it = item_at(m, page, idx);
    if (it == NULL) {
        return false;
    }
    return m->multi ? (m->picked[page] & (1U << idx)) != 0U : it->checked;
}

bool ak_menu_armed(const ak_menu_t *m)
{
    return m->state == AK_MENU_OPEN && m->armed_key >= 0;
}

/* Put the highlight on `default` once its page (and every page before it)
 * has arrived, unless the user already moved it. */
static void apply_default(ak_menu_t *m)
{
    if (m->cursor_moved) {
        return;
    }
    unsigned base = 0U;
    for (uint8_t p = 0U; p < m->pages && m->pg[p].count > 0U; p++) {
        if (m->def < base + m->pg[p].count) {
            m->cur_page = p;
            m->cur_idx  = (uint8_t)(m->def - base);
            break;
        }
        base += m->pg[p].count;
    }
}

static void disarm(ak_menu_t *m)
{
    m->armed_key   = -1;
    m->armed_again = false;
}

static bool store_page(ak_menu_t *m, const ak_menu_page_t *p)
{
    uint8_t       i  = (uint8_t)(p->page - 1U);
    ak_menu_pg_t *pg = &m->pg[i];
    if (total_items(m) - pg->count + p->count > AK_MENU_ITEMS_MAX) {
        return false;
    }
    pg->kind  = p->kind;
    pg->count = p->count;
    memcpy(pg->title, p->title, sizeof(pg->title));
    memcpy(pg->items, p->items, sizeof(pg->items));
    memcpy(m->ctx, p->ctx, sizeof(m->ctx));
    memcpy(m->queue, p->queue, sizeof(m->queue));
    m->more = p->more;
    if (p->page == 1U) {
        m->def = p->def; /* the Companion sends `default` on page 1 only */
    }
    m->picked[i] = 0U;
    for (uint8_t k = 0U; k < p->count; k++) {
        if (p->items[k].checked) {
            m->picked[i] |= (uint8_t)(1U << k);
        }
    }
    if (m->cur_page == i && m->cur_idx >= p->count) {
        m->cur_idx = (uint8_t)(p->count - 1U);
    }
    if (m->armed_key >= 0 && m->armed_page == i) {
        disarm(m);
    }
    return true;
}

/* ───────────────────────── State machine ───────────────────────── */

void ak_menu_init(ak_menu_t *m)
{
    memset(m, 0, sizeof(*m));
    m->state        = AK_MENU_IDLE;
    m->armed_key    = -1;
    m->dictate_page = -1;
}

ak_menu_apply_t ak_menu_on_page(ak_menu_t *m, const ak_menu_page_t *p, uint32_t now_ms)
{
    bool same = strcmp(m->id, p->id) == 0;
    if (m->state == AK_MENU_OPEN && same) {
        if (p->pages != m->pages || p->multi != m->multi || !store_page(m, p)) {
            return AK_MENU_APPLY_REJECTED;
        }
        apply_default(m);
        return AK_MENU_APPLY_UPDATED;
    }
    if (m->state == AK_MENU_WAIT && same) {
        return AK_MENU_APPLY_IGNORED; /* already answered */
    }
    /* A new id replaces whatever was showing; the old one gets no result. */
    ak_menu_init(m);
    m->state = AK_MENU_OPEN;
    memcpy(m->id, p->id, sizeof(m->id));
    m->pages     = p->pages;
    m->multi     = p->multi;
    m->timeout_s = p->timeout_s;
    m->opened_ms = now_ms;
    m->guard_ms  = now_ms;
    (void)store_page(m, p); /* one page of <= 4 items always fits */
    apply_default(m);
    return AK_MENU_APPLY_OPENED;
}

bool ak_menu_on_close(ak_menu_t *m, const char *id)
{
    if (m->state == AK_MENU_IDLE || id == NULL || strcmp(m->id, id) != 0) {
        return false;
    }
    ak_menu_init(m);
    return true;
}

void ak_menu_set_hidden(ak_menu_t *m, bool hidden, uint32_t now_ms)
{
    if (m->hidden && !hidden) {
        m->guard_ms = now_ms; /* it appears now */
    }
    if (hidden) {
        m->pressed  = 0U;
        m->consumed = 0U;
        disarm(m);
    }
    m->hidden = hidden;
}

static void out_clear(ak_menu_out_t *out)
{
    out->kind   = AK_MENU_OUT_NONE;
    out->reason = NULL;
    out->text   = NULL;
    out->n      = 0U;
    out->redraw = false;
}

/* A result or dismissal went out: keys return to the keymap, LCD shows
 * "..." until menu_close / a new menu / AK_MENU_WAIT_MS. */
static void finish(ak_menu_t *m, uint32_t now_ms, ak_menu_out_t *out)
{
    m->state        = AK_MENU_WAIT;
    m->wait_ms      = now_ms;
    m->pressed      = 0U;
    m->consumed     = 0U;
    m->dictate_page = -1;
    disarm(m);
    out->redraw = true;
}

static void dismiss(ak_menu_t *m, const char *reason, uint32_t now_ms, ak_menu_out_t *out)
{
    out->kind   = AK_MENU_OUT_DISMISS;
    out->reason = reason;
    finish(m, now_ms, out);
}

static void result_single(ak_menu_t *m, uint8_t page, uint8_t idx, uint32_t now_ms, ak_menu_out_t *out)
{
    int g = ak_menu_global_index(m, page, idx);
    if (g < 0) {
        return; /* an earlier page has not arrived: the index is unknown */
    }
    out->kind        = AK_MENU_OUT_RESULT;
    out->selected[0] = (uint8_t)g;
    out->n           = 1U;
    finish(m, now_ms, out);
}

static void submit_multi(ak_menu_t *m, uint32_t now_ms, ak_menu_out_t *out)
{
    uint8_t n = 0U, base = 0U;
    for (uint8_t p = 0U; p < m->pages; p++) {
        if (m->pg[p].count == 0U) {
            return; /* wait for every page before answering */
        }
        for (uint8_t i = 0U; i < m->pg[p].count; i++) {
            if (m->picked[p] & (1U << i)) {
                out->selected[n++] = (uint8_t)(base + i);
            }
        }
        base = (uint8_t)(base + m->pg[p].count);
    }
    out->kind = AK_MENU_OUT_RESULT;
    out->n    = n; /* 0 = the Companion treats it as dismissed */
    finish(m, now_ms, out);
}

static void arm(ak_menu_t *m, int key, uint8_t page, uint8_t idx, uint32_t now_ms, ak_menu_out_t *out)
{
    m->armed_key   = (int8_t)key;
    m->armed_page  = page;
    m->armed_idx   = idx;
    m->armed_ms    = now_ms;
    m->armed_again = false;
    out->redraw    = true;
}

/* Single select: answer, start a dictation, or arm the double confirm. */
static void choose(ak_menu_t *m, int key, uint8_t page, uint8_t idx, uint32_t now_ms, ak_menu_out_t *out)
{
    const ak_menu_item_t *it = item_at(m, page, idx);
    if (it == NULL) {
        return;
    }
    if (it->role == AK_MENU_ROLE_VOICE) {
        m->dictate_page = (int8_t)page;
        m->dictate_idx  = idx;
        out->kind       = AK_MENU_OUT_DICTATE;
        out->redraw     = true;
        return;
    }
    if (it->double_confirm) {
        arm(m, key, page, idx, now_ms, out);
        return;
    }
    result_single(m, page, idx, now_ms, out);
}

/* Multi select: tick / untick. Ticking a double-confirm item arms first. */
static void toggle(ak_menu_t *m, int key, uint8_t page, uint8_t idx, uint32_t now_ms, ak_menu_out_t *out)
{
    const ak_menu_item_t *it = item_at(m, page, idx);
    if (it == NULL || it->role == AK_MENU_ROLE_VOICE) {
        return;
    }
    uint8_t bit = (uint8_t)(1U << idx);
    if (it->double_confirm && (m->picked[page] & bit) == 0U) {
        arm(m, key, page, idx, now_ms, out);
        return;
    }
    m->picked[page] ^= bit;
    out->redraw = true;
}

/* Act on (page, idx): choose (single) or tick (multi). */
static void act(ak_menu_t *m, int key, uint8_t page, uint8_t idx, uint32_t now_ms, ak_menu_out_t *out)
{
    if (m->multi) {
        toggle(m, key, page, idx, now_ms, out);
    } else {
        choose(m, key, page, idx, now_ms, out);
    }
}

static void confirm_armed(ak_menu_t *m, uint32_t now_ms, ak_menu_out_t *out)
{
    uint8_t page = m->armed_page, idx = m->armed_idx;
    disarm(m);
    out->redraw = true;
    if (m->multi) {
        m->picked[page] |= (uint8_t)(1U << idx);
    } else {
        result_single(m, page, idx, now_ms, out);
    }
}

/* Highlight one step; past the page edge it enters the neighbouring page
 * (which may not have arrived yet: the LCD then shows "..."). */
static bool move(ak_menu_t *m, int dir)
{
    if (dir > 0) {
        if ((uint8_t)(m->cur_idx + 1U) < m->pg[m->cur_page].count) {
            m->cur_idx++;
        } else if ((uint8_t)(m->cur_page + 1U) < m->pages) {
            m->cur_page++;
            m->cur_idx = 0U;
        } else {
            return false;
        }
    } else if (m->cur_idx > 0U) {
        m->cur_idx--;
    } else if (m->cur_page > 0U) {
        m->cur_page--;
        uint8_t n  = m->pg[m->cur_page].count;
        m->cur_idx = n > 0U ? (uint8_t)(n - 1U) : 0U;
    } else {
        return false;
    }
    m->cursor_moved = true;
    return true;
}

static void on_release(ak_menu_t *m, int key, uint32_t now_ms, ak_menu_out_t *out)
{
    if (key == 0) {
        dismiss(m, "esc", now_ms, out);
        return;
    }
    if (key >= 1 && key <= 4) { /* K2..K5: item 1..4 of the current page */
        uint8_t idx = (uint8_t)(key - 1);
        if (item_at(m, m->cur_page, idx) == NULL) {
            return; /* unlit key */
        }
        m->cur_idx      = idx;
        m->cursor_moved = true;
        act(m, key, m->cur_page, idx, now_ms, out);
        out->redraw = true;
        return;
    }
    if (!ak_menu_enter_allowed(m)) {
        return; /* approval: K6 / EC11 click do nothing */
    }
    if (key == 5 && m->multi) {
        submit_multi(m, now_ms, out); /* K6 submits the ticks */
        return;
    }
    act(m, key, m->cur_page, m->cur_idx, now_ms, out); /* K6 / EC11 click */
}

void ak_menu_key(ak_menu_t *m, int key, bool down, uint32_t now_ms, ak_menu_out_t *out)
{
    out_clear(out);
    if (key < 0 || key >= AK_MENU_KEY_COUNT || !ak_menu_owns_keys(m)) {
        return;
    }
    uint8_t bit = (uint8_t)(1U << key);
    if (down) {
        if (!elapsed(now_ms, m->guard_ms, AK_MENU_GUARD_MS)) {
            return; /* anti-mistouch: this press (and its release) is ignored */
        }
        m->pressed |= bit;
        m->consumed &= (uint8_t)~bit;
        if (m->armed_key >= 0) {
            if (key == m->armed_key) {
                m->armed_again = true;
            } else {
                disarm(m); /* any other key cancels the double confirm */
                m->consumed |= bit;
                out->redraw = true;
            }
        }
        return;
    }
    if ((m->pressed & bit) == 0U) {
        return; /* pressed before the menu appeared, or inside the guard */
    }
    m->pressed &= (uint8_t)~bit;
    if (m->consumed & bit) {
        m->consumed &= (uint8_t)~bit;
        return;
    }
    if (m->armed_key >= 0) {
        if (key == m->armed_key && m->armed_again) {
            confirm_armed(m, now_ms, out);
        }
        return;
    }
    on_release(m, key, now_ms, out);
}

void ak_menu_encoder(ak_menu_t *m, int32_t detents, uint32_t now_ms, ak_menu_out_t *out)
{
    out_clear(out);
    if (!ak_menu_owns_keys(m) || detents == 0 || !elapsed(now_ms, m->guard_ms, AK_MENU_GUARD_MS)) {
        return;
    }
    if (m->armed_key >= 0) {
        disarm(m);
        out->redraw = true;
        return;
    }
    int     dir   = detents > 0 ? 1 : -1;
    int32_t steps = detents > 0 ? detents : -detents;
    for (int32_t i = 0; i < steps && move(m, dir); i++) {
        out->redraw = true;
    }
}

void ak_menu_dictated(ak_menu_t *m, const char *text, uint32_t now_ms, ak_menu_out_t *out)
{
    out_clear(out);
    if (m->state != AK_MENU_OPEN || m->dictate_page < 0 || text == NULL || text[0] == '\0') {
        return;
    }
    int g = ak_menu_global_index(m, (uint8_t)m->dictate_page, m->dictate_idx);
    if (g < 0) {
        return;
    }
    out->kind        = AK_MENU_OUT_RESULT;
    out->selected[0] = (uint8_t)g;
    out->n           = 1U;
    out->text        = text;
    finish(m, now_ms, out);
}

void ak_menu_dictate_cancel(ak_menu_t *m)
{
    m->dictate_page = -1;
}

void ak_menu_tick(ak_menu_t *m, uint32_t now_ms, ak_menu_out_t *out)
{
    out_clear(out);
    if (m->state == AK_MENU_OPEN) {
        if (m->timeout_s > 0U && elapsed(now_ms, m->opened_ms, (uint32_t)m->timeout_s * 1000U)) {
            dismiss(m, "timeout", now_ms, out);
        } else if (m->armed_key >= 0 && !m->armed_again && elapsed(now_ms, m->armed_ms, AK_MENU_CONFIRM2_MS)) {
            disarm(m);
            out->redraw = true;
        }
    } else if (m->state == AK_MENU_WAIT && elapsed(now_ms, m->wait_ms, AK_MENU_WAIT_MS)) {
        ak_menu_init(m);
        out->redraw = true;
    }
}

/* ───────────────────────── Key lights ───────────────────────── */

/* Full-level colour per light. ak_leds caps every channel at 48. */
static const ak_rgb_t k_light_full[] = {
    [AK_LIGHT_OFF]    = {0U, 0U, 0U},
    [AK_LIGHT_RED]    = {48U, 0U, 0U},
    [AK_LIGHT_GREEN]  = {0U, 48U, 0U},
    [AK_LIGHT_BLUE]   = {0U, 0U, 48U},
    [AK_LIGHT_YELLOW] = {48U, 32U, 0U},
    [AK_LIGHT_PURPLE] = {32U, 0U, 48U},
};

ak_light_t ak_menu_role_light(uint8_t role)
{
    switch (role) {
    case AK_MENU_ROLE_CONFIRM:
        return AK_LIGHT_GREEN;
    case AK_MENU_ROLE_CANCEL:
    case AK_MENU_ROLE_DANGER:
        return AK_LIGHT_RED;
    case AK_MENU_ROLE_ATTENTION:
        return AK_LIGHT_YELLOW;
    case AK_MENU_ROLE_VOICE:
        return AK_LIGHT_PURPLE;
    default:
        return AK_LIGHT_BLUE; /* a selectable option (user decision 2026-11-17) */
    }
}

ak_rgb_t ak_light_rgb(ak_light_t light, uint8_t pct)
{
    ak_rgb_t c = k_light_full[(unsigned)light < sizeof(k_light_full) / sizeof(k_light_full[0]) ? light : AK_LIGHT_OFF];
    if (pct > 100U) {
        pct = 100U;
    }
    c.r = (uint8_t)(c.r * pct / 100U);
    c.g = (uint8_t)(c.g * pct / 100U);
    c.b = (uint8_t)(c.b * pct / 100U);
    return c;
}

/* Triangle 0..100..0 over `period_ms`. */
static uint8_t breath_pct(uint32_t t, uint32_t period_ms)
{
    uint32_t half = period_ms / 2U;
    uint32_t p    = t % period_ms;
    return (uint8_t)(p < half ? p * 100U / half : (period_ms - p) * 100U / half);
}

void ak_menu_lights(const ak_menu_t *m, uint32_t now_ms, ak_rgb_t out[AK_LIGHT_COUNT])
{
    memset(out, 0, sizeof(ak_rgb_t) * AK_LIGHT_COUNT);
    if (!ak_menu_owns_keys(m)) {
        return;
    }
    out[0]                 = ak_light_rgb(AK_LIGHT_RED, AK_LIGHT_FULL_PCT); /* K1 = ESC */
    const ak_menu_pg_t *pg = &m->pg[m->cur_page];
    for (uint8_t i = 0U; i < pg->count; i++) {
        uint8_t role = pg->items[i].role;
        bool    lit  = i == m->cur_idx || ak_menu_item_ticked(m, m->cur_page, i);
        out[1U + i]  = ak_light_rgb(ak_menu_role_light(role), lit ? AK_LIGHT_FULL_PCT : AK_LIGHT_DIM_PCT);
    }
    if (ak_menu_enter_allowed(m)) {
        out[5] = ak_light_rgb(AK_LIGHT_GREEN, AK_LIGHT_FULL_PCT); /* K6 = Enter / submit */
        if (m->cur_idx < pg->count) {                           /* LED7: what the knob click picks */
            out[6] = ak_light_rgb(ak_menu_role_light(pg->items[m->cur_idx].role), AK_LIGHT_FULL_PCT);
        }
    }
    if (m->armed_key >= 0) {
        bool     on    = (uint32_t)(now_ms - m->armed_ms) % AK_LIGHT_FLASH_MS < AK_LIGHT_FLASH_MS / 2U;
        ak_rgb_t flash = ak_light_rgb(on ? AK_LIGHT_YELLOW : AK_LIGHT_OFF, AK_LIGHT_FULL_PCT);
        out[m->armed_key] = flash;
        if (m->armed_page == m->cur_page) {
            out[1U + m->armed_idx] = flash; /* the item's own key */
        }
    }
}

void ak_session_lights(uint8_t waiting_mask, bool k4_waiting, bool input_confirm, uint32_t pc_ms, uint32_t now_ms,
                       ak_rgb_t out[AK_LIGHT_COUNT])
{
    memset(out, 0, sizeof(ak_rgb_t) * AK_LIGHT_COUNT);
    uint8_t wait_pct = breath_pct(now_ms, AK_LIGHT_BREATH_MS);
    for (int i = 0; i < 3; i++) {
        if (waiting_mask & (1U << i)) {
            out[i] = ak_light_rgb(AK_LIGHT_YELLOW, wait_pct); /* K1..K3: chat waiting */
        }
    }
    if (input_confirm) {
        /* Post-ASR confirm hints: K4 red breathing (~1.6 s), K6 green blink. */
        out[3] = ak_light_rgb(AK_LIGHT_RED, breath_pct(pc_ms, 1600U));
        out[5] = ak_light_rgb(AK_LIGHT_GREEN, pc_ms % 1000U < 150U ? AK_LIGHT_FULL_PCT : 0U);
    } else if (k4_waiting) {
        out[3] = ak_light_rgb(AK_LIGHT_YELLOW, wait_pct); /* K4: a request waits */
    }
}
