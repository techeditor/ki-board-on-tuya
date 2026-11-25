//! FR-03: keeping the keyboard's 5 scene slots in step with the Companion's
//! scene store, and switching scenes (design §6.3). Pure: every method
//! returns the frames to write; `lib.rs` writes them and emits events.
//!
//! Slots: 0 = the current scene when it is not a favourite, 1..=4 = the
//! favourites in order. The keyboard answers every scene_put / scene_switch /
//! scene_del / scene_list with one `scene_state`, so the manager counts the
//! answers it still waits for and only plans again once all are in. That
//! keeps a half-applied sync from planning the same puts twice.

use std::path::PathBuf;

use serde::Serialize;
use serde_json::Value;

use crate::protocol::{
    self, DeviceSettingsPayload, DeviceSettingsUpdate, SceneDataPayload, SceneSlotInfo, SceneStatePayload,
    SCENE_SLOTS,
};
use crate::scene::{self, Scene};
use crate::scene_store::{SceneStore, MAX_FAVORITES};

/// A busy keyboard (not in standby) is asked again this often, this many times.
pub const BUSY_RETRY_MS: u64 = 1500;
pub const BUSY_RETRIES: u32 = 20;

/// One change to the keyboard's slots.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SlotOp {
    Put { slot: u8, scene: Box<Scene> },
    Del { slot: u8 },
    Switch { slot: u8 },
}

impl SlotOp {
    pub fn frame(&self) -> Value {
        match self {
            SlotOp::Put { slot, scene } => scene::scene_put_payload(*slot, scene),
            SlotOp::Del { slot } => protocol::scene_slot_payload("scene_del", *slot),
            SlotOp::Switch { slot } => protocol::scene_slot_payload("scene_switch", *slot),
        }
    }
}

fn slot_is(info: Option<&SceneSlotInfo>, s: &Scene) -> bool {
    info.is_some_and(|i| i.id == s.id && i.rev == s.rev)
}

/// Ops that make the keyboard hold the favourites in slots 1..=4 and the
/// current scene in slot 0 (when it is not a favourite). Never writes a
/// different scene over the active slot (that would change what "current"
/// means without applying it), never deletes the active slot, and keeps
/// slots whose id this Companion does not know (made on another computer).
/// A newer revision of the active scene is applied again unless the user
/// changed settings on the keyboard since (`dirty`).
pub fn plan_sync(store: &SceneStore, board: &SceneStatePayload) -> Vec<SlotOp> {
    let known = |id: &str| store.get(id).is_some();
    let mut ops = Vec::new();
    let mut want: Vec<Option<Scene>> = vec![None; SCENE_SLOTS];
    for (i, id) in store.favorites.iter().take(MAX_FAVORITES).enumerate() {
        want[i + 1] = store.get(id);
    }
    if let Some(cur) = store.current.as_deref() {
        if !store.favorites.iter().any(|f| f == cur) {
            want[0] = store.get(cur);
        }
    }
    for (slot, wanted) in want.iter().enumerate() {
        let info = board.slots.get(slot);
        let active = board.active == slot as i32;
        let held = info.map(|i| i.id.as_str()).unwrap_or("");
        match wanted {
            Some(s) if !slot_is(info, s) => {
                if active && held != s.id {
                    continue;
                }
                ops.push(SlotOp::Put { slot: slot as u8, scene: Box::new(s.clone()) });
                if active && !board.dirty {
                    ops.push(SlotOp::Switch { slot: slot as u8 });
                }
            }
            None if slot > 0 && !held.is_empty() && !active && known(held) => {
                ops.push(SlotOp::Del { slot: slot as u8 });
            }
            _ => {}
        }
    }
    ops
}

/// Ops that switch the keyboard to scene `s`: an up-to-date copy is used
/// where it already is; a favourite goes into its own slot; anything else
/// into slot 0. The last op is always the Switch.
pub fn plan_switch(store: &SceneStore, board: &SceneStatePayload, s: &Scene) -> Vec<SlotOp> {
    if let Some(slot) = (0..SCENE_SLOTS).find(|&i| slot_is(board.slots.get(i), s)) {
        return vec![SlotOp::Switch { slot: slot as u8 }];
    }
    let fav = store.favorites.iter().position(|f| *f == s.id).map(|i| i + 1);
    let slot = (match fav {
        // The favourite's slot is active with another scene: use slot 0.
        Some(i) if board.active != i as i32 || board.slots.get(i).is_some_and(|x| x.id == s.id) => i,
        _ => 0,
    }) as u8;
    vec![SlotOp::Put { slot, scene: Box::new(s.clone()) }, SlotOp::Switch { slot }]
}

