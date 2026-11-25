//! FP-4b Milestone 4 — per-key chat-number bind store (§3.2, simplified).
//!
//! Each of the three slot keys (K1-K3) binds to a Kiro Crew chat NUMBER
//! (`1..=9`, the digit in Kiro Crew's "Alt+N = Jump to chat N" shortcut).
//! Pressing the key fires `Alt+<that number>` — a deterministic, client-side,
//! no-reload jump. The number is user-configurable per key.
//!
//! This module is **pure + cross-platform**: it (de)serializes and persists the
//! store to `~/.ak-companion/binds.json` (see the `app_dirs` module, shared with
//! the status-sink handoff and the diagnostic logs). No Tauri / Windows deps, so
//! it `cargo test`s on the Linux host.

use std::collections::BTreeMap;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

/// Most chats whose last hand-picked model is remembered; the oldest-sorted
/// keys beyond this are dropped so the file cannot grow without bound.
/// shortcut: drops by key order, not by age; track last use if users exceed it.
pub const LAST_MODELS_MAX: usize = 64;

/// Number of bindable slots (K1, K2, K3), named A, B, and C in the UI.
pub const SLOT_COUNT: usize = 3;
/// Maximum alias width that fits the AK's 64-pixel sidebar (7 × 8px glyphs).
pub const ALIAS_MAX_CHARS: usize = 7;

/// Default chat number for slot `i` (0-based): K1->1, K2->2, K3->3.
fn default_chats() -> Vec<u8> {
    // 0 = "not set": pressing the key does NOT jump the Kiro Crew view. A
    // value in 1..=9 means "also jump to that Alt+N on click" (a bonus on top
    // of the stable-slot selection). Default is unset so nothing jumps until
    // the user opts in per slot.
    vec![0, 0, 0]
}

/// Empty persisted aliases mean that the AK displays the defaults A, B, and C.
fn default_aliases() -> Vec<String> {
    vec![String::new(); SLOT_COUNT]
}

/// Empty session ids mean no observed Crew chat is associated with the slot.
fn default_sessions() -> Vec<String> {
    vec![String::new(); SLOT_COUNT]
}

/// Instance id of this machine's Crew in a binding (`local:<slot key>`).
pub const LOCAL_INSTANCE: &str = "local";

/// The stored form of a session binding: `<instance id>:<slot key>`, the
/// same string the Crew Hub stamps as `row_identity` on a remote crew's rows,
/// and `local:<slot key>` for this machine. Older files hold a bare slot key
/// and Crew hooks report `dashboard:<slot key>`; both mean this machine.
/// Slot keys never contain `:`, so the first `:` splits the two parts.
/// shortcut: a remote crew whose id is `local` or `dashboard` would be read as
/// this machine; Crew derives ids from names, rename such a crew if it occurs.
pub fn row_identity(id: &str) -> String {
    let id = id.trim();
    match id.split_once(':') {
        _ if id.is_empty() => String::new(),
        None => format!("{LOCAL_INSTANCE}:{id}"),
        Some(("dashboard", key)) => format!("{LOCAL_INSTANCE}:{key}"),
        Some(_) => id.to_string(),
    }
}

/// `(instance id, slot key)` of a row identity; `None` when either part is empty.
pub fn split_row(row: &str) -> Option<(&str, &str)> {
    row.split_once(':').filter(|(inst, key)| !inst.is_empty() && !key.is_empty())
}

/// The id the rest of the Companion keys sessions by: the bare slot key for
/// this machine (Crew API and status events), the row identity for a remote
/// crew's session.
pub fn status_id(row: &str) -> String {
    match split_row(row) {
        Some((LOCAL_INSTANCE, key)) => key.to_string(),
        _ => row.to_string(),
    }
}

