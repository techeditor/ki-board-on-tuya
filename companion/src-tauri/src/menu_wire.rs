//! LCD menu frames (Companion -> keyboard): `menu`, `menu_close`, `notice`.
//!
//! Spec: `shared/protocol/protocol.md`, section "LCD 菜单与按键灯光". This module
//! is pure (no IO) so the size budget and pagination rules are unit-tested on
//! any host. The keyboard drops any line over [`MAX_FRAME_BYTES`], so every
//! frame built here is measured after serialisation, not estimated.

use serde_json::{Map, Value};

use crate::protocol::PROTOCOL_VERSION;

/// Longest line the keyboard accepts, excluding the trailing `\n`.
pub const MAX_FRAME_BYTES: usize = 511;
/// Byte caps for display text (the keyboard clips these too).
pub const LABEL_MAX: usize = 24;
pub const LABEL_MIN: usize = 12;
pub const TITLE_MAX: usize = 40;
pub const CTX_MAX: usize = 120;
pub const QUEUE_MAX: usize = 7;
pub const CLOSE_MSG_MAX: usize = 32;
pub const NOTICE_MAX: usize = 48;
/// Item and page limits: every page holds up to 4 items, one per K2..K5.
pub const ITEMS_PER_PAGE_MAX: usize = 4;
pub const PAGES_MAX: usize = 12;
pub const ITEMS_TOTAL_MAX: usize = 48;
pub const ID_MAX: usize = 15;
pub const TIMEOUT_MAX_S: u32 = 900;
pub const PARKED_MAX: usize = 99;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MenuKind {
    Approval,
    Options,
    Question,
    Model,
    Mode,
}

impl MenuKind {
    pub fn wire(self) -> &'static str {
        match self {
            MenuKind::Approval => "approval",
            MenuKind::Options => "options",
            MenuKind::Question => "question",
            MenuKind::Model => "model",
            MenuKind::Mode => "mode",
        }
    }
}

/// Item role: drives the key LED colour on the keyboard.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub enum MenuRole {
    Confirm,
    Cancel,
    Danger,
    #[default]
    Normal,
    Attention,
    Voice,
}

impl MenuRole {
    pub fn wire(self) -> &'static str {
        match self {
            MenuRole::Confirm => "confirm",
            MenuRole::Cancel => "cancel",
            MenuRole::Danger => "danger",
            MenuRole::Normal => "normal",
            MenuRole::Attention => "attention",
            MenuRole::Voice => "voice",
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Default)]
pub struct MenuItem {
    pub label: String,
    pub role: MenuRole,
    pub needs_double_confirm: bool,
    pub checked: bool,
}

impl MenuItem {
    pub fn new(label: impl Into<String>, role: MenuRole) -> Self {
        MenuItem { label: label.into(), role, ..Default::default() }
    }
    pub fn double_confirm(mut self) -> Self {
        self.needs_double_confirm = true;
        self
    }
    pub fn checked(mut self, on: bool) -> Self {
        self.checked = on;
        self
    }
}

/// A run of pages sharing one kind/title (the model menu has a "Model"
/// section followed by a "Mode" section).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MenuSection {
    pub kind: MenuKind,
    pub title: String,
    pub items: Vec<MenuItem>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Default)]
pub enum MenuSelect {
    #[default]
    Single,
    Multi,
}

/// One logical menu. Item indexes are global across sections, in order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct MenuSpec {
    pub id: String,
    pub ctx: String,
    /// The full content (diff, code, long question) is only on the PC
    /// screen; the keyboard shows "See screen".
    pub more: bool,
    pub queue: String,
    pub select: MenuSelect,
    pub default: usize,
    pub timeout_s: u32,
    pub sections: Vec<MenuSection>,
}

impl MenuSpec {
    pub fn single(id: impl Into<String>, kind: MenuKind, title: impl Into<String>, items: Vec<MenuItem>) -> Self {
        MenuSpec {
            id: id.into(),
            ctx: String::new(),
            more: false,
            queue: String::new(),
            select: MenuSelect::Single,
            default: 0,
            timeout_s: 0,
            sections: vec![MenuSection { kind, title: title.into(), items }],
        }
    }