/// Frames for firmware without `scenes`: the keymap and the scene's voice
/// mode and LED power through the existing frames. Actions an old firmware
/// does not know (scene_menu, key_*) keep its current binding there.
pub fn legacy_frames(s: &Scene) -> Vec<Value> {
    let keys = protocol::keymap_payload(&legacy_bindings(s));
    let settings = protocol::device_settings_update_payload(&DeviceSettingsUpdate {
        voice_mode: Some(s.voice_mode.clone()),
        led_power: Some(s.lights.power.clone()),
        led_auto_off_s: Some(s.lights.auto_off_s),
        ..Default::default()
    });
    vec![keys, settings]
}

fn legacy_bindings(s: &Scene) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let km = scene::keys_from_km(&scene::km_hex(s)).unwrap_or_default();
    for (id, k) in [
        ("k1", &km.k1),
        ("k2", &km.k2),
        ("k3", &km.k3),
        ("k4", &km.k4),
        ("k5", &km.k5),
        ("k6", &km.k6),
        ("enc", &km.enc),
    ] {
        if k.kind.as_deref() == Some("voice") {
            out.push((format!("{id}.kind"), "voice".to_string()));
            continue;
        }
        out.push((format!("{id}.kind"), "normal".to_string()));
        for (g, v) in [("single", &k.single), ("double", &k.double), ("long", &k.long)] {
            if id == "enc" && g == "long" {
                continue; // reserved for the keyboard settings page
            }
            out.push((format!("{id}.{g}"), v.clone().unwrap_or_else(|| "none".into())));
        }
    }
    out
}

/// A user switch waiting for the keyboard's answer.
#[derive(Clone, Debug, PartialEq, Eq)]
struct PendingSwitch {
    id: String,
    slot: u8,
    attempts: u32,
}

/// What a frame from the keyboard means for the UI and the link.
#[derive(Debug, Default, PartialEq)]
pub struct Reaction {
    /// Frames to write now, in order.
    pub frames: Vec<Value>,
    /// Call [`SceneManager::retry`] after this many ms (the keyboard was busy).
    pub retry_after_ms: Option<u64>,
    /// A short message for the user (switch failed, sync failed, imported).
    pub message: Option<String>,
    /// The view changed: emit it.
    pub changed: bool,
}

/// What the UI shows (`scenes-changed` event and `scenes_list`).
#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct SceneView {
    pub scenes: Vec<Scene>,
    pub presets: Vec<String>,
    pub favorites: Vec<String>,
    pub current: Option<String>,
    /// A keyboard with `scenes` is connected and has reported its slots.
    pub board_synced: bool,
    /// The keyboard has old firmware: switching still sends the keymap and
    /// settings, but home layout and idle colours do not apply.
    pub board_legacy: bool,
    /// Settings changed on the keyboard since the current scene was applied.
    pub dirty: bool,
    /// A switch is waiting for the keyboard (e.g. it is busy).
    pub switching: Option<String>,
    /// Keyboard slots holding a scene this Companion does not have.
    pub board_only: Vec<BoardOnly>,
    /// The store file could not be read; changes are not saved.
    pub store_error: Option<String>,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct BoardOnly {
    pub slot: u8,
    pub name: String,
}

#[derive(Debug)]
pub struct SceneManager {
    pub store: SceneStore,
    path: Option<PathBuf>,
    store_error: Option<String>,
    /// `Some(true)` = the connected keyboard advertised `scenes`.
    scenes_cap: Option<bool>,
    board: Option<SceneStatePayload>,
    /// scene_state answers still expected for frames we sent.
    awaiting: usize,
    pending: Option<PendingSwitch>,
    /// Slot asked with scene_get for "import from keyboard".
    importing: Option<u8>,
    /// A sync write failed; do not retry until the store or the link changes.
    sync_failed: bool,
    live: Option<DeviceSettingsPayload>,
}