/// The persisted Agent Status and per-key binding store. `chats[i]` is the
/// chat number, `aliases[i]` its optional AK label, and `sessions[i]` the Crew
/// session shown beside that slot in the Companion. All vectors have
/// `SLOT_COUNT` entries. `pinned_session` is the optional Agent Status Watch.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct BindStore {
    /// Index 0..=2 maps K1..=K3 to the A/B/C slots.
    #[serde(default = "default_chats")]
    pub chats: Vec<u8>,
    /// Optional display aliases; an empty entry falls back to A, B, or C.
    #[serde(default = "default_aliases")]
    pub aliases: Vec<String>,
    /// The Crew session bound to each slot as a row identity
    /// (`local:<key>` / `<instance id>:<key>`, see [`row_identity`]); empty =
    /// not bound.
    #[serde(default = "default_sessions")]
    pub sessions: Vec<String>,
    /// Display name of each bound session (its title when it was bound),
    /// shown on the keyboard's Bind session page.
    #[serde(default = "default_sessions")]
    pub names: Vec<String>,
    /// Optional Agent Status session pinned to the keyboard screen.
    #[serde(default)]
    pub pinned_session: String,
    /// Per Crew chat (slot key): the model last picked by hand, the target of
    /// the K6 double-press Auto toggle. Model ids only, nothing sensitive.
    #[serde(default)]
    pub last_models: BTreeMap<String, String>,
    /// Bring the Crew window to the front when K1-K3 selects a chat.
    #[serde(default = "default_true")]
    pub focus_on_select: bool,
    /// Voice input: `true` = every slot uses keyboard mode (text on the
    /// keyboard's input strip, K6 sends it through the Crew API). `false`
    /// (default) = automatic: a slot with an Alt+N number (`chats[i]` in
    /// 1..=9) uses PC mode (the text is typed into the Crew input box).
    /// Replaces the old `voice_inject`; an old file's value is ignored.
    #[serde(default)]
    pub voice_force_keyboard: bool,
    /// List Crew Mode / Bot worker sessions in the keyboard's Bind session menu.
    #[serde(default)]
    pub show_workers: bool,
}

fn default_true() -> bool {
    true
}

