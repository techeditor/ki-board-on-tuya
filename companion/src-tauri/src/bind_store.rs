//! FP-4b Milestone 4 — per-conversation bind store (§3.2).
//!
//! Three key slots (K1-K3) each hold an optional bound Kiro Crew conversation.
//! The PRIMARY handle is `label` — the conversation's TITLE — which the focus
//! path types into the SPA's CLIENT-SIDE command bar (Ctrl+K), navigating in-app
//! with NO page reload (the dashboard SPA is history/pushState, so address-bar
//! navigation reloads, but its own Command Bar does not). `url` is retained as
//! an optional manual navigate fallback (`/chat/<slug>?sid=<slug>`). Short-press
//! a slot focuses its conversation; long-press binds the CURRENT conversation
//! (captured from the window title).
//!
//! This module is **pure + cross-platform**: it (de)serializes and persists the
//! store to `config_dir/ak-companion/binds.json` (reusing the `dirs` crate the
//! status-sink handoff already uses), and provides a pure `parse_chat_slug`
//! that extracts a `/chat/<slug>` URL out of arbitrary text (used by the manual
//! URL fallback). No Tauri / Windows deps, so it `cargo test`s on the Linux host.
//!
//! Design ref: `.kiro/specs/feature-probes-kirocrew-local-control/design.md`
//! ("Milestone 4 — per-conversation binding").

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// Number of bindable slots (K1, K2, K3).
pub const SLOT_COUNT: usize = 3;

/// One bind slot. The PRIMARY handle is `label` — the conversation TITLE, used
/// for client-side command-bar focus (no reload). `url` is an optional manual
/// navigate fallback. Empty (both `None`/blank) means "unbound" — a short press
/// then falls back to creating a new chat (design M4: "empty slot -> new session").
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Slot {
    /// Navigable conversation URL, e.g. `http://127.0.0.1:5476/chat/<slug>?sid=<slug>`.
    /// Optional manual fallback; the primary focus path uses `label` instead.
    #[serde(default)]
    pub url: Option<String>,
    /// The conversation TITLE — typed into the client-side command bar to focus
    /// the conversation (no reload), and shown on the board LCD / UI.
    #[serde(default)]
    pub label: Option<String>,
}

impl Slot {
    /// Is this slot bound to a conversation? True when EITHER the label (title,
    /// the primary focus handle) or the url (manual fallback) is non-empty.
    pub fn is_bound(&self) -> bool {
        let non_empty = |o: &Option<String>| o.as_deref().map(|s| !s.trim().is_empty()).unwrap_or(false);
        non_empty(&self.label) || non_empty(&self.url)
    }
}

/// The persisted three-slot bind store.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct BindStore {
    /// Fixed-length: index 0 = slot 1 (K1), … index 2 = slot 3 (K3).
    #[serde(default)]
    pub slots: Vec<Slot>,
}

impl BindStore {
    /// A store with `SLOT_COUNT` empty slots.
    pub fn new() -> Self {
        Self {
            slots: vec![Slot::default(); SLOT_COUNT],
        }
    }

    /// Ensure the slot vector is exactly `SLOT_COUNT` long (tolerant to a
    /// truncated / over-long file). Called after load.
    fn normalize(&mut self) {
        if self.slots.len() < SLOT_COUNT {
            self.slots.resize(SLOT_COUNT, Slot::default());
        } else if self.slots.len() > SLOT_COUNT {
            self.slots.truncate(SLOT_COUNT);
        }
    }

    /// Read slot `n` (1-based). Returns `None` for an out-of-range slot.
    pub fn get(&self, n: u8) -> Option<&Slot> {
        idx(n).and_then(|i| self.slots.get(i))
    }

    /// Bind slot `n` (1-based) to `url` + `label`. Out-of-range is a no-op.
    pub fn set(&mut self, n: u8, url: Option<String>, label: Option<String>) {
        if let Some(i) = idx(n) {
            if let Some(slot) = self.slots.get_mut(i) {
                slot.url = url;
                slot.label = label;
            }
        }
    }

    /// Clear slot `n` (1-based). Out-of-range is a no-op.
    pub fn clear(&mut self, n: u8) {
        if let Some(i) = idx(n) {
            if let Some(slot) = self.slots.get_mut(i) {
                *slot = Slot::default();
            }
        }
    }

    /// Load the store from disk, or a fresh empty store when the file is absent
    /// / unreadable / malformed (never fails the caller — a missing bind file
    /// is normal on first run).
    pub fn load() -> Self {
        let mut store = binds_path()
            .and_then(|p| std::fs::read_to_string(p).ok())
            .and_then(|s| serde_json::from_str::<BindStore>(&s).ok())
            .unwrap_or_else(BindStore::new);
        store.normalize();
        store
    }

