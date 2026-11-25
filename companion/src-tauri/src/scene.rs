//! FR-03 application scenes: the scene model, its allowlists, the import /
//! export file format and its validation, and the compact `km` / `idle`
//! encodings the keyboard stores (shared/protocol/protocol.md "应用场景").
//!
//! Pure and IO-free; `scene_store` owns `~/.ak-companion/scenes.json`.
//! The allowlists are plain constant tables so the pending product decisions
//! (FR-03 D3 idle colours, D4 shortcuts) only change a table.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

/// Import file format marker and the only version this Companion reads.
pub const FILE_FORMAT: &str = "ki-board-scenes";
pub const FILE_VERSION: u32 = 1;
/// Largest import file, in bytes.
pub const MAX_FILE_BYTES: usize = 64 * 1024;
/// Most scenes in one import file / in the store.
pub const MAX_FILE_SCENES: usize = 32;
pub const MAX_STORE_SCENES: usize = 64;
/// Scene name limit in UTF-8 bytes (same as an LCD menu label).
pub const NAME_MAX_BYTES: usize = 24;
/// Scene id: exactly this many of [a-z0-9].
pub const ID_LEN: usize = 8;

/// Key ids in keyboard order (the `km` / `idle` byte order).
pub const KEYS: [&str; 7] = ["k1", "k2", "k3", "k4", "k5", "k6", "enc"];

/// Fixed actions and their keymap bytes (firmware ak_keymap.h).
const FIXED_ACTIONS: &[(&str, u8)] = &[
    ("none", 0x00),
    ("cancel_char", 0x01),
    ("cancel_all", 0x02),
    ("confirm", 0x03),
    ("model_menu", 0x04),
    ("model_toggle", 0x05),
    ("scene_menu", 0x06),
];

/// Limited shortcuts (FR-03 D4). SHORTCUT n = byte 0x30 + n, so the ORDER is
/// the wire table shared with the firmware (ak_keymap.c s_shortcuts): append
/// only. The chords live in `kirocrew_control::SHORTCUT_CHORDS`.
pub const SHORTCUTS: [&str; 14] = [
    "key_enter",
    "key_esc",
    "key_up",
    "key_down",
    "key_left",
    "key_right",
    "key_backspace",
    "key_delete_word",
    "key_clear_input",
    "key_command_bar",
    "key_prev_chat",
    "key_next_chat",
    "key_focus_input",
    "key_new_chat",
];
const SHORTCUT_BASE: u8 = 0x30;

/// Idle colours (ak_light_t byte), FR-03 D3: off / green / blue / purple;
/// red and yellow keep their FR-09 meanings. The firmware accepts the same 4.
pub const IDLE_COLORS: &[(&str, u8)] = &[("off", 0), ("green", 2), ("blue", 3), ("purple", 5)];
/// All colour bytes the keyboard knows, for decoding a keyboard copy.
const ALL_COLORS: &[(&str, u8)] =
    &[("off", 0), ("red", 1), ("green", 2), ("blue", 3), ("yellow", 4), ("purple", 5)];

pub const KNOB_MODES: &[&str] = &["arrows", "chats", "none"];
pub const VOICE_MODES: &[&str] = &["asr", "chat"];
/// LCD home layouts (FR-03 D2): sessions, or the keys page.
pub const HOME_LAYOUTS: &[&str] = &["sessions", "keys"];
pub const LED_POWERS: &[&str] = &["off", "on", "auto"];
pub const LED_AUTO_OFF_S: [u32; 4] = [30, 60, 300, 900];

/// One key's bindings. A missing gesture is `none`.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct KeyBinding {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kind: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub single: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub double: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub long: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Keys {
    #[serde(default)]
    pub k1: KeyBinding,
    #[serde(default)]
    pub k2: KeyBinding,
    #[serde(default)]
    pub k3: KeyBinding,
    #[serde(default)]
    pub k4: KeyBinding,
    #[serde(default)]
    pub k5: KeyBinding,
    #[serde(default)]
    pub k6: KeyBinding,
    #[serde(default)]
    pub enc: KeyBinding,
}