    pub fn item_count(&self) -> usize {
        self.sections.iter().map(|s| s.items.len()).sum()
    }

    pub fn item(&self, index: usize) -> Option<&MenuItem> {
        self.sections.iter().flat_map(|s| s.items.iter()).nth(index)
    }

    /// One page on the keyboard: one section, <= 4 items (single or multi).
    pub fn is_direct(&self) -> bool {
        self.sections.len() == 1 && self.item_count() <= ITEMS_PER_PAGE_MAX
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MenuEncodeError {
    BadId,
    Empty,
    TooManyItems,
    TooManyPages,
    DoesNotFit,
}

/// Valid menu id: 1..=15 bytes of `[a-z0-9_]`.
pub fn valid_menu_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= ID_MAX
        && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// Clip to at most `max` bytes on a UTF-8 boundary; a clipped string ends in
/// `...` (within the budget).
pub fn clip_text(s: &str, max: usize) -> String {
    if s.len() <= max {
        return s.to_string();
    }
    if max <= 3 {
        return floor_boundary(s, max).to_string();
    }
    format!("{}...", floor_boundary(s, max - 3))
}

fn floor_boundary(s: &str, max: usize) -> &str {
    let mut end = max.min(s.len());
    while end > 0 && !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

fn item_value(item: &MenuItem, label_max: usize) -> Value {
    let mut m = Map::new();
    m.insert("label".into(), Value::String(clip_text(&item.label, label_max)));
    if item.role != MenuRole::Normal {
        m.insert("role".into(), Value::String(item.role.wire().into()));
    }
    if item.needs_double_confirm {
        m.insert("needs_double_confirm".into(), Value::Bool(true));
    }
    if item.checked {
        m.insert("checked".into(), Value::Bool(true));
    }
    Value::Object(m)
}

struct PageDraft<'a> {
    kind: MenuKind,
    title: &'a str,
    items: &'a [MenuItem],
}

fn page_frame(spec: &MenuSpec, page: &PageDraft, page_no: usize, pages: usize, ctx: &str, label_max: usize) -> Value {
    let mut p = Map::new();
    p.insert("id".into(), Value::String(spec.id.clone()));
    p.insert("kind".into(), Value::String(page.kind.wire().into()));
    let title = clip_text(page.title, TITLE_MAX);
    if !title.is_empty() {
        p.insert("title".into(), Value::String(title));
    }
    if !ctx.is_empty() {
        p.insert("ctx".into(), Value::String(ctx.to_string()));
    }
    if spec.more {
        p.insert("more".into(), Value::Bool(true));
    }
    if pages > 1 {
        p.insert("page".into(), Value::from(page_no as u64));
        p.insert("pages".into(), Value::from(pages as u64));
    }
    let queue = clip_text(&spec.queue, QUEUE_MAX);
    if !queue.is_empty() {
        p.insert("queue".into(), Value::String(queue));
    }
    if spec.select == MenuSelect::Multi {
        p.insert("select".into(), Value::String("multi".into()));
    }
    if page_no == 1 && spec.default != 0 {
        p.insert("default".into(), Value::from(spec.default as u64));
    }
    if spec.timeout_s != 0 {
        p.insert("timeout_s".into(), Value::from(spec.timeout_s.min(TIMEOUT_MAX_S) as u64));
    }
    p.insert(
        "items".into(),
        Value::Array(page.items.iter().map(|i| item_value(i, label_max)).collect()),
    );
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "menu", "payload": Value::Object(p) })
}

fn frame_len(v: &Value) -> usize {
    serde_json::to_vec(v).map(|b| b.len()).unwrap_or(usize::MAX)
}