impl Default for BindStore {
    fn default() -> Self {
        Self {
            chats: default_chats(),
            aliases: default_aliases(),
            sessions: default_sessions(),
            names: default_sessions(),
            pinned_session: String::new(),
            last_models: BTreeMap::new(),
            focus_on_select: true,
            voice_force_keyboard: false,
            show_workers: false,
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
            // Valid range is 0..=9: 0 = "not set" (no Alt+N jump), 1..=9 = jump
            // to that chat. Anything else falls back to the slot default.
            if *n > 9 {
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
            *session = row_identity(&normalize_session_id(session).unwrap_or_default());
        }
        self.names.resize(SLOT_COUNT, String::new());
        self.names.truncate(SLOT_COUNT);
        for (name, session) in self.names.iter_mut().zip(&self.sessions) {
            *name = if session.is_empty() { String::new() } else { normalize_name(name) };
        }
        self.pinned_session = row_identity(&normalize_session_id(&self.pinned_session).unwrap_or_default());
        // Keys move to the row-identity form too (older files: bare slot keys).
        self.last_models = std::mem::take(&mut self.last_models)
            .into_iter()
            .filter(|(slot, model)| {
                normalize_session_id(slot).is_ok_and(|s| !s.is_empty() && s == *slot) && valid_model_id(model)
            })
            .map(|(slot, model)| (row_identity(&slot), model))
            .collect();
        while self.last_models.len() > LAST_MODELS_MAX {
            let first = self.last_models.keys().next().cloned();
            if let Some(k) = first {
                self.last_models.remove(&k);
            }
        }
    }

    /// Remember `model` as the last hand-picked model of chat `slot` (a slot
    /// key of this machine, or a row identity).
    pub fn set_last_model(&mut self, slot: &str, model: &str) -> Result<(), String> {
        let slot = normalize_session_id(slot)?;
        if slot.is_empty() || !valid_model_id(model) {
            return Err("invalid chat or model id".to_string());
        }
        self.last_models.insert(row_identity(&slot), model.to_string());
        self.normalize();
        Ok(())
    }

    /// `last_models` keyed the way the menus key chats ([`status_id`]).
    pub fn last_models_by_status_id(&self) -> Vec<(String, String)> {
        self.last_models.iter().map(|(row, model)| (status_id(row), model.clone())).collect()
    }

    /// The bound sessions in [`status_id`] form (empty = not bound).
    pub fn status_ids(&self) -> Vec<String> {
        self.sessions.iter().map(|row| status_id(row)).collect()
    }

    /// Slot key of the session bound to slot index `i` when it is on this
    /// machine; `None` when unbound or bound to a remote crew's session.
    pub fn local_key(&self, i: usize) -> Option<String> {
        match split_row(self.sessions.get(i)?) {
            Some((LOCAL_INSTANCE, key)) => Some(key.to_string()),
            _ => None,
        }
    }

    /// Per slot A/B/C: whether voice input uses PC mode (see
    /// `voice_force_keyboard`). `control` = the keyboard may control the PC;
    /// without it every slot is keyboard mode.
    pub fn voice_pc(&self, control: bool) -> [bool; SLOT_COUNT] {
        let mut pc = [false; SLOT_COUNT];
        if control && !self.voice_force_keyboard {
            for (i, flag) in pc.iter_mut().enumerate() {
                *flag = matches!(self.chats.get(i), Some(1..=9));
            }
        }
        pc
    }

    /// The pinned Agent Status session in [`status_id`] form.
    pub fn pinned_status_id(&self) -> String {
        status_id(&self.pinned_session)
    }

    /// Read the chat number for slot `n` (1-based). Returns `None` for an
    /// out-of-range slot.
    pub fn get(&self, n: u8) -> Option<u8> {
        idx(n).and_then(|i| self.chats.get(i).copied())
    }

    /// Set slot `n` (1-based) to chat `number`, clamped to `0..=9`. `0` means
    /// "not set" (pressing the key does not jump the Kiro Crew view). Values
    /// 1..=9 jump to that Alt+N on click. Out-of-range slot is a no-op; an
    /// out-of-range number is clamped into `0..=9`.
    pub fn set(&mut self, n: u8, number: u8) {
        if let Some(i) = idx(n) {
            if let Some(slot) = self.chats.get_mut(i) {
                *slot = number.min(9);
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
        self.set_session_named(n, session_id, "")
    }

    /// Bind slot `n` to `session_id` (bare local key or row identity) and
    /// remember its display name.
    pub fn set_session_named(&mut self, n: u8, session_id: &str, name: &str) -> Result<(), String> {
        let i = idx(n).ok_or_else(|| format!("binding slot must be 1..={SLOT_COUNT}"))?;
        self.sessions[i] = row_identity(&normalize_session_id(session_id)?);
        self.names[i] = if self.sessions[i].is_empty() { String::new() } else { normalize_name(name) };
        Ok(())
    }

    /// Return every 1-based Chat A/B/C slot associated with `session_id`.
    /// Empty and unknown ids intentionally produce no board destination.
    pub fn slots_for_session(&self, session_id: &str) -> Vec<u8> {
        let session_id = row_identity(session_id);
        if session_id.is_empty() {
            return Vec::new();
        }
        self.sessions
            .iter()
            .enumerate()
            .filter_map(|(index, bound)| {
                (*bound == session_id).then_some((index + 1) as u8)
            })
            .collect()
    }

    /// Remove only the UI session association for a slot.
    pub fn clear_session(&mut self, n: u8) -> Result<(), String> {
        self.set_session(n, "")
    }

    /// Persist the Agent Status session watched by the keyboard screen. Empty
    /// input restores the default behavior of following every session.
    pub fn set_pinned_session(&mut self, session_id: &str) -> Result<(), String> {
        self.pinned_session = row_identity(&normalize_session_id(session_id)?);
        Ok(())
    }

    /// Reset slot `n` (1-based) to its default chat number (`0` = not set, no
    /// Alt+N jump). Out-of-range is a no-op. Alias and session are preserved.
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

    /// Persist the store to `~/.ak-companion/binds.json`, creating the
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

/// A session display name: control characters become spaces, at most 64 characters.
fn normalize_name(name: &str) -> String {
    let flat: String = name.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
    flat.trim().chars().take(64).collect()
}

/// A Crew model id: 1..=128 printable ASCII characters, no spaces.
fn valid_model_id(model: &str) -> bool {
    !model.is_empty() && model.len() <= 128 && model.bytes().all(|b| b.is_ascii_graphic())
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

/// Path of the bind file: `~/.ak-companion/binds.json` (see `app_dirs`).
/// The same dot-directory holds the hook handoff and the diagnostic logs, so
/// the companion keeps all its state under one place in the user's home on
/// every platform.
pub fn binds_path() -> Option<PathBuf> {
    Some(crate::app_dirs::companion_file("binds.json"))
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── default / get / set / clear ──────────────────────────

    #[test]
    fn new_store_has_default_chat_numbers_and_empty_aliases() {
        let s = BindStore::new();
        // Default is "not set" (0) for all slots: no Alt+N jump until opted in.
        assert_eq!(s.chats, vec![0, 0, 0]);
        assert_eq!(s.aliases, vec!["", "", ""]);
        assert_eq!(s.sessions, vec!["", "", ""]);
        assert_eq!(s.pinned_session, "");
        assert_eq!(s.get(1), Some(0));
        assert_eq!(s.get(2), Some(0));
        assert_eq!(s.get(3), Some(0));
    }

    #[test]
    fn set_get_clear_round_trip() {
        let mut s = BindStore::new();
        s.set(2, 7);
        assert_eq!(s.get(2), Some(7));
        // Slots A and C keep their defaults (unset = 0).
        assert_eq!(s.get(1), Some(0));
        assert_eq!(s.get(3), Some(0));

        // Clear resets slot B's chat number to unset (0) but keeps its alias.
        s.set_alias(2, " Build ").unwrap();
        s.clear(2);
        assert_eq!(s.get(2), Some(0));
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
        s.set_session_named(3, "chat-2", "Plan").unwrap();
        assert_eq!(s.sessions[1], "local:chat-2", "hook ids are this machine's slots");
        assert_eq!(s.names[2], "Plan");
        // Hook id, bare API key and row identity all name the same session.
        assert_eq!(s.slots_for_session(" dashboard:chat-2 "), vec![2, 3]);
        assert_eq!(s.slots_for_session("chat-2"), vec![2, 3]);
        assert_eq!(s.slots_for_session("local:chat-2"), vec![2, 3]);
        assert!(s.slots_for_session("").is_empty());
        assert!(s.slots_for_session("unknown").is_empty());
        s.clear_session(2).unwrap();
        assert_eq!(s.sessions[1], "");
        assert_eq!(s.slots_for_session("dashboard:chat-2"), vec![3]);
        assert!(s.set_session(4, "other").is_err());
        assert!(s.set_session(1, "bad\nsession").is_err());
    }

    #[test]
    fn set_clamps_number_into_0_9() {
        let mut s = BindStore::new();
        s.set(1, 0);
        assert_eq!(s.get(1), Some(0)); // 0 is valid = not set
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
        assert_eq!(s.chats, vec![0, 0, 0]);
    }

    // ── (de)serialize ───────────────────────────────────────

    #[test]
    fn store_json_round_trips() {
        let mut s = BindStore::new();
        s.set(1, 4);
        s.set(3, 8);
        s.set_alias(1, "Plan").unwrap();
        s.set_session_named(1, "chat-4", "Plan it").unwrap();
        s.set_session(2, "remote1:chat-9").unwrap();
        s.set_pinned_session("chat-4").unwrap();
        s.set_last_model("chat-4", "claude-opus").unwrap();
        let json = serde_json::to_string(&s).unwrap();
        assert_eq!(
            json,
            r#"{"chats":[4,0,8],"aliases":["Plan","",""],"sessions":["local:chat-4","remote1:chat-9",""],"names":["Plan it","",""],"pinned_session":"local:chat-4","last_models":{"local:chat-4":"claude-opus"},"focus_on_select":true,"voice_force_keyboard":false,"show_workers":false}"#
        );
        let back: BindStore = serde_json::from_str(&json).unwrap();
        assert_eq!(s, back);
    }

    #[test]
    fn load_normalizes_vectors_and_migrates_old_files() {
        // Short vector is padded with the default (unset = 0).
        let mut old: BindStore = serde_json::from_str(r#"{"chats":[5]}"#).unwrap();
        old.normalize();
        assert_eq!(old.chats, vec![5, 0, 0]);
        assert_eq!(old.aliases, vec!["", "", ""]);
        assert_eq!(old.sessions, vec!["", "", ""]);
        assert_eq!(old.pinned_session, "");

        let mut long: BindStore =
            serde_json::from_str(r#"{"chats":[9,8,7,6],"aliases":["One","Two","Three","Four"]}"#)
                .unwrap();
        long.normalize();
        assert_eq!(long.chats, vec![9, 8, 7]);
        assert_eq!(long.aliases, vec!["One", "Two", "Three"]);
        assert_eq!(long.sessions, vec!["", "", ""]);

        // 0 is now valid (= not set); only >9 falls back to the default (0).
        let mut bad: BindStore =
            serde_json::from_str(r#"{"chats":[0,10,4],"aliases":["12345678","中文","Ok"]}"#)
                .unwrap();
        bad.normalize();
        assert_eq!(bad.chats, vec![0, 0, 4]);
        assert_eq!(bad.aliases, vec!["", "", "Ok"]);
        assert_eq!(bad.sessions, vec!["", "", ""]);
    }

    #[test]
    fn old_files_migrate_to_row_identities() {
        // v0.3 files: bare slot keys, Ctrl+N numbers in `instances` (dropped).
        let mut old: BindStore = serde_json::from_str(
            r#"{"instances":[1,2,3],"sessions":["chat-1-17","","dashboard:chat-2-18"],"pinned_session":"chat-1-17","last_models":{"chat-1-17":"m1"}}"#,
        )
        .unwrap();
        old.normalize();
        assert_eq!(old.sessions, vec!["local:chat-1-17", "", "local:chat-2-18"]);
        assert_eq!(old.names, vec!["", "", ""]);
        assert_eq!(old.pinned_session, "local:chat-1-17");
        assert_eq!(old.last_models["local:chat-1-17"], "m1");
        assert!(!serde_json::to_string(&old).unwrap().contains("instances"));
        // The rest of the Companion keeps keying local chats by slot key.
        assert_eq!(old.status_ids(), vec!["chat-1-17", "", "chat-2-18"]);
        assert_eq!(old.pinned_status_id(), "chat-1-17");
        assert_eq!(old.last_models_by_status_id(), vec![("chat-1-17".to_string(), "m1".to_string())]);
        assert_eq!(old.local_key(0).as_deref(), Some("chat-1-17"));
        assert_eq!(old.local_key(1), None);
        // A remote crew's session keeps its row identity everywhere.
        old.set_session(3, "remote1:chat-9").unwrap();
        assert_eq!(old.local_key(2), None);
        assert_eq!(old.status_ids()[2], "remote1:chat-9");
        assert_eq!(old.slots_for_session("remote1:chat-9"), vec![3]);
        assert!(old.slots_for_session("chat-9").is_empty(), "a local key never matches a remote binding");
        assert_eq!(split_row("remote1:chat-9"), Some(("remote1", "chat-9")));
        assert_eq!(split_row(":chat-9"), None);
        assert_eq!(row_identity(""), "");
    }

    #[test]
    fn last_models_are_validated_and_capped() {
        let mut s = BindStore::new();
        assert!(s.set_last_model("", "m").is_err());
        assert!(s.set_last_model("chat", "has space").is_err());
        for i in 0..(LAST_MODELS_MAX + 5) {
            s.set_last_model(&format!("chat-{i:03}"), "claude-opus").unwrap();
        }
        assert_eq!(s.last_models.len(), LAST_MODELS_MAX);
        let mut bad: BindStore =
            serde_json::from_str(r#"{"last_models":{"ok":"m1","bad\nkey":"m2","x":""}}"#).unwrap();
        bad.normalize();
        assert_eq!(bad.last_models.len(), 1);
        assert_eq!(bad.last_models["local:ok"], "m1");
    }

    #[test]
    fn empty_object_deserializes_to_defaults() {
        let mut s: BindStore = serde_json::from_str("{}").unwrap();
        s.normalize();
        assert_eq!(s.chats, vec![0, 0, 0]);
        assert_eq!(s.aliases, vec!["", "", ""]);
        assert_eq!(s.sessions, vec!["", "", ""]);
        assert_eq!(s.pinned_session, "");
        assert!(s.focus_on_select);
        assert!(!s.voice_force_keyboard);
        assert!(s.last_models.is_empty());
    }

    #[test]
    fn voice_mode_follows_the_alt_n_number_unless_forced() {
        let mut s = BindStore::new();
        s.chats = vec![0, 2, 9];
        assert_eq!(s.voice_pc(true), [false, true, true]);
        assert_eq!(s.voice_pc(false), [false, false, false], "no PC control: keyboard mode");
        s.voice_force_keyboard = true;
        assert_eq!(s.voice_pc(true), [false, false, false]);
    }

    #[test]
    fn the_old_voice_inject_field_is_ignored() {
        let mut s: BindStore = serde_json::from_str(r#"{"chats":[1,0,0],"voice_inject":true}"#).unwrap();
        s.normalize();
        assert!(!s.voice_force_keyboard, "an old inject user is back on automatic");
        assert!(!serde_json::to_string(&s).unwrap().contains("voice_inject"));
    }
}