impl Keys {
    fn all(&self) -> [(&'static str, &KeyBinding); 7] {
        [
            ("k1", &self.k1),
            ("k2", &self.k2),
            ("k3", &self.k3),
            ("k4", &self.k4),
            ("k5", &self.k5),
            ("k6", &self.k6),
            ("enc", &self.enc),
        ]
    }

    fn slot_mut(&mut self, i: usize) -> &mut KeyBinding {
        match i {
            0 => &mut self.k1,
            1 => &mut self.k2,
            2 => &mut self.k3,
            3 => &mut self.k4,
            4 => &mut self.k5,
            5 => &mut self.k6,
            _ => &mut self.enc,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Lights {
    pub power: String,
    pub auto_off_s: u32,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub idle: BTreeMap<String, String>,
}

/// A scene as stored and exported.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Scene {
    #[serde(default)]
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub rev: u32,
    pub keys: Keys,
    pub knob: String,
    pub voice_mode: String,
    pub home: String,
    pub lights: Lights,
}

/// The import / export file.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SceneFile {
    pub format: String,
    pub version: u32,
    pub scenes: Vec<Scene>,
}

/// Keymap byte of an action name, or `None` when it is not on the allowlist.
pub fn action_byte(name: &str) -> Option<u8> {
    if let Some((_, b)) = FIXED_ACTIONS.iter().find(|(n, _)| *n == name) {
        return Some(*b);
    }
    if let Some(i) = SHORTCUTS.iter().position(|s| *s == name) {
        return Some(SHORTCUT_BASE + 1 + i as u8);
    }
    for (prefix, base) in [("chat", 0x10u8), ("instance", 0x20u8)] {
        if let Some(d) = name.strip_prefix(prefix) {
            if d.len() == 1 && matches!(d.as_bytes()[0], b'1'..=b'9') {
                return Some(base + (d.as_bytes()[0] - b'0'));
            }
        }
    }
    None
}

/// Action name of a keymap byte (inverse of [`action_byte`]).
pub fn action_name(b: u8) -> Option<String> {
    if let Some((n, _)) = FIXED_ACTIONS.iter().find(|(_, v)| *v == b) {
        return Some((*n).to_string());
    }
    match b {
        0x11..=0x19 => Some(format!("chat{}", b - 0x10)),
        0x21..=0x29 => Some(format!("instance{}", b - 0x20)),
        _ if b > SHORTCUT_BASE && ((b - SHORTCUT_BASE) as usize) <= SHORTCUTS.len() => {
            Some(SHORTCUTS[(b - SHORTCUT_BASE - 1) as usize].to_string())
        }
        _ => None,
    }
}

/// 1-based shortcut number of a `key_*` name.
pub fn shortcut_index(name: &str) -> Option<u8> {
    SHORTCUTS.iter().position(|s| *s == name).map(|i| i as u8 + 1)
}

fn is_bad_char(c: char) -> bool {
    c.is_control()
        || matches!(c, '\u{200E}' | '\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2066}'..='\u{2069}' | '\u{2028}' | '\u{2029}')
}

/// Trimmed name if it is 1..=24 bytes with no control / bidi characters.
pub fn check_name(name: &str) -> Result<String, String> {
    let t = name.trim();
    if t.is_empty() {
        return Err("name is empty".into());
    }
    if t.len() > NAME_MAX_BYTES {
        return Err(format!("name is longer than {NAME_MAX_BYTES} bytes"));
    }
    if t.chars().any(is_bad_char) {
        return Err("name contains a control or direction character".into());
    }
    Ok(t.to_string())
}

pub fn id_ok(id: &str) -> bool {
    id.len() == ID_LEN && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
}

/// Validate every field (the firmware applies the same keymap rules). The
/// error names the first offending field.
pub fn validate(s: &Scene) -> Result<(), String> {
    check_name(&s.name).map_err(|e| format!("name: {e}"))?;
    let mut voice_keys = 0;
    let mut has_confirm = false;
    for (id, k) in s.keys.all() {
        let is_voice = match k.kind.as_deref() {
            None | Some("normal") => false,
            Some("voice") => true,
            Some(other) => return Err(format!("keys.{id}.kind: unknown kind '{other}'")),
        };
        if is_voice {
            if id == "enc" {
                return Err("keys.enc.kind: the knob cannot be the voice key".into());
            }
            if k.single.is_some() || k.double.is_some() || k.long.is_some() {
                return Err(format!("keys.{id}: the voice key has no gestures"));
            }
            voice_keys += 1;
        }
        for (g, v) in [("single", &k.single), ("double", &k.double), ("long", &k.long)] {
            let Some(v) = v else { continue };
            if action_byte(v).is_none() {
                return Err(format!("keys.{id}.{g}: unknown action '{v}'"));
            }
            if id == "enc" && g == "long" && v != "none" {
                return Err("keys.enc.long: reserved for the keyboard settings page".into());
            }
            has_confirm |= v == "confirm";
        }
    }
    if voice_keys > 1 {
        return Err("keys: more than one voice key".into());
    }
    if !KNOB_MODES.contains(&s.knob.as_str()) {
        return Err(format!("knob: unknown mode '{}'", s.knob));
    }
    if !VOICE_MODES.contains(&s.voice_mode.as_str()) {
        return Err(format!("voice_mode: unknown mode '{}'", s.voice_mode));
    }
    if s.voice_mode == "asr" && voice_keys == 1 && !has_confirm {
        return Err("keys: no key is bound to confirm, so voice text cannot be confirmed".into());
    }
    if !HOME_LAYOUTS.contains(&s.home.as_str()) {
        return Err(format!("home: unknown layout '{}'", s.home));
    }
    if !LED_POWERS.contains(&s.lights.power.as_str()) {
        return Err(format!("lights.power: unknown value '{}'", s.lights.power));
    }
    if !LED_AUTO_OFF_S.contains(&s.lights.auto_off_s) {
        return Err(format!("lights.auto_off_s: must be one of {LED_AUTO_OFF_S:?}"));
    }
    for (k, c) in &s.lights.idle {
        if !KEYS.contains(&k.as_str()) {
            return Err(format!("lights.idle: unknown key '{k}'"));
        }
        if !IDLE_COLORS.iter().any(|(n, _)| n == c) {
            return Err(format!("lights.idle.{k}: colour '{c}' is not allowed"));
        }
    }
    Ok(())
}

/// 56 lower-case hex chars: per key [kind, single, double, long]. The scene
/// must be valid.
pub fn km_hex(s: &Scene) -> String {
    let byte = |v: &Option<String>| v.as_deref().and_then(action_byte).unwrap_or(0);
    let mut out = String::with_capacity(56);
    for (_, k) in s.keys.all() {
        let kind = u8::from(k.kind.as_deref() == Some("voice"));
        for b in [kind, byte(&k.single), byte(&k.double), byte(&k.long)] {
            out.push_str(&format!("{b:02x}"));
        }
    }
    out
}

/// 14 lower-case hex chars: idle colour per key in [`KEYS`] order.
pub fn idle_hex(s: &Scene) -> String {
    KEYS.iter()
        .map(|k| {
            let b = s
                .lights
                .idle
                .get(*k)
                .and_then(|c| ALL_COLORS.iter().find(|(n, _)| n == c))
                .map_or(0, |(_, b)| *b);
            format!("{b:02x}")
        })
        .collect()
}

fn hex_bytes(hex: &str, n: usize) -> Option<Vec<u8>> {
    if hex.len() != n * 2 || !hex.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) {
        return None;
    }
    (0..n).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).ok()).collect()
}