    /// Persist the store to `config_dir/ak-companion/binds.json`, creating the
    /// parent directory as needed.
    pub fn save(&self) -> Result<(), String> {
        let path = binds_path().ok_or("cannot resolve config dir for bind store")?;
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| format!("create {}: {e}", parent.display()))?;
        }
        let body = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(&path, body).map_err(|e| format!("write {}: {e}", path.display()))
    }
}

/// Map a 1-based slot number to a 0-based index within `0..SLOT_COUNT`.
fn idx(n: u8) -> Option<usize> {
    let i = (n as usize).checked_sub(1)?;
    (i < SLOT_COUNT).then_some(i)
}

/// Path of the bind file: `<config-dir>/ak-companion/binds.json`. On Windows
/// that resolves under `%LOCALAPPDATA%`, else `~/.config`; falls back to
/// `$HOME/.config` when the platform config dir is unavailable — matching the
/// status-sink handoff resolution.
pub fn binds_path() -> Option<PathBuf> {
    let base = dirs::config_dir().or_else(|| {
        std::env::var("HOME")
            .ok()
            .filter(|h| !h.trim().is_empty())
            .map(|h| PathBuf::from(h).join(".config"))
    })?;
    Some(base.join("ak-companion").join("binds.json"))
}

// ─────────────────────── parse_chat_slug (pure) ───────────────────────