impl SceneManager {
    /// Load `~/.ak-companion/scenes.json`. A damaged file is kept untouched:
    /// the manager runs on an empty store and refuses to save.
    pub fn load() -> Self {
        match SceneStore::load() {
            Ok(store) => Self::new(store, Some(crate::app_dirs::companion_file(crate::scene_store::STORE_FILE))),
            Err(e) => {
                let mut m = Self::new(SceneStore::default(), None);
                m.store_error = Some(e);
                m
            }
        }
    }

    /// `path` None = never write (tests, damaged file).
    pub fn new(store: SceneStore, path: Option<PathBuf>) -> Self {
        SceneManager {
            store,
            path,
            store_error: None,
            scenes_cap: None,
            board: None,
            awaiting: 0,
            pending: None,
            importing: None,
            sync_failed: false,
            live: None,
        }
    }

    fn save(&self) -> Result<(), String> {
        if let Some(e) = &self.store_error {
            return Err(format!("scenes are not saved: {e}"));
        }
        match &self.path {
            Some(p) => self.store.save_to(p),
            None => Ok(()),
        }
    }

    pub fn view(&self) -> SceneView {
        let board_only = self
            .board
            .iter()
            .flat_map(|b| b.slots.iter().enumerate())
            .filter(|(_, i)| !i.id.is_empty() && self.store.get(&i.id).is_none())
            .map(|(slot, i)| BoardOnly { slot: slot as u8, name: i.name.clone() })
            .collect();
        SceneView {
            scenes: self.store.all(),
            presets: scene::presets().into_iter().map(|p| p.id).collect(),
            favorites: self.store.favorites.clone(),
            current: self.store.current.clone(),
            board_synced: self.scenes_cap == Some(true) && self.board.is_some(),
            board_legacy: self.scenes_cap == Some(false),
            dirty: self.board.as_ref().is_some_and(|b| b.dirty),
            switching: self.pending.as_ref().map(|p| p.id.clone()),
            board_only,
            store_error: self.store_error.clone(),
        }
    }

    /// A keyboard answered hello. Its `scene_state` follows by itself.
    pub fn on_hello(&mut self, capabilities: &[String]) {
        self.scenes_cap = Some(capabilities.iter().any(|c| c == "scenes"));
        self.board = None;
        self.awaiting = 0;
        self.pending = None;
        self.importing = None;
        self.sync_failed = false;
    }

    pub fn on_disconnect(&mut self) {
        self.scenes_cap = None;
        self.board = None;
        self.awaiting = 0;
        self.pending = None;
        self.importing = None;
    }

    fn send(&mut self, ops: Vec<SlotOp>) -> Vec<Value> {
        self.awaiting += ops.len();
        ops.iter().map(SlotOp::frame).collect()
    }

    /// Sync frames when the keyboard is idle from our side.
    fn sync(&mut self) -> Vec<Value> {
        if self.awaiting > 0 || self.pending.is_some() || self.sync_failed || self.scenes_cap != Some(true) {
            return Vec::new();
        }
        let Some(board) = &self.board else { return Vec::new() };
        let ops = plan_sync(&self.store, board);
        self.send(ops)
    }