/// Decode a keyboard `km` into key bindings (`none` gestures left out).
pub fn keys_from_km(hex: &str) -> Option<Keys> {
    let bytes = hex_bytes(hex, 28)?;
    let mut keys = Keys::default();
    for i in 0..7 {
        let b = &bytes[i * 4..i * 4 + 4];
        let slot = keys.slot_mut(i);
        match b[0] {
            0 => {}
            1 => slot.kind = Some("voice".into()),
            _ => return None,
        }
        let gest = |x: u8| -> Option<Option<String>> {
            if x == 0 {
                Some(None)
            } else {
                action_name(x).map(Some)
            }
        };
        slot.single = gest(b[1])?;
        slot.double = gest(b[2])?;
        slot.long = gest(b[3])?;
    }
    Some(keys)
}

/// Decode a keyboard `idle` into the colour map (off entries left out).
pub fn idle_from_hex(hex: &str) -> Option<BTreeMap<String, String>> {
    let bytes = hex_bytes(hex, 7)?;
    let mut out = BTreeMap::new();
    for (k, b) in KEYS.iter().zip(bytes) {
        let (name, _) = ALL_COLORS.iter().find(|(_, v)| *v == b)?;
        if *name != "off" {
            out.insert((*k).to_string(), (*name).to_string());
        }
    }
    Some(out)
}

/// The `scene_put` frame for keyboard slot 0..=4. The scene must be valid.
pub fn scene_put_payload(slot: u8, s: &Scene) -> Value {
    json!({
        "v": 1,
        "type": "scene_put",
        "payload": {
            "slot": slot,
            "id": s.id,
            "rev": s.rev,
            "name": s.name,
            "km": km_hex(s),
            "voice": s.voice_mode,
            "led": s.lights.power,
            "led_off_s": s.lights.auto_off_s,
            "idle": idle_hex(s),
            "home": s.home,
        }
    })
}

