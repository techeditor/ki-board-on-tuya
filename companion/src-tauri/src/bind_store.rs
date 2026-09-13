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

/// Number of bindable slots (K1, K2, K3).
pub const SLOT_COUNT: usize = 3;

/// Default chat number for slot `i` (0-based): K1->1, K2->2, K3->3.
fn default_chats() -> Vec<u8> {
    vec![1, 2, 3]
}

/// The persisted per-key chat-number store. `chats[i]` is the chat number
/// (`1..=9`) that K(i+1) jumps to. Length is exactly `SLOT_COUNT` after load.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BindStore {
    /// Fixed-length: index 0 = slot 1 (K1), … index 2 = slot 3 (K3).
    #[serde(default = "default_chats")]
    pub chats: Vec<u8>,
}

impl Default for BindStore {
    fn default() -> Self {
        Self { chats: default_chats() }
    }
}

impl BindStore {
    /// A store with the default chat numbers (`[1, 2, 3]`).
    pub fn new() -> Self {
        Self::default()
    }

    /// Ensure the chats vector is exactly `SLOT_COUNT` long, filling any missing
    /// slot with its default number and clamping every entry to `1..=9`. Called
    /// after load (tolerant to a truncated / over-long / out-of-range file).
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

    /// Reset slot `n` (1-based) to its default number (slot index + 1).
    /// Out-of-range is a no-op.
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
    fn new_store_has_default_chat_numbers() {
        let s = BindStore::new();
        assert_eq!(s.chats, vec![1, 2, 3]);
        assert_eq!(s.get(1), Some(1));
        assert_eq!(s.get(2), Some(2));
        assert_eq!(s.get(3), Some(3));
    }

    #[test]
    fn set_get_clear_round_trip() {
        let mut s = BindStore::new();
        s.set(2, 7);
        assert_eq!(s.get(2), Some(7));
        // Slots 1 and 3 keep their defaults.
        assert_eq!(s.get(1), Some(1));
        assert_eq!(s.get(3), Some(3));

        // Clear resets slot 2 to its default (index+1 = 2).
        s.clear(2);
        assert_eq!(s.get(2), Some(2));
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
        let json = serde_json::to_string(&s).unwrap();
        assert_eq!(json, r#"{"chats":[4,2,8]}"#);
        let back: BindStore = serde_json::from_str(&json).unwrap();
        assert_eq!(s, back);
    }

    #[test]
    fn load_normalizes_short_and_long_and_out_of_range_vectors() {
        // Truncated (1 entry) -> padded to SLOT_COUNT with defaults.
        let mut short: BindStore = serde_json::from_str(r#"{"chats":[5]}"#).unwrap();
        short.normalize();
        assert_eq!(short.chats, vec![5, 2, 3]);

        // Over-long (5 entries) -> truncated to SLOT_COUNT.
        let mut long: BindStore =
            serde_json::from_str(r#"{"chats":[9,8,7,6,5]}"#).unwrap();
        long.normalize();
        assert_eq!(long.chats, vec![9, 8, 7]);

        // Out-of-range entries -> replaced with each slot's default.
        let mut bad: BindStore = serde_json::from_str(r#"{"chats":[0,10,4]}"#).unwrap();
        bad.normalize();
        assert_eq!(bad.chats, vec![1, 2, 4]);
    }

    #[test]
    fn empty_object_deserializes_to_defaults() {
        // Missing `chats` -> serde default -> [1,2,3]; normalize keeps it.
        let mut s: BindStore = serde_json::from_str("{}").unwrap();
        assert_eq!(s.chats, vec![1, 2, 3]);
        s.normalize();
        assert_eq!(s.chats.len(), SLOT_COUNT);
        assert_eq!(s.chats, vec![1, 2, 3]);
    }
}