/// Shrink `ctx` in 16-byte steps until `fits(ctx)` or it is empty.
fn shrink_ctx(ctx: &str, fits: impl Fn(&str) -> bool) -> Option<String> {
    let mut cur = ctx.to_string();
    loop {
        if fits(&cur) {
            return Some(cur);
        }
        if cur.is_empty() {
            return None;
        }
        let next = cur.len().saturating_sub(16);
        cur = if next == 0 { String::new() } else { clip_text(ctx, next) };
    }
}

/// Encode a menu into one `menu` frame per page, page 1 first, applying the
/// spec's omission and truncation rules. Every returned frame serialises to
/// at most [`MAX_FRAME_BYTES`].
pub fn encode_menu(spec: &MenuSpec) -> Result<Vec<Value>, MenuEncodeError> {
    if !valid_menu_id(&spec.id) {
        return Err(MenuEncodeError::BadId);
    }
    let total = spec.item_count();
    if total == 0 {
        return Err(MenuEncodeError::Empty);
    }
    if total > ITEMS_TOTAL_MAX {
        return Err(MenuEncodeError::TooManyItems);
    }
    let ctx = clip_text(&spec.ctx, CTX_MAX);

    if spec.is_direct() {
        let s = &spec.sections[0];
        let page = PageDraft { kind: s.kind, title: &s.title, items: &s.items };
        for label_max in [LABEL_MAX, LABEL_MIN] {
            let fits = |c: &str| frame_len(&page_frame(spec, &page, 1, 1, c, label_max)) <= MAX_FRAME_BYTES;
            if let Some(c) = shrink_ctx(&ctx, fits) {
                return Ok(vec![page_frame(spec, &page, 1, 1, &c, label_max)]);
            }
        }
        return Err(MenuEncodeError::DoesNotFit);
    }

    // Paged: fill each section's pages greedily (<= 6 items, <= 511 bytes).
    // The pages count is a single digit (<= 8), so measuring with a
    // placeholder of PAGES_MAX gives the final byte length.
    let mut drafts: Vec<PageDraft> = Vec::new();
    let mut ctx_used = ctx.clone();
    for s in &spec.sections {
        let mut start = 0;
        while start < s.items.len() {
            let mut end = (start + ITEMS_PER_PAGE_MAX).min(s.items.len());
            loop {
                let page = PageDraft { kind: s.kind, title: &s.title, items: &s.items[start..end] };
                let len = frame_len(&page_frame(spec, &page, 1, PAGES_MAX, &ctx_used, LABEL_MAX));
                if len <= MAX_FRAME_BYTES {
                    break;
                }
                if end - start > 1 {
                    end -= 1;
                    continue;
                }
                // A single item does not fit: shrink the shared ctx line.
                let fits = |c: &str| frame_len(&page_frame(spec, &page, 1, PAGES_MAX, c, LABEL_MAX)) <= MAX_FRAME_BYTES;
                ctx_used = shrink_ctx(&ctx_used, fits).ok_or(MenuEncodeError::DoesNotFit)?;
            }
            drafts.push(PageDraft { kind: s.kind, title: &s.title, items: &s.items[start..end] });
            start = end;
        }
    }
    if drafts.len() > PAGES_MAX {
        return Err(MenuEncodeError::TooManyPages);
    }
    let pages = drafts.len();
    let frames: Vec<Value> = drafts
        .iter()
        .enumerate()
        .map(|(i, d)| page_frame(spec, d, i + 1, pages, &ctx_used, LABEL_MAX))
        .collect();
    if frames.iter().any(|f| frame_len(f) > MAX_FRAME_BYTES) {
        return Err(MenuEncodeError::DoesNotFit);
    }
    Ok(frames)
}

/// Companion -> keyboard: revoke/close menu `id`; `msg` shows for 2 s.
pub fn menu_close_payload(id: &str, msg: &str) -> Value {
    let mut p = Map::new();
    p.insert("id".into(), Value::String(id.to_string()));
    let msg = clip_text(msg, CLOSE_MSG_MAX);
    if !msg.is_empty() {
        p.insert("msg".into(), Value::String(msg));
    }
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "menu_close", "payload": Value::Object(p) })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NoticeLevel {
    Info,
    Warn,
    Error,
}