/// Parse and validate an import file. Every scene must pass; ids and revs
/// are NOT trusted (the store assigns new ones). Errors name the position.
pub fn parse_file(bytes: &[u8]) -> Result<Vec<Scene>, String> {
    if bytes.len() > MAX_FILE_BYTES {
        return Err(format!("file is larger than {} KiB", MAX_FILE_BYTES / 1024));
    }
    let text = std::str::from_utf8(bytes).map_err(|_| "file is not UTF-8".to_string())?;
    let file: SceneFile = serde_json::from_str(text).map_err(|e| format!("not a scene file: {e}"))?;
    if file.format != FILE_FORMAT {
        return Err(format!("format must be '{FILE_FORMAT}'"));
    }
    if file.version != FILE_VERSION {
        return Err(format!("unsupported version {} (this Companion reads {FILE_VERSION})", file.version));
    }
    if file.scenes.is_empty() || file.scenes.len() > MAX_FILE_SCENES {
        return Err(format!("a file holds 1..={MAX_FILE_SCENES} scenes"));
    }
    let mut out = Vec::with_capacity(file.scenes.len());
    for (i, mut s) in file.scenes.into_iter().enumerate() {
        validate(&s).map_err(|e| format!("scenes[{i}].{e}"))?;
        s.name = s.name.trim().to_string();
        s.id.clear();
        s.rev = 0;
        out.push(s);
    }
    Ok(out)
}

/// Serialize scenes into an export file.
pub fn export_file(scenes: &[Scene]) -> String {
    let file = SceneFile { format: FILE_FORMAT.into(), version: FILE_VERSION, scenes: scenes.to_vec() };
    serde_json::to_string_pretty(&file).unwrap_or_default()
}

/// Built-in presets (FR-03 D4): read-only, copy to edit (D5). K1-K3 follow
/// the K1-K3 latency fix, plan C: single = chat (instant, no double-click
/// wait), long = instance / navigation, double unbound. "编程" is that
/// default keymap plus the scene picker on the knob double-click.
pub fn presets() -> Vec<Scene> {
    let b = |s: &str, d: &str, l: &str| KeyBinding {
        kind: None,
        single: Some(s.into()),
        double: Some(d.into()),
        long: Some(l.into()),
    };
    let voice = || KeyBinding { kind: Some("voice".into()), ..Default::default() };
    let knob = || KeyBinding { double: Some("scene_menu".into()), ..Default::default() };
    let lights = |idle: &[(&str, &str)]| Lights {
        power: "auto".into(),
        auto_off_s: 60,
        idle: idle.iter().map(|(k, c)| ((*k).to_string(), (*c).to_string())).collect(),
    };
    let coding_keys = Keys {
        k1: b("chat1", "none", "instance1"),
        k2: b("chat2", "none", "instance2"),
        k3: b("chat3", "none", "instance3"),
        k4: b("cancel_char", "none", "cancel_all"),
        k5: voice(),
        k6: b("confirm", "model_toggle", "model_menu"),
        enc: knob(),
    };
    vec![
        Scene {
            id: "preset01".into(),
            name: "编程".into(),
            rev: 0,
            keys: coding_keys.clone(),
            knob: "arrows".into(),
            voice_mode: "asr".into(),
            home: "sessions".into(),
            lights: lights(&[]),
        },
        Scene {
            id: "preset02".into(),
            name: "语音对话".into(),
            rev: 0,
            keys: coding_keys.clone(),
            knob: "arrows".into(),
            voice_mode: "chat".into(),
            home: "sessions".into(),
            lights: lights(&[("k5", "purple")]),
        },
        Scene {
            id: "preset03".into(),
            name: "导航".into(),
            rev: 0,
            keys: Keys {
                k1: b("chat1", "none", "key_prev_chat"),
                k2: b("chat2", "none", "key_new_chat"),
                k3: b("chat3", "none", "key_next_chat"),
                k4: b("key_esc", "none", "cancel_all"),
                ..coding_keys
            },
            knob: "chats".into(),
            voice_mode: "asr".into(),
            home: "keys".into(),
            lights: lights(&[("k1", "blue"), ("k2", "blue"), ("k3", "blue")]),
        },
    ]
}

/// Presets cannot be deleted (FR-03 D5).
pub const PRESETS_DELETABLE: bool = false;

