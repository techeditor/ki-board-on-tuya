//! FP-4b Milestone 4 — per-key chat-number bind store (§3.2, simplified).
//!
//! Each of the three slot keys (K1-K3) binds to a Kiro Crew chat NUMBER
//! (`1..=9`, the digit in Kiro Crew's "Alt+N = Jump to chat N" shortcut).
//! Pressing the key fires `Alt+<that number>` — a deterministic, client-side,
//! no-reload jump. The number is user-configurable per key.
//!
//! This module is **pure + cross-platform**: it (de)serializes and persists the
//! store to `config_dir/ak-companion/binds.json` (reusing the `dirs` crate the
//! status-sink handoff already uses). No Tauri / Windows deps, so it `cargo
//! test`s on the Linux host.
//!
//! Design ref: `.kiro/specs/feature-probes-kirocrew-local-control/design.md`
//! ("Milestone 4 — per-conversation binding").

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// Number of bindable slots (K1, K2, K3), named A, B, and C in the UI.
pub const SLOT_COUNT: usize = 3;
/// Maximum alias width that fits the AK's 64-pixel sidebar (7 × 8px glyphs).
pub const ALIAS_MAX_CHARS: usize = 7;

/// Default chat number for slot `i` (0-based): K1->1, K2->2, K3->3.
fn default_chats() -> Vec<u8> {
    vec![1, 2, 3]
}

/// Empty persisted aliases mean that the AK displays the defaults A, B, and C.
fn default_aliases() -> Vec<String> {
    vec![String::new(); SLOT_COUNT]
}

/// Empty session ids mean no observed Crew chat is associated with the slot.
fn default_sessions() -> Vec<String> {
    vec![String::new(); SLOT_COUNT]
}

/// The persisted per-key binding store. `chats[i]` is the chat number,
/// `aliases[i]` its optional AK label, and `sessions[i]` the Crew session shown
/// beside that slot in the Companion. All vectors have `SLOT_COUNT` entries.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BindStore {
    /// Index 0..=2 maps K1..=K3 to the A/B/C slots.
    #[serde(default = "default_chats")]
    pub chats: Vec<u8>,
    /// Optional display aliases; an empty entry falls back to A, B, or C.
    #[serde(default = "default_aliases")]
    pub aliases: Vec<String>,
    /// Optional stable Crew session ids used only for Companion presentation.
    #[serde(default = "default_sessions")]
    pub sessions: Vec<String>,
}

impl Default for BindStore {
    fn default() -> Self {
        Self {
            chats: default_chats(),
            aliases: default_aliases(),
            sessions: default_sessions(),
        }
    }
}

impl BindStore {
    /// A store with default chat numbers and no custom aliases.
    pub fn new() -> Self {
        Self::default()
    }

    /// Ensure both vectors are exactly `SLOT_COUNT` long and all values are
    /// valid. Malformed persisted aliases fall back to their default A/B/C
    /// labels rather than leaking unsupported text to the AK display.
    fn normalize(&mut self) {
        let defaults = default_chats();
        if self.chats.len() < SLOT_COUNT {
            for i in self.chats.len()..SLOT_COUNT {
                self.chats.push(defaults[i]);
            }
        } else if self.chats.len() > SLOT_COUNT {
            self.chats.truncate(SLOT_COUNT);
        }
        for (i, n) in self.chats.iter_mut().enumerate() {
            if !(1..=9).contains(n) {
                *n = defaults[i];
            }
        }

        self.aliases.resize(SLOT_COUNT, String::new());
        self.aliases.truncate(SLOT_COUNT);
        for alias in &mut self.aliases {
            *alias = normalize_alias(alias).unwrap_or_default();
        }

        self.sessions.resize(SLOT_COUNT, String::new());
        self.sessions.truncate(SLOT_COUNT);
        for session in &mut self.sessions {
            *session = normalize_session_id(session).unwrap_or_default();
        }
    }

    /// Read the chat number for slot `n` (1-based). Returns `None` for an
    /// out-of-range slot.
    pub fn get(&self, n: u8) -> Option<u8> {
        idx(n).and_then(|i| self.chats.get(i).copied())
    }