    pub fn on_state(&mut self, st: &SceneStatePayload) -> Reaction {
        let mut r = Reaction { changed: true, ..Default::default() };
        if self.scenes_cap.is_none() {
            self.scenes_cap = Some(true); // a scene_state proves the capability
        }
        self.awaiting = self.awaiting.saturating_sub(1);
        self.board = Some(st.clone());

        // The keyboard's active slot names the current scene (it may have
        // been switched on the keyboard itself).
        let active_id = usize::try_from(st.active).ok().and_then(|i| st.slots.get(i)).map(|i| i.id.clone());
        if let Some(id) = active_id.filter(|id| !id.is_empty()) {
            let cur = self.store.get(&id).map(|s| s.id);
            if self.store.current != cur && self.pending.is_none() {
                self.store.current = cur;
                let _ = self.save();
            }
        }

        let req_switch = st.req.as_deref() == Some("scene_switch");
        if let Some(p) = self.pending.clone() {
            if st.error.is_some() && !req_switch {
                self.pending = None;
                r.message = Some(format!("switch failed: keyboard said {}", st.error.as_deref().unwrap_or("")));
            } else if self.awaiting == 0 {
                match st.error.as_deref() {
                    Some("busy") if p.attempts < BUSY_RETRIES => r.retry_after_ms = Some(BUSY_RETRY_MS),
                    Some("busy") => {
                        self.pending = None;
                        r.message = Some("the keyboard stayed busy; switch cancelled".into());
                    }
                    Some(e) => {
                        self.pending = None;
                        r.message = Some(format!("switch failed: keyboard said {e}"));
                    }
                    None => {
                        self.pending = None;
                        let ok = st.active == p.slot as i32 && st.slots.get(p.slot as usize).is_some_and(|i| i.id == p.id);
                        if ok {
                            self.store.current = Some(p.id);
                            let _ = self.save();
                        } else {
                            r.message = Some("switch failed: the keyboard did not apply the scene".into());
                        }
                    }
                }
            }
        } else if let Some(e) = st.error.as_deref() {
            if e == "busy" && req_switch {
                // A sync re-apply hit a busy keyboard; the next sync tries again.
            } else if matches!(e, "invalid" | "storage") {
                self.sync_failed = true;
                r.message = Some(format!("could not copy scenes to the keyboard ({e})"));
            } else if e == "empty" && self.importing.take().is_some() {
                r.message = Some("that keyboard slot is empty".into());
            }
        }
        if self.pending.is_none() {
            r.frames = self.sync();
        }
        r
    }

    /// The keyboard sent one slot in full (import from keyboard).
    pub fn on_data(&mut self, d: &SceneDataPayload) -> Reaction {
        let mut r = Reaction { changed: true, ..Default::default() };
        if self.importing != Some(d.slot) {
            r.changed = false;
            return r;
        }
        self.importing = None;
        let built = scene::keys_from_km(&d.km).zip(scene::idle_from_hex(&d.idle)).map(|(keys, idle)| Scene {
            id: String::new(),
            name: d.name.clone(),
            rev: 0,
            keys,
            knob: "arrows".into(), // the keyboard does not store the Companion-side knob mode
            voice_mode: d.voice.clone(),
            home: d.home.clone(),
            lights: scene::Lights { power: d.led.clone(), auto_off_s: d.led_off_s, idle },
        });
        r.message = Some(match built.ok_or_else(|| "the keyboard copy is damaged".to_string()).and_then(|s| {
            let id = self.store.add(s)?;
            self.save()?;
            Ok(id)
        }) {
            Ok(_) => "imported the scene from the keyboard".into(),
            Err(e) => format!("import from keyboard failed: {e}"),
        });
        r
    }

    /// Settings reported by the keyboard: kept for "save to scene"; a local
    /// change may have made the current scene dirty, so ask again.
    pub fn on_device_settings(&mut self, p: &DeviceSettingsPayload) -> Vec<Value> {
        self.live = Some(p.clone());
        if self.scenes_cap == Some(true) && self.board.is_some() && self.awaiting == 0 && self.pending.is_none() {
            self.awaiting += 1;
            return vec![protocol::scene_list_payload()];
        }
        Vec::new()
    }

    /// Refuse edits up front when the store cannot be saved, so memory and
    /// disk never disagree.
    fn writable(&self) -> Result<(), String> {
        match &self.store_error {
            Some(e) => Err(format!("scenes are not saved: {e}")),
            None => Ok(()),
        }
    }

    fn after_edit(&mut self) -> Result<Vec<Value>, String> {
        self.save()?;
        self.sync_failed = false;
        Ok(self.sync())
    }

    pub fn create(&mut self, s: Scene) -> Result<(String, Vec<Value>), String> {
        self.writable()?;
        let id = self.store.add(s)?;
        Ok((id, self.after_edit()?))
    }

    pub fn duplicate(&mut self, id: &str) -> Result<(String, Vec<Value>), String> {
        self.writable()?;
        let id = self.store.duplicate(id)?;
        Ok((id, self.after_edit()?))
    }