pub fn is_preset_id(id: &str) -> bool {
    presets().iter().any(|p| p.id == id)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn good() -> Scene {
        presets().remove(0)
    }

    #[test]
    fn presets_are_valid_and_match_firmware_vector() {
        let p = presets();
        assert_eq!(p.iter().map(|s| s.name.as_str()).collect::<Vec<_>>(), ["编程", "语音对话", "导航"]);
        for s in &p {
            validate(s).unwrap();
            assert!(id_ok(&s.id) && is_preset_id(&s.id));
        }
        // 编程: K1-K3 single chatN, long instanceN, no double (plan C); K4,
        // K5, K6 as the v0.3 defaults; scene_menu on the knob double.
        assert_eq!(km_hex(&p[0]), "00110021001200220013002300010002010000000003050400000600");
        assert_eq!(km_hex(&p[1]), km_hex(&p[0]));
        // No preset makes a K1-K3 single click wait for a double click.
        for s in &p {
            for k in [&s.keys.k1, &s.keys.k2, &s.keys.k3] {
                assert_eq!(k.double.as_deref(), Some("none"), "{}", s.name);
            }
        }
        assert_eq!(idle_hex(&p[0]), "00000000000000");
        assert_eq!(idle_hex(&p[1]), "00000000050000");
        assert_eq!(idle_hex(&p[2]), "03030300000000");
        assert_eq!(p[2].keys.k1.long.as_deref(), Some("key_prev_chat"));
        assert_eq!(p[2].keys.k3.long.as_deref(), Some("key_next_chat"));
        assert_eq!(p[2].keys.k4.single.as_deref(), Some("key_esc"));
    }

    #[test]
    fn action_bytes_round_trip() {
        for b in 0u8..=0xff {
            if let Some(n) = action_name(b) {
                assert_eq!(action_byte(&n), Some(b), "{n}");
            }
        }
        assert_eq!(action_byte("key_enter"), Some(0x31));
        assert_eq!(action_byte("key_new_chat"), Some(0x3e));
        assert_eq!(action_byte("scene_menu"), Some(0x06));
        assert_eq!(action_byte("chat0"), None);
        assert_eq!(action_byte("chat10"), None);
        assert_eq!(action_byte("key_f13"), None);
        assert_eq!(action_name(0x3f), None);
        assert_eq!(action_name(0x30), None);
    }

    #[test]
    fn km_and_idle_decode_back() {
        let mut s = good();
        s.keys.enc.double = Some("scene_menu".into());
        s.keys.k4.single = Some("key_esc".into());
        s.lights.idle.insert("k5".into(), "purple".into());
        validate(&s).unwrap();
        let mut keys = keys_from_km(&km_hex(&s)).unwrap();
        // none gestures decode as absent
        let mut want = s.keys.clone();
        for i in 0..7 {
            let k = want.slot_mut(i);
            for g in [&mut k.single, &mut k.double, &mut k.long] {
                if g.as_deref() == Some("none") {
                    *g = None;
                }
            }
        }
        assert_eq!(std::mem::take(&mut keys), want);
        assert_eq!(idle_from_hex(&idle_hex(&s)).unwrap(), s.lights.idle);
        assert!(keys_from_km("00").is_none());
        assert!(keys_from_km("00112100001222000013230000010002010000000003050600313e00").is_some());
        assert!(keys_from_km("00112100001222000013230000010002010000000003050600313E00").is_none());
        assert!(keys_from_km("02112100001222000013230000010002010000000003050400000000").is_none());
        assert!(idle_from_hex("00000000000006").is_none());
    }

    #[test]
    fn validation_rejects_bad_scenes() {
        let cases: Vec<(fn(&mut Scene), &str)> = vec![
            (|s| s.name = "".into(), "name"),
            (|s| s.name = "1234567890123456789012345".into(), "name"),
            (|s| s.name = "a\u{202e}b".into(), "name"),
            (|s| s.name = "a\nb".into(), "name"),
            (|s| s.keys.k1.single = Some("rm_rf".into()), "keys.k1.single"),
            (|s| s.keys.k1.single = Some("key_ctrl_alt_del".into()), "keys.k1.single"),
            (|s| s.keys.enc.long = Some("chat1".into()), "keys.enc.long"),
            (|s| s.keys.enc.kind = Some("voice".into()), "keys.enc.kind"),
            (|s| s.keys.k1 = KeyBinding { kind: Some("voice".into()), ..Default::default() }, "more than one"),
            (|s| s.keys.k5.single = Some("chat1".into()), "keys.k5"),
            (|s| s.keys.k2.kind = Some("macro".into()), "keys.k2.kind"),
            (|s| s.keys.k6.single = Some("key_enter".into()), "confirm"),
            (|s| s.knob = "volume".into(), "knob"),
            (|s| s.voice_mode = "talk".into(), "voice_mode"),
            (|s| s.home = "grid".into(), "home"),
            (|s| s.lights.power = "blink".into(), "lights.power"),
            (|s| s.lights.auto_off_s = 61, "lights.auto_off_s"),
            (|s| drop(s.lights.idle.insert("k7".into(), "blue".into())), "lights.idle"),
            (|s| drop(s.lights.idle.insert("k1".into(), "red".into())), "lights.idle.k1"),
        ];
        for (edit, want) in cases {
            let mut s = good();
            edit(&mut s);
            let err = validate(&s).unwrap_err();
            assert!(err.contains(want), "{err} should mention {want}");
        }
        // Chat mode needs no confirm key.
        let mut s = good();
        s.voice_mode = "chat".into();
        s.keys.k6.single = Some("key_enter".into());
        validate(&s).unwrap();
    }

    fn file_with(scenes: Value) -> Vec<u8> {
        json!({"format": FILE_FORMAT, "version": 1, "scenes": scenes}).to_string().into_bytes()
    }

    #[test]
    fn import_round_trip_resets_id_and_rev() {
        let mut s = good();
        s.id = "abcd1234".into();
        s.rev = 9;
        s.name = "  编程  ".into();
        let text = export_file(&[s.clone()]);
        let got = parse_file(text.as_bytes()).unwrap();
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].id, "");
        assert_eq!(got[0].rev, 0);
        assert_eq!(got[0].name, "编程");
        assert_eq!(got[0].keys, s.keys);
    }

    #[test]
    fn import_rejects_bad_files() {
        let scene = serde_json::to_value(good()).unwrap();
        assert!(parse_file(&vec![b' '; MAX_FILE_BYTES + 1]).unwrap_err().contains("larger"));
        assert!(parse_file(b"\xff\xfe").unwrap_err().contains("UTF-8"));
        assert!(parse_file(b"[]").unwrap_err().contains("not a scene file"));
        let wrong = json!({"format": "other", "version": 1, "scenes": [scene]}).to_string();
        assert!(parse_file(wrong.as_bytes()).unwrap_err().contains("format"));
        let v2 = json!({"format": FILE_FORMAT, "version": 2, "scenes": [scene]}).to_string();
        assert!(parse_file(v2.as_bytes()).unwrap_err().contains("version"));
        assert!(parse_file(&file_with(json!([]))).unwrap_err().contains("1..="));
        let many: Vec<Value> = (0..33).map(|_| scene.clone()).collect();
        assert!(parse_file(&file_with(Value::Array(many))).unwrap_err().contains("1..="));
        let extra = json!({"format": FILE_FORMAT, "version": 1, "scenes": [scene], "x": 1}).to_string();
        assert!(parse_file(extra.as_bytes()).unwrap_err().contains("unknown field"));
        let mut unknown = scene.clone();
        unknown["script"] = json!("calc.exe");
        assert!(parse_file(&file_with(json!([unknown]))).unwrap_err().contains("unknown field"));
        let mut nested = scene.clone();
        nested["keys"]["k1"]["triple"] = json!("chat1");
        assert!(parse_file(&file_with(json!([nested]))).unwrap_err().contains("unknown field"));
        let mut bad = scene.clone();
        bad["keys"]["k3"]["long"] = json!("format_disk");
        let err = parse_file(&file_with(json!([scene, bad]))).unwrap_err();
        assert!(err.starts_with("scenes[1].keys.k3.long"), "{err}");
    }

    #[test]
    fn scene_put_fits_one_line() {
        let mut s = good();
        s.id = "zzzzzzzz".into();
        s.rev = u32::MAX;
        s.name = "鹏鹏鹏鹏鹏鹏鹏鹏".into(); // 24 bytes
        for k in KEYS {
            s.lights.idle.insert(k.into(), "purple".into());
        }
        let v = scene_put_payload(4, &s);
        let line = serde_json::to_string(&v).unwrap();
        assert!(line.len() <= 511, "{} bytes", line.len());
        assert_eq!(v["payload"]["km"], km_hex(&s));
        assert_eq!(v["payload"]["idle"], "05050505050505");
    }
}