/// Companion -> keyboard: one-off status-bar notice (3 s).
pub fn notice_payload(text: &str, level: NoticeLevel) -> Value {
    let mut p = Map::new();
    p.insert("text".into(), Value::String(clip_text(text, NOTICE_MAX)));
    match level {
        NoticeLevel::Info => {}
        NoticeLevel::Warn => {
            p.insert("level".into(), Value::String("warn".into()));
        }
        NoticeLevel::Error => {
            p.insert("level".into(), Value::String("error".into()));
        }
    }
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "notice", "payload": Value::Object(p) })
}

/// Companion -> keyboard: how many ESC'd requests wait for K4 (0 = none).
pub fn menu_parked_payload(count: usize) -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "menu_parked", "payload": { "count": count.min(PARKED_MAX) } })
}

/// Check a `menu_result` selection against the menu it answers. Returns the
/// validated indexes, or `None` when the result must be dropped: an index out
/// of range, duplicates/unsorted, or a count that does not match the select
/// mode. An empty multi-select is reported as `Some(vec![])` (= dismissed).
pub fn validate_selection(spec: &MenuSpec, selected: &[u32]) -> Option<Vec<usize>> {
    let n = spec.item_count();
    let idx: Vec<usize> = selected.iter().map(|&i| i as usize).collect();
    if idx.iter().any(|&i| i >= n) || idx.windows(2).any(|w| w[0] >= w[1]) {
        return None;
    }
    match spec.select {
        MenuSelect::Single if idx.len() == 1 => Some(idx),
        MenuSelect::Single => None,
        MenuSelect::Multi => Some(idx),
    }
}

/// Generates menu ids `<prefix><counter>` in base 36. The per-process prefix
/// keeps a stale `menu_result` from a previous Companion run from matching a
/// new menu that happens to reuse the counter.
#[derive(Debug)]
pub struct MenuIdGen {
    prefix: String,
    next: u64,
}

impl MenuIdGen {
    pub fn new(seed: u64) -> Self {
        MenuIdGen { prefix: base36(seed % (36u64.pow(4))), next: 1 }
    }
    pub fn next_id(&mut self) -> String {
        // Wraps after 36^10 ids so the id stays within ID_MAX (4 + 10 bytes).
        let id = format!("{}{}", self.prefix, base36(self.next % 36u64.pow(10)));
        self.next += 1;
        id
    }
}