    pub fn update(&mut self, id: &str, s: Scene) -> Result<Vec<Value>, String> {
        self.writable()?;
        self.store.update(id, s)?;
        self.after_edit()
    }

    pub fn rename(&mut self, id: &str, name: &str) -> Result<Vec<Value>, String> {
        self.writable()?;
        self.store.rename(id, name)?;
        self.after_edit()
    }

    pub fn delete(&mut self, id: &str) -> Result<Vec<Value>, String> {
        self.writable()?;
        self.store.delete(id)?;
        self.after_edit()
    }

    pub fn set_favorites(&mut self, ids: &[String]) -> Result<Vec<Value>, String> {
        self.writable()?;
        self.store.set_favorites(ids)?;
        self.after_edit()
    }

    pub fn import(&mut self, bytes: &[u8]) -> Result<Vec<String>, String> {
        self.writable()?;
        let ids = self.store.import(bytes)?;
        self.save()?;
        Ok(ids)
    }

    pub fn export(&self, ids: &[String]) -> Result<String, String> {
        self.store.export(ids)
    }

    /// Ask the keyboard for one slot; `on_data` adds it to the store.
    pub fn import_from_board(&mut self, slot: u8) -> Result<Vec<Value>, String> {
        if self.scenes_cap != Some(true) {
            return Err("the keyboard firmware does not support scenes".into());
        }
        if slot as usize >= SCENE_SLOTS {
            return Err("slot must be 0..=4".into());
        }
        self.importing = Some(slot);
        Ok(vec![protocol::scene_slot_payload("scene_get", slot)])
    }

    /// Switch to scene `id`. Old firmware gets the keymap + settings frames
    /// and the switch counts as done at once.
    pub fn switch(&mut self, id: &str) -> Result<Vec<Value>, String> {
        let s = self.store.get(id).ok_or("no such scene")?;
        match (self.scenes_cap, &self.board) {
            (Some(true), Some(board)) => {
                if self.pending.is_some() {
                    return Err("another switch is still waiting for the keyboard".into());
                }
                let ops = plan_switch(&self.store, board, &s);
                let slot = match ops.last() {
                    Some(SlotOp::Switch { slot }) => *slot,
                    _ => return Err("nothing to switch".into()),
                };
                self.pending = Some(PendingSwitch { id: s.id.clone(), slot, attempts: 0 });
                Ok(self.send(ops))
            }
            (Some(true), None) => Err("the keyboard has not reported its scenes yet".into()),
            (Some(false), _) => {
                self.store.current = Some(s.id.clone());
                self.save()?;
                Ok(legacy_frames(&s))
            }
            (None, _) => Err("no keyboard connected".into()),
        }
    }

    /// Ask a busy keyboard again (after `retry_after_ms`).
    pub fn retry(&mut self) -> Vec<Value> {
        let Some(p) = self.pending.as_mut() else { return Vec::new() };
        p.attempts += 1;
        let slot = p.slot;
        self.send(vec![SlotOp::Switch { slot }])
    }

    /// Give up a waiting switch (user cancelled).
    pub fn cancel_switch(&mut self) {
        self.pending = None;
    }

    /// "Restore scene": apply the current scene again, dropping changes made
    /// on the keyboard.
    pub fn restore(&mut self) -> Result<Vec<Value>, String> {
        let id = self.store.current.clone().ok_or("no current scene")?;
        self.switch(&id)
    }

    /// "Save to scene": take the voice mode and LED power the keyboard now
    /// uses into the current scene. Presets are read-only (copy first).
    pub fn save_live(&mut self) -> Result<Vec<Value>, String> {
        let id = self.store.current.clone().ok_or("no current scene")?;
        let live = self.live.clone().ok_or("the keyboard has not reported its settings")?;
        let mut s = self.store.get(&id).ok_or("no such scene")?;
        if let Some(v) = live.voice_mode {
            s.voice_mode = v;
        }
        if let Some(v) = live.led_power {
            s.lights.power = v;
        }
        if let Some(v) = live.led_auto_off_s {
            s.lights.auto_off_s = v;
        }
        self.update(&id, s)
    }