    /// Set slot `n` (1-based) to chat `number`, clamped to `1..=9`. Out-of-range
    /// slot is a no-op; an out-of-range number is clamped into `1..=9`.
    pub fn set(&mut self, n: u8, number: u8) {
        if let Some(i) = idx(n) {
            if let Some(slot) = self.chats.get_mut(i) {
                *slot = number.clamp(1, 9);
            }
        }
    }

    /// Set an optional display alias for slot A, B, or C. Empty input clears the
    /// alias. The AK font is ASCII-only and the sidebar fits at most 7 glyphs.
    pub fn set_alias(&mut self, n: u8, alias: &str) -> Result<(), String> {
        let i = idx(n).ok_or_else(|| format!("binding slot must be 1..={SLOT_COUNT}"))?;
        self.aliases[i] = normalize_alias(alias)?;
        Ok(())
    }

    /// Associate an observed Crew session with a slot for Companion display.
    /// This does not alter the physical shortcut; `chats` stays authoritative.
    pub fn set_session(&mut self, n: u8, session_id: &str) -> Result<(), String> {
        let i = idx(n).ok_or_else(|| format!("binding slot must be 1..={SLOT_COUNT}"))?;
        self.sessions[i] = normalize_session_id(session_id)?;
        Ok(())
    }

    /// Remove only the UI session association for a slot.
    pub fn clear_session(&mut self, n: u8) -> Result<(), String> {
        self.set_session(n, "")
    }

    /// Reset slot `n` (1-based) to its default chat number (slot index + 1).
    /// Out-of-range is a no-op. Its alias and session association are preserved.
    pub fn clear(&mut self, n: u8) {
        if let Some(i) = idx(n) {
            if let Some(slot) = self.chats.get_mut(i) {
                *slot = default_chats()[i];
            }
        }
    }