fn base36(mut n: u64) -> String {
    const D: &[u8] = b"0123456789abcdefghijklmnopqrstuvwxyz";
    if n == 0 {
        return "0".into();
    }
    let mut out = Vec::new();
    while n > 0 {
        out.push(D[(n % 36) as usize]);
        n /= 36;
    }
    out.reverse();
    String::from_utf8(out).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn approval() -> MenuSpec {
        let mut s = MenuSpec::single(
            "a1b2c3",
            MenuKind::Approval,
            "Run shell?",
            vec![
                MenuItem::new("Approve once", MenuRole::Confirm),
                MenuItem::new("Reject", MenuRole::Cancel),
                MenuItem::new("Reject once", MenuRole::Danger),
            ],
        );
        s.ctx = "git push origin feat/x".into();
        s.queue = "1/3".into();
        s
    }

    #[test]
    fn approval_menu_is_one_direct_frame_with_defaults_omitted() {
        let frames = encode_menu(&approval()).unwrap();
        assert_eq!(frames.len(), 1);
        let p = &frames[0]["payload"];
        assert_eq!(frames[0]["type"], "menu");
        assert_eq!(p["id"], "a1b2c3");
        assert_eq!(p["kind"], "approval");
        assert_eq!(p["queue"], "1/3");
        assert!(p.get("page").is_none() && p.get("select").is_none() && p.get("default").is_none());
        assert_eq!(p["items"][0], serde_json::json!({"label":"Approve once","role":"confirm"}));
        assert_eq!(p["items"][2]["role"], "danger");
    }

    #[test]
    fn every_frame_fits_511_bytes_even_with_long_text() {
        let long = "x".repeat(400);
        let mut s = approval();
        s.ctx = long.clone();
        s.sections[0].title = long.clone();
        for it in &mut s.sections[0].items {
            it.label = long.clone();
            it.needs_double_confirm = true;
            it.checked = true;
            it.role = MenuRole::Attention;
        }
        s.sections[0].items.push(MenuItem::new(long.clone(), MenuRole::Attention).double_confirm().checked(true));
        let frames = encode_menu(&s).unwrap();
        assert_eq!(frames.len(), 1);
        for f in &frames {
            assert!(serde_json::to_vec(f).unwrap().len() <= MAX_FRAME_BYTES);
        }
        assert!(frames[0]["payload"]["items"][0]["label"].as_str().unwrap().ends_with("..."));
    }

    #[test]
    fn model_menu_paginates_models_then_modes_with_global_default() {
        let models: Vec<MenuItem> = (0..9)
            .map(|i| MenuItem::new(format!("claude-model-number-{i}"), MenuRole::Normal).checked(i == 2))
            .collect();
        let spec = MenuSpec {
            id: "m1".into(),
            ctx: String::new(),
            more: false,
            queue: String::new(),
            select: MenuSelect::Single,
            default: 2,
            timeout_s: 30,
            sections: vec![
                MenuSection { kind: MenuKind::Model, title: "Model".into(), items: models },
                MenuSection {
                    kind: MenuKind::Mode,
                    title: "Mode".into(),
                    items: vec![
                        MenuItem::new("Normal", MenuRole::Normal).checked(true),
                        MenuItem::new("Reads", MenuRole::Normal),
                        MenuItem::new("Trust", MenuRole::Attention).double_confirm(),
                    ],
                },
            ],
        };
        assert!(!spec.is_direct());
        let frames = encode_menu(&spec).unwrap();
        // 9 models -> 4 + 4 + 1, then the mode section on its own page.
        assert_eq!(frames.len(), 4);
        assert_eq!(frames[0]["payload"]["items"].as_array().unwrap().len(), 4);
        assert_eq!(frames[2]["payload"]["items"].as_array().unwrap().len(), 1);
        assert_eq!(frames[3]["payload"]["kind"], "mode");
        assert_eq!(frames[3]["payload"]["page"], 4);
        assert_eq!(frames[3]["payload"]["pages"], 4);
        assert_eq!(frames[0]["payload"]["default"], 2);
        assert!(frames[1]["payload"].get("default").is_none());
        assert_eq!(frames[3]["payload"]["items"][2]["needs_double_confirm"], true);
        assert_eq!(spec.item(11).unwrap().label, "Trust");
        for f in &frames {
            assert!(serde_json::to_vec(f).unwrap().len() <= MAX_FRAME_BYTES);
            assert!(f["payload"]["items"].as_array().unwrap().len() <= ITEMS_PER_PAGE_MAX);
        }
    }

    #[test]
    fn more_flag_and_parked_frame() {
        let mut s = approval();
        s.ctx.clear();
        s.more = true;
        let f = encode_menu(&s).unwrap();
        assert_eq!(f[0]["payload"]["more"], true);
        assert!(f[0]["payload"].get("ctx").is_none());
        assert!(encode_menu(&approval()).unwrap()[0]["payload"].get("more").is_none());
        let p = menu_parked_payload(2);
        assert_eq!(p["type"], "menu_parked");
        assert_eq!(p["payload"]["count"], 2);
        assert_eq!(menu_parked_payload(500)["payload"]["count"], PARKED_MAX);
    }

    #[test]
    fn oversized_page_moves_items_to_the_next_page() {
        // Non-ASCII labels at the byte cap: 4 per page would exceed 511 with ctx.
        let items: Vec<MenuItem> = (0..6)
            .map(|_| MenuItem::new("确认确认确认确认", MenuRole::Attention).double_confirm().checked(true))
            .collect();
        let mut spec = MenuSpec::single("q1", MenuKind::Question, "Pick", items);
        spec.select = MenuSelect::Multi;
        spec.ctx = "c".repeat(120);
        let frames = encode_menu(&spec).unwrap();
        assert!(frames.len() >= 2);
        let total: usize = frames.iter().map(|f| f["payload"]["items"].as_array().unwrap().len()).sum();
        assert_eq!(total, 6);
        for f in &frames {
            assert!(serde_json::to_vec(f).unwrap().len() <= MAX_FRAME_BYTES);
            assert_eq!(f["payload"]["select"], "multi");
        }
    }

    #[test]
    fn rejects_bad_ids_empty_and_too_many() {
        let mut s = approval();
        s.id = "Bad-Id".into();
        assert_eq!(encode_menu(&s), Err(MenuEncodeError::BadId));
        s.id = "a".repeat(16);
        assert_eq!(encode_menu(&s), Err(MenuEncodeError::BadId));
        s.id = "ok".into();
        s.sections[0].items.clear();
        assert_eq!(encode_menu(&s), Err(MenuEncodeError::Empty));
        s.sections[0].items = vec![MenuItem::new("x", MenuRole::Normal); 49];
        assert_eq!(encode_menu(&s), Err(MenuEncodeError::TooManyItems));
        s.sections = (0..13)
            .map(|_| MenuSection { kind: MenuKind::Options, title: "t".into(), items: vec![MenuItem::new("x", MenuRole::Normal)] })
            .collect();
        assert_eq!(encode_menu(&s), Err(MenuEncodeError::TooManyPages));
    }

    #[test]
    fn clip_respects_utf8_boundaries() {
        assert_eq!(clip_text("abc", 24), "abc");
        let c = clip_text("中文中文中文中文中文", 10);
        assert!(c.len() <= 10 && c.ends_with("..."));
        assert_eq!(c, "中文...");
    }

    #[test]
    fn close_and_notice_frames() {
        let c = menu_close_payload("a1", "Approved");
        assert_eq!(c["type"], "menu_close");
        assert_eq!(c["payload"], serde_json::json!({"id":"a1","msg":"Approved"}));
        assert!(menu_close_payload("a1", "")["payload"].get("msg").is_none());
        let n = notice_payload(&"n".repeat(80), NoticeLevel::Warn);
        assert_eq!(n["payload"]["level"], "warn");
        assert!(n["payload"]["text"].as_str().unwrap().len() <= NOTICE_MAX);
        assert!(notice_payload("hi", NoticeLevel::Info)["payload"].get("level").is_none());
    }

    #[test]
    fn selection_validation() {
        let s = approval();
        assert_eq!(validate_selection(&s, &[1]), Some(vec![1]));
        assert_eq!(validate_selection(&s, &[3]), None);
        assert_eq!(validate_selection(&s, &[]), None);
        assert_eq!(validate_selection(&s, &[0, 1]), None);
        let mut m = s.clone();
        m.select = MenuSelect::Multi;
        assert_eq!(validate_selection(&m, &[0, 2]), Some(vec![0, 2]));
        assert_eq!(validate_selection(&m, &[2, 0]), None);
        assert_eq!(validate_selection(&m, &[1, 1]), None);
        assert_eq!(validate_selection(&m, &[]), Some(vec![]));
    }

    #[test]
    fn id_gen_is_valid_and_unique() {
        let mut g = MenuIdGen::new(123_456_789);
        let a = g.next_id();
        let b = g.next_id();
        assert_ne!(a, b);
        assert!(valid_menu_id(&a) && valid_menu_id(&b));
        let mut big = MenuIdGen { prefix: base36(36u64.pow(4) - 1), next: 36u64.pow(10) - 1 };
        assert!(big.next_id().len() <= ID_MAX);
    }
}