    /// The knob mode of the current scene (Companion-side, see ControlCtx).
    pub fn knob(&self) -> String {
        self.store.current.as_deref().and_then(|id| self.store.get(id)).map_or("arrows".into(), |s| s.knob)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn slots(list: &[(&str, u32)]) -> Vec<SceneSlotInfo> {
        let mut v: Vec<SceneSlotInfo> = list
            .iter()
            .map(|(id, rev)| SceneSlotInfo { id: (*id).into(), rev: *rev, name: (*id).into() })
            .collect();
        v.resize(SCENE_SLOTS, SceneSlotInfo::default());
        v
    }

    fn state(active: i32, dirty: bool, list: &[(&str, u32)]) -> SceneStatePayload {
        SceneStatePayload { active, dirty, slots: slots(list), error: None, req: None }
    }

    fn err(st: &SceneStatePayload, e: &str, req: &str) -> SceneStatePayload {
        SceneStatePayload { error: Some(e.into()), req: Some(req.into()), ..st.clone() }
    }

    /// Store with presets + two user scenes; returns (store, a, b).
    fn store() -> (SceneStore, String, String) {
        let mut st = SceneStore::default();
        let a = st.add(scene::presets().remove(0)).unwrap();
        let b = st.add(scene::presets().remove(0)).unwrap();
        (st, a, b)
    }

    fn kinds(frames: &[Value]) -> Vec<String> {
        frames
            .iter()
            .map(|f| {
                let slot = f["payload"]["slot"].as_u64().map(|s| s.to_string()).unwrap_or_default();
                format!("{}{}", f["type"].as_str().unwrap().trim_start_matches("scene_"), slot)
            })
            .collect()
    }

    #[test]
    fn sync_puts_favourites_and_deletes_stale_known_slots() {
        let (mut st, a, b) = store();
        st.set_favorites(&[a.clone(), "preset01".into()]).unwrap();
        // slot1 empty, slot2 holds an old preset copy, slot3 holds b (not a favourite), slot4 unknown.
        let board = state(-1, false, &[("", 0), ("", 0), ("preset01", 0), (&b, 0), ("zzzzzzzz", 1)]);
        let ops = plan_sync(&st, &board);
        let f: Vec<Value> = ops.iter().map(SlotOp::frame).collect();
        assert_eq!(kinds(&f), vec!["put1", "del3"], "preset copy up to date, unknown slot kept");
    }

    #[test]
    fn sync_protects_the_active_slot() {
        let (mut st, a, b) = store();
        st.set_favorites(&[a.clone()]).unwrap();
        // slot 1 is active with b: do not overwrite it.
        assert!(plan_sync(&st, &state(1, false, &[("", 0), (&b, 0)])).is_empty());
        // Active a at an old rev: put and apply again, unless dirty.
        st.rename(&a, "New").unwrap();
        let f: Vec<Value> = plan_sync(&st, &state(1, false, &[("", 0), (&a, 0)])).iter().map(SlotOp::frame).collect();
        assert_eq!(kinds(&f), vec!["put1", "switch1"]);
        let f: Vec<Value> = plan_sync(&st, &state(1, true, &[("", 0), (&a, 0)])).iter().map(SlotOp::frame).collect();
        assert_eq!(kinds(&f), vec!["put1"]);
        // Current (not a favourite) goes to slot 0.
        st.current = Some(b.clone());
        let f: Vec<Value> = plan_sync(&st, &state(-1, false, &[("", 0), (&a, 1)])).iter().map(SlotOp::frame).collect();
        assert_eq!(kinds(&f), vec!["put0"]);
    }

    #[test]
    fn switch_plans() {
        let (mut st, a, b) = store();
        st.set_favorites(&[a.clone()]).unwrap();
        let sa = st.get(&a).unwrap();
        let sb = st.get(&b).unwrap();
        let k = |ops: Vec<SlotOp>| kinds(&ops.iter().map(SlotOp::frame).collect::<Vec<_>>());
        assert_eq!(k(plan_switch(&st, &state(-1, false, &[("", 0), (&a, 0)]), &sa)), vec!["switch1"]);
        assert_eq!(k(plan_switch(&st, &state(-1, false, &[]), &sa)), vec!["put1", "switch1"]);
        assert_eq!(k(plan_switch(&st, &state(-1, false, &[]), &sb)), vec!["put0", "switch0"]);
        // Favourite slot active with another scene: use slot 0.
        assert_eq!(k(plan_switch(&st, &state(1, false, &[("", 0), (&b, 0)]), &sa)), vec!["put0", "switch0"]);
    }

    fn connected(st: SceneStore, board: SceneStatePayload) -> SceneManager {
        let mut m = SceneManager::new(st, None);
        m.on_hello(&["menu".into(), "scenes".into()]);
        let r = m.on_state(&board);
        assert!(r.message.is_none());
        m
    }

    #[test]
    fn switch_waits_for_answers_and_retries_when_busy() {
        let (st, a, _) = store();
        let empty = state(-1, false, &[]);
        let mut m = connected(st, empty.clone());
        let frames = m.switch(&a).unwrap();
        assert_eq!(kinds(&frames), vec!["put0", "switch0"]);
        assert!(m.switch(&a).is_err(), "one switch at a time");
        assert_eq!(m.view().switching.as_deref(), Some(a.as_str()));
        // Answer to the put: nothing decided yet.
        let after_put = state(-1, false, &[(&a, 0)]);
        let r = m.on_state(&after_put);
        assert!(r.frames.is_empty() && r.retry_after_ms.is_none() && r.message.is_none());
        // Answer to the switch: busy -> retry later.
        let r = m.on_state(&err(&after_put, "busy", "scene_switch"));
        assert_eq!(r.retry_after_ms, Some(BUSY_RETRY_MS));
        assert_eq!(kinds(&m.retry()), vec!["switch0"]);
        let r = m.on_state(&state(0, false, &[(&a, 0)]));
        assert!(r.message.is_none() && r.retry_after_ms.is_none());
        assert_eq!(m.store.current.as_deref(), Some(a.as_str()));
        assert!(m.view().switching.is_none());
    }

    #[test]
    fn busy_gives_up_and_other_errors_fail() {
        let (st, a, _) = store();
        let board = state(-1, false, &[("", 0), ("", 0)]);
        let mut m = connected(st, board.clone());
        let _ = m.switch(&a).unwrap();
        let _ = m.on_state(&board); // put answered
        let mut r = m.on_state(&err(&board, "busy", "scene_switch"));
        for _ in 0..BUSY_RETRIES {
            assert!(r.retry_after_ms.is_some());
            let _ = m.retry();
            r = m.on_state(&err(&board, "busy", "scene_switch"));
        }
        assert!(r.retry_after_ms.is_none() && r.message.as_deref().unwrap().contains("busy"));
        assert!(m.store.current.is_none());

        let _ = m.switch(&a).unwrap();
        let r = m.on_state(&err(&board, "storage", "scene_put"));
        assert!(r.message.unwrap().contains("storage"));
        assert!(m.view().switching.is_none());
    }

    #[test]
    fn keyboard_side_switch_updates_current_and_dirty_flows() {
        let (mut st, a, _) = store();
        st.set_favorites(&[a.clone()]).unwrap();
        let mut m = connected(st, state(-1, false, &[("", 0), (&a, 0)]));
        let r = m.on_state(&state(1, true, &[("", 0), (&a, 0)]));
        assert!(r.frames.is_empty());
        assert_eq!(m.store.current.as_deref(), Some(a.as_str()));
        assert!(m.view().dirty);
        // Settings report -> scene_list once, not again while it is open.
        let live = DeviceSettingsPayload {
            voice_mode: Some("chat".into()),
            led_power: Some("on".into()),
            led_auto_off_s: Some(300),
            ..Default::default()
        };
        assert_eq!(kinds(&m.on_device_settings(&live)), vec!["list"]);
        assert!(m.on_device_settings(&live).is_empty());
        let _ = m.on_state(&state(1, true, &[("", 0), (&a, 0)]));
        // Save to scene: rev+1, put into its slot, no re-apply while dirty.
        let frames = m.save_live().unwrap();
        assert_eq!(kinds(&frames), vec!["put1"]);
        let s = m.store.get(&a).unwrap();
        assert_eq!((s.voice_mode.as_str(), s.lights.power.as_str(), s.lights.auto_off_s, s.rev), ("chat", "on", 300, 1));
        // Restore: re-switch the current scene.
        let _ = m.on_state(&state(1, false, &[("", 0), (&a, 1)]));
        assert_eq!(kinds(&m.restore().unwrap()), vec!["switch1"]);
    }

    #[test]
    fn presets_cannot_be_saved_live() {
        let (st, _, _) = store();
        let mut m = connected(st, state(-1, false, &[("preset01", 0)]));
        let _ = m.on_state(&state(0, true, &[("preset01", 0)]));
        m.on_device_settings(&DeviceSettingsPayload::default());
        let _ = m.on_state(&state(0, true, &[("preset01", 0)]));
        assert!(m.save_live().unwrap_err().contains("read-only"));
    }

    #[test]
    fn sync_failure_stops_retrying_until_an_edit() {
        let (mut st, a, _) = store();
        st.set_favorites(&[a.clone()]).unwrap();
        let mut m = SceneManager::new(st, None);
        m.on_hello(&["scenes".into()]);
        let board = state(-1, false, &[]);
        let r = m.on_state(&board);
        assert_eq!(kinds(&r.frames), vec!["put1"]);
        let r = m.on_state(&err(&board, "storage", "scene_put"));
        assert!(r.frames.is_empty() && r.message.is_some());
        assert!(m.on_state(&board).frames.is_empty(), "no loop");
        let frames = m.rename(&a, "Again").unwrap();
        assert_eq!(kinds(&frames), vec!["put1"]);
    }

    #[test]
    fn legacy_keyboard_gets_keymap_and_settings() {
        let (st, a, _) = store();
        let mut m = SceneManager::new(st, None);
        m.on_hello(&["menu".into()]);
        let frames = m.switch(&a).unwrap();
        assert_eq!(frames[0]["type"], "keymap");
        assert_eq!(frames[0]["payload"]["k5.kind"], "voice");
        assert_eq!(frames[0]["payload"]["k1.single"], "chat1");
        assert_eq!(frames[0]["payload"]["k1.long"], "instance1");
        assert_eq!(frames[0]["payload"]["k1.double"], "none");
        assert!(frames[0]["payload"].get("enc.long").is_none());
        assert_eq!(frames[1]["type"], "device_settings");
        assert_eq!(frames[1]["payload"]["voice_mode"], "asr");
        assert_eq!(m.store.current.as_deref(), Some(a.as_str()));
        assert!(m.view().board_legacy);
        assert!(m.import_from_board(1).is_err());
    }

    #[test]
    fn import_from_keyboard() {
        let (st, _, _) = store();
        let mut m = connected(st, state(-1, false, &[("", 0), ("zzzzzzzz", 3)]));
        assert_eq!(m.view().board_only, vec![BoardOnly { slot: 1, name: "zzzzzzzz".into() }]);
        assert_eq!(kinds(&m.import_from_board(1).unwrap()), vec!["get1"]);
        let d = SceneDataPayload {
            slot: 1,
            id: "zzzzzzzz".into(),
            rev: 3,
            name: "Other PC".into(),
            km: "00112100001222000013230000010002010000000003050400000000".into(),
            voice: "chat".into(),
            led: "on".into(),
            led_off_s: 300,
            idle: "00000000050000".into(),
            home: "keys".into(),
        };
        let r = m.on_data(&d);
        assert!(r.message.unwrap().contains("imported"));
        let s = m.store.all().into_iter().find(|s| s.name == "Other PC").unwrap();
        assert_eq!((s.voice_mode.as_str(), s.home.as_str()), ("chat", "keys"));
        assert_eq!(s.lights.idle.get("k5").map(String::as_str), Some("purple"));
        // An unasked scene_data is ignored.
        assert!(!m.on_data(&d).changed);
    }

    #[test]
    fn damaged_store_is_never_saved() {
        let mut m = SceneManager::new(SceneStore::default(), None);
        m.store_error = Some("scenes.json is damaged".into());
        assert!(m.create(scene::presets().remove(0)).unwrap_err().contains("not saved"));
        assert!(m.view().store_error.is_some());
    }
}