/// Extract a Kiro Crew chat URL + slug from arbitrary pasted/clipboard text.
///
/// The chat URL scheme (design M4) is `/chat/<slug>` with an optional
/// `?sid=<slug>` query. `text` may be a bare URL, a full `http(s)://host/...`
/// URL, or arbitrary text containing one (e.g. the clipboard after Ctrl+L /
/// Ctrl+A / Ctrl+C over a browser address bar). Returns `(url, slug)`:
/// * `url`  — the captured URL as-is (trimmed) when the input already looks
///            like a URL, else a normalized `http://127.0.0.1:<...>` cannot be
///            inferred so the raw matched substring is returned.
/// * `slug` — the `<slug>` path segment (used as the human label).
///
/// Returns `None` when no `/chat/<slug>` pattern is present.
pub fn parse_chat_slug(text: &str) -> Option<(String, String)> {
    let text = text.trim();
    if text.is_empty() {
        return None;
    }
    // Find the `/chat/` marker; the slug is the following path segment, ending
    // at the next `/`, `?`, `#`, or whitespace.
    let marker = "/chat/";
    let start = text.find(marker)? + marker.len();
    let rest = &text[start..];
    let slug_end = rest
        .find(|c: char| c == '/' || c == '?' || c == '#' || c.is_whitespace())
        .unwrap_or(rest.len());
    let slug = &rest[..slug_end];
    if slug.is_empty() {
        return None;
    }

    // The URL is the whole matched token: walk left from the marker to a
    // whitespace boundary (so a URL embedded in surrounding text is isolated),
    // and right to the end of the URL (whitespace terminates it).
    let marker_pos = start - marker.len();
    let url_start = text[..marker_pos]
        .rfind(char::is_whitespace)
        .map(|p| p + 1)
        .unwrap_or(0);
    let url_end = text[start..]
        .find(char::is_whitespace)
        .map(|p| start + p)
        .unwrap_or(text.len());
    let url = text[url_start..url_end].trim();
    if url.is_empty() {
        return None;
    }
    Some((url.to_string(), slug.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Slot / store get/set/clear ───────────────────────────

    #[test]
    fn new_store_has_three_empty_slots() {
        let s = BindStore::new();
        assert_eq!(s.slots.len(), SLOT_COUNT);
        assert!(s.slots.iter().all(|sl| !sl.is_bound()));
    }

    #[test]
    fn set_get_clear_round_trip() {
        let mut s = BindStore::new();
        s.set(2, Some("http://127.0.0.1:5476/chat/abc".into()), Some("abc".into()));
        let slot = s.get(2).expect("slot 2");
        assert!(slot.is_bound());
        assert_eq!(slot.url.as_deref(), Some("http://127.0.0.1:5476/chat/abc"));
        assert_eq!(slot.label.as_deref(), Some("abc"));
        // Slots 1 and 3 stay empty.
        assert!(!s.get(1).unwrap().is_bound());
        assert!(!s.get(3).unwrap().is_bound());

        s.clear(2);
        assert!(!s.get(2).unwrap().is_bound());
    }

    #[test]
    fn label_only_slot_is_bound() {
        // Title-primary bind: label set, url None -> still bound (command-bar path).
        let mut s = BindStore::new();
        s.set(1, None, Some("My chat".into()));
        let slot = s.get(1).expect("slot 1");
        assert!(slot.is_bound());
        assert!(slot.url.is_none());
        assert_eq!(slot.label.as_deref(), Some("My chat"));
        // Blank label/url is NOT bound.
        s.set(3, Some("  ".into()), Some("  ".into()));
        assert!(!s.get(3).unwrap().is_bound());
    }

    #[test]
    fn out_of_range_slot_is_none_or_noop() {
        let mut s = BindStore::new();
        assert!(s.get(0).is_none());
        assert!(s.get(4).is_none());
        // set/clear on an out-of-range slot must not panic and must not grow.
        s.set(0, Some("x".into()), None);
        s.set(9, Some("x".into()), None);
        s.clear(0);
        assert_eq!(s.slots.len(), SLOT_COUNT);
    }

    // ── (de)serialize ───────────────────────────────────────

    #[test]
    fn store_json_round_trips() {
        let mut s = BindStore::new();
        s.set(1, Some("http://127.0.0.1:5476/chat/one".into()), Some("one".into()));
        s.set(3, Some("http://127.0.0.1:5476/chat/three".into()), Some("three".into()));
        let json = serde_json::to_string(&s).unwrap();
        let back: BindStore = serde_json::from_str(&json).unwrap();
        assert_eq!(s, back);
    }

    #[test]
    fn load_normalizes_short_and_long_slot_vectors() {
        // Truncated (1 slot) -> padded to SLOT_COUNT.
        let short: BindStore = serde_json::from_str(r#"{"slots":[{"url":"u","label":"l"}]}"#).unwrap();
        let mut short = short;
        short.normalize();
        assert_eq!(short.slots.len(), SLOT_COUNT);
        assert_eq!(short.slots[0].url.as_deref(), Some("u"));

        // Over-long (5 slots) -> truncated to SLOT_COUNT.
        let long: BindStore = serde_json::from_str(
            r#"{"slots":[{},{},{},{},{}]}"#,
        )
        .unwrap();
        let mut long = long;
        long.normalize();
        assert_eq!(long.slots.len(), SLOT_COUNT);
    }

    #[test]
    fn empty_object_deserializes_to_no_slots_then_normalizes() {
        let mut s: BindStore = serde_json::from_str("{}").unwrap();
        assert!(s.slots.is_empty());
        s.normalize();
        assert_eq!(s.slots.len(), SLOT_COUNT);
    }

    // ── parse_chat_slug ─────────────────────────────────────

    #[test]
    fn parse_full_url_with_sid() {
        let (url, slug) =
            parse_chat_slug("http://127.0.0.1:5476/chat/my-slug?sid=my-slug").unwrap();
        assert_eq!(url, "http://127.0.0.1:5476/chat/my-slug?sid=my-slug");
        assert_eq!(slug, "my-slug");
    }

    #[test]
    fn parse_url_without_sid() {
        let (url, slug) = parse_chat_slug("http://localhost:1420/chat/abc123").unwrap();
        assert_eq!(url, "http://localhost:1420/chat/abc123");
        assert_eq!(slug, "abc123");
    }

    #[test]
    fn parse_slug_from_surrounding_text() {
        // Clipboard often carries trailing/leading whitespace or context.
        let (url, slug) =
            parse_chat_slug("  see http://127.0.0.1:5476/chat/deadbeef?sid=deadbeef here ")
                .unwrap();
        assert_eq!(url, "http://127.0.0.1:5476/chat/deadbeef?sid=deadbeef");
        assert_eq!(slug, "deadbeef");
    }

    #[test]
    fn parse_slug_stops_at_trailing_path_and_fragment() {
        let (_, slug) = parse_chat_slug("http://h/chat/xyz/extra").unwrap();
        assert_eq!(slug, "xyz");
        let (_, slug2) = parse_chat_slug("http://h/chat/xyz#frag").unwrap();
        assert_eq!(slug2, "xyz");
    }

    #[test]
    fn parse_bare_relative_path() {
        let (url, slug) = parse_chat_slug("/chat/rel-slug?sid=rel-slug").unwrap();
        assert_eq!(url, "/chat/rel-slug?sid=rel-slug");
        assert_eq!(slug, "rel-slug");
    }

    #[test]
    fn parse_returns_none_without_chat_path() {
        assert!(parse_chat_slug("").is_none());
        assert!(parse_chat_slug("http://127.0.0.1:5476/").is_none());
        assert!(parse_chat_slug("just some text").is_none());
        // `/chat/` with an empty slug (trailing slash) is not a valid bind.
        assert!(parse_chat_slug("http://h/chat/").is_none());
    }
}