    /// Load the store from disk, or a fresh default store when the file is
    /// absent / unreadable / malformed (never fails the caller — a missing bind
    /// file is normal on first run). Always normalized to `SLOT_COUNT`.
    pub fn load() -> Self {
        let mut store = binds_path()
            .and_then(|p| std::fs::read_to_string(p).ok())
            .and_then(|s| serde_json::from_str::<BindStore>(&s).ok())
            .unwrap_or_default();
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

fn normalize_session_id(session_id: &str) -> Result<String, String> {
    let session_id = session_id.trim();
    if session_id.chars().any(char::is_control) {
        return Err("session id must not contain control characters".to_string());
    }
    if session_id.chars().count() > 512 {
        return Err("session id must be at most 512 characters".to_string());
    }
    Ok(session_id.to_string())
}

fn normalize_alias(alias: &str) -> Result<String, String> {
    let alias = alias.trim();
    if !alias.is_ascii() || !alias.bytes().all(|b| (0x20..=0x7e).contains(&b)) {
        return Err("alias must contain displayable ASCII characters only".to_string());
    }
    if alias.len() > ALIAS_MAX_CHARS {
        return Err(format!(
            "alias must be at most {ALIAS_MAX_CHARS} characters"
        ));
    }
    Ok(alias.to_string())
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

#[cfg(test)]
mod tests {
    use super::*;

    // ── default / get / set / clear ──────────────────────────

    #[test]
    fn new_store_has_default_chat_numbers_and_empty_aliases() {
        let s = BindStore::new();
        assert_eq!(s.chats, vec![1, 2, 3]);
        assert_eq!(s.aliases, vec!["", "", ""]);
        assert_eq!(s.sessions, vec!["", "", ""]);
        assert_eq!(s.get(1), Some(1));
        assert_eq!(s.get(2), Some(2));
        assert_eq!(s.get(3), Some(3));
    }

    #[test]
    fn set_get_clear_round_trip() {
        let mut s = BindStore::new();
        s.set(2, 7);
        assert_eq!(s.get(2), Some(7));
        // Slots A and C keep their defaults.
        assert_eq!(s.get(1), Some(1));
        assert_eq!(s.get(3), Some(3));

        // Clear resets slot B's chat number but preserves its display alias.
        s.set_alias(2, " Build ").unwrap();
        s.clear(2);
        assert_eq!(s.get(2), Some(2));
        assert_eq!(s.aliases[1], "Build");
    }

    #[test]
    fn alias_validation_matches_ak_display_limits() {
        let mut s = BindStore::new();
        s.set_alias(1, "Plan-1").unwrap();
        assert_eq!(s.aliases[0], "Plan-1");
        s.set_alias(1, "   ").unwrap();
        assert_eq!(s.aliases[0], "");
        assert!(s.set_alias(2, "12345678").is_err());
        assert!(s.set_alias(2, "中文").is_err());
        assert!(s.set_alias(4, "Build").is_err());
    }

    #[test]
    fn session_association_round_trips_and_clears() {
        let mut s = BindStore::new();
        s.set_session(2, " dashboard:chat-2 ").unwrap();
        assert_eq!(s.sessions[1], "dashboard:chat-2");
        s.clear_session(2).unwrap();
        assert_eq!(s.sessions[1], "");
        assert!(s.set_session(4, "other").is_err());
        assert!(s.set_session(1, "bad\nsession").is_err());
    }

    #[test]
    fn set_clamps_number_into_1_9() {
        let mut s = BindStore::new();
        s.set(1, 0);
        assert_eq!(s.get(1), Some(1)); // 0 -> clamped up to 1
        s.set(1, 42);
        assert_eq!(s.get(1), Some(9)); // 42 -> clamped down to 9
        s.set(1, 9);
        assert_eq!(s.get(1), Some(9));
    }

    #[test]
    fn out_of_range_slot_is_none_or_noop() {
        let mut s = BindStore::new();
        assert!(s.get(0).is_none());
        assert!(s.get(4).is_none());
        // set/clear on an out-of-range slot must not panic and must not grow.
        s.set(0, 5);
        s.set(9, 5);
        s.clear(0);
        assert_eq!(s.chats.len(), SLOT_COUNT);
        assert_eq!(s.chats, vec![1, 2, 3]);
    }

    // ── (de)serialize ───────────────────────────────────────

    #[test]
    fn store_json_round_trips() {
        let mut s = BindStore::new();
        s.set(1, 4);
        s.set(3, 8);
        s.set_alias(1, "Plan").unwrap();
        s.set_session(1, "dashboard:chat-4").unwrap();
        let json = serde_json::to_string(&s).unwrap();
        assert_eq!(
            json,
            r#"{"chats":[4,2,8],"aliases":["Plan","",""],"sessions":["dashboard:chat-4","",""]}"#
        );
        let back: BindStore = serde_json::from_str(&json).unwrap();
        assert_eq!(s, back);
    }

    #[test]
    fn load_normalizes_vectors_and_migrates_old_files() {
        // Old files have no aliases; serde defaults them to three empty entries.
        let mut old: BindStore = serde_json::from_str(r#"{"chats":[5]}"#).unwrap();
        old.normalize();
        assert_eq!(old.chats, vec![5, 2, 3]);
        assert_eq!(old.aliases, vec!["", "", ""]);
        assert_eq!(old.sessions, vec!["", "", ""]);

        let mut long: BindStore =
            serde_json::from_str(r#"{"chats":[9,8,7,6],"aliases":["One","Two","Three","Four"]}"#)
                .unwrap();
        long.normalize();
        assert_eq!(long.chats, vec![9, 8, 7]);
        assert_eq!(long.aliases, vec!["One", "Two", "Three"]);
        assert_eq!(long.sessions, vec!["", "", ""]);

        // Invalid chat numbers and aliases both fall back safely.
        let mut bad: BindStore =
            serde_json::from_str(r#"{"chats":[0,10,4],"aliases":["12345678","中文","Ok"]}"#)
                .unwrap();
        bad.normalize();
        assert_eq!(bad.chats, vec![1, 2, 4]);
        assert_eq!(bad.aliases, vec!["", "", "Ok"]);
        assert_eq!(bad.sessions, vec!["", "", ""]);
    }

    #[test]
    fn empty_object_deserializes_to_defaults() {
        let mut s: BindStore = serde_json::from_str("{}").unwrap();
        s.normalize();
        assert_eq!(s.chats, vec![1, 2, 3]);
        assert_eq!(s.aliases, vec!["", "", ""]);
        assert_eq!(s.sessions, vec!["", "", ""]);
    }
}
