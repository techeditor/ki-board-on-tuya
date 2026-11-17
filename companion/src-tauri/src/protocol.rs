//! Wire protocol (v1) shared between firmware and companion.
//!
//! Line-delimited JSON: every object is followed by a single `\n`. See
//! `../../../shared/protocol/protocol.md` for the authoritative spec. This
//! module is pure (no Tauri / IO deps) so it stays unit-testable on any host.
//!
//! Decoding is deliberately *tolerant* and mirrors the firmware `ak_rx` /
//! `ak_protocol` semantics: blank lines, non-JSON, partial frames, missing
//! fields, unknown `type`s, and unknown enum values must never panic. Unknown
//! frame types are surfaced as [`Inbound::Unknown`] so the receiver can log and
//! ignore them (the spec requires ignoring unknown `type`s).

use serde::{Deserialize, Serialize};
use serde_json::Value;

/// Current protocol version carried in the `v` envelope field.
pub const PROTOCOL_VERSION: u8 = 1;

/// Upper bound on the reassembly buffer for a single line. A stream that never
/// emits `\n` must not grow memory without bound (matches the fixed-size RX
/// buffer on the firmware side).
const MAX_LINE_BYTES: usize = 8 * 1024;

// ───────────────────────────── Encoding ─────────────────────────────

/// Encode a JSON payload as a single protocol line (JSON + trailing newline).
pub fn encode_json_line(payload: &Value) -> Result<Vec<u8>, String> {
    let mut line = serde_json::to_vec(payload).map_err(|err| err.to_string())?;
    line.push(b'\n');
    Ok(line)
}

/// Companion -> board handshake.
pub fn hello_payload() -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "hello",
        "payload": {
            "client": "companion",
            "version": env!("CARGO_PKG_VERSION"),
            // PC ASR text comes back as input_restore: the keyboard may wait in ②.
            "pc_asr": true
        }
    })
}

/// Companion -> board graceful disconnect. Plain UART has no hardware
/// disconnect signal, so the board cannot otherwise tell the companion closed
/// the port; this lets it flip its link header to "USB Disconnected".
pub fn bye_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "bye" })
}

/// Companion -> board keepalive request.
pub fn ping_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "ping" })
}

/// Companion -> board keepalive reply (companion answers board pings too).
pub fn pong_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "pong" })
}

/// PC -> board: agent status / text for one Chat A/B/C LCD slot.
pub fn display_payload(chat_slot: u8, agent_name: &str, state: &str, text: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "display",
        "payload": {
            "chat_slot": chat_slot,
            "agent_name": agent_name,
            "state": state,
            "text": text
        }
    })
}

/// PC -> board: RGB LED command.
pub fn led_payload(state: &str, color: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "led",
        "payload": { "state": state, "color": color }
    })
}

/// PC -> board: select the active voice engine (`system` / `third_party`).
pub fn config_payload(voice_engine: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "config",
        "payload": { "voice_engine": voice_engine }
    })
}

/// PC -> board: synchronize optional display aliases for slots A, B, and C,
/// and the name of the Crew session bound to each (Settings > Bind session
/// page; empty = not bound, clipped to 48 bytes). Empty aliases tell the AK
/// to render the corresponding default letter.
pub fn bindings_payload(aliases: &[String], names: &[String]) -> Value {
    let alias = |index: usize| aliases.get(index).map(String::as_str).unwrap_or("");
    let name = |index: usize| {
        crate::menu_wire::clip_text(names.get(index).map(String::as_str).unwrap_or(""), BIND_NAME_MAX)
    };
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "bindings",
        "payload": {
            "alias_a": alias(0),
            "alias_b": alias(1),
            "alias_c": alias(2),
            "name_a": name(0),
            "name_b": name(1),
            "name_c": name(2)
        }
    })
}

/// Longest bound-session name in `bindings` (bytes).
pub const BIND_NAME_MAX: usize = 48;

/// PC -> board: answer to `bind_session` that ends a step of the keyboard's
/// Bind session flow. `result`: `bound` / `unbound` / `back` / `closed` /
/// `error`; `msg` (status line, <= 48 bytes) is omitted when empty.
pub fn bind_done_payload(slot: u8, result: &str, msg: &str) -> Value {
    let mut payload = serde_json::json!({ "slot": slot, "result": result });
    if !msg.is_empty() {
        payload["msg"] = Value::String(crate::menu_wire::clip_text(msg, crate::menu_wire::NOTICE_MAX));
    }
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "bind_done", "payload": payload })
}

/// PC -> board: set the current device volume and screen brightness.
pub fn device_settings_payload(volume: u8, brightness: u8) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "device_settings",
        "payload": { "volume": volume, "brightness": brightness }
    })
}

/// PC -> board: set the interface language ("zh" / "en", FR-01). The board
/// stores it, redraws the LCD and echoes `device_settings_state`.
pub fn device_language_payload(language: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "device_settings",
        "payload": { "language": language }
    })
}

/// FR-02: the normal keyboard settings the Companion may change (USB or the
/// BLE secure channel). Only the fields that are `Some` are sent; the board
/// rejects the whole frame if any of them is out of range.
#[derive(Clone, Debug, Default, PartialEq, Eq, Deserialize)]
pub struct DeviceSettingsUpdate {
    #[serde(default)]
    pub volume: Option<u8>,
    #[serde(default)]
    pub brightness: Option<u8>,
    #[serde(default)]
    pub language: Option<String>,
    #[serde(default)]
    pub voice_mode: Option<String>,
    #[serde(default)]
    pub led_power: Option<String>,
    #[serde(default)]
    pub led_auto_off_s: Option<u32>,
    #[serde(default)]
    pub font_size: Option<u8>,
}

/// Auto power-off delays the keyboard offers (ak_led_policy.c).
pub const LED_AUTO_OFF_S: [u32; 4] = [30, 60, 300, 900];

/// LCD font sizes in px the keyboard offers (ak_text.h).
pub const FONT_SIZES: [u8; 2] = [12, 16];

impl DeviceSettingsUpdate {
    /// Same ranges as the firmware, so the UI gets an error instead of a
    /// silently dropped frame.
    pub fn validate(&self) -> Result<(), String> {
        let empty = self.volume.is_none()
            && self.brightness.is_none()
            && self.language.is_none()
            && self.voice_mode.is_none()
            && self.led_power.is_none()
            && self.led_auto_off_s.is_none()
            && self.font_size.is_none();
        if empty {
            return Err("no setting to change".into());
        }
        if self.volume.is_some_and(|v| v > 100) {
            return Err("volume must be 0..=100".into());
        }
        if self.brightness.is_some_and(|b| !(10..=100).contains(&b)) {
            return Err("brightness must be 10..=100".into());
        }
        if self.language.as_deref().is_some_and(|l| l != "zh" && l != "en") {
            return Err("language must be zh or en".into());
        }
        if self.voice_mode.as_deref().is_some_and(|m| m != "asr" && m != "chat") {
            return Err("voice_mode must be asr or chat".into());
        }
        if self.led_power.as_deref().is_some_and(|m| !matches!(m, "off" | "on" | "auto")) {
            return Err("led_power must be off, on or auto".into());
        }
        if self.led_auto_off_s.is_some_and(|s| !LED_AUTO_OFF_S.contains(&s)) {
            return Err("led_auto_off_s must be 30, 60, 300 or 900".into());
        }
        if self.font_size.is_some_and(|s| !FONT_SIZES.contains(&s)) {
            return Err("font_size must be 12 or 16".into());
        }
        Ok(())
    }
}

/// PC -> board: change some of the normal settings (FR-02).
pub fn device_settings_update_payload(u: &DeviceSettingsUpdate) -> Value {
    let mut p = serde_json::Map::new();
    if let Some(v) = u.volume {
        p.insert("volume".into(), v.into());
    }
    if let Some(v) = u.brightness {
        p.insert("brightness".into(), v.into());
    }
    if let Some(v) = &u.language {
        p.insert("language".into(), v.clone().into());
    }
    if let Some(v) = &u.voice_mode {
        p.insert("voice_mode".into(), v.clone().into());
    }
    if let Some(v) = &u.led_power {
        p.insert("led_power".into(), v.clone().into());
    }
    if let Some(v) = u.led_auto_off_s {
        p.insert("led_auto_off_s".into(), v.into());
    }
    if let Some(v) = u.font_size {
        p.insert("font_size".into(), v.into());
    }
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "device_settings", "payload": Value::Object(p) })
}

/// PC -> board (USB only): ask the keyboard to open the pairing window; the
/// keyboard shows its own K6 / K1 confirm (FR-02).
pub fn prov_open_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "prov_open" })
}

/// PC -> board (USB only): ask the keyboard to clear its BLE pairing key; the
/// keyboard shows its own K6 / K1 confirm (FR-02).
pub fn ble_unpair_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "ble_unpair" })
}

/// PC -> board: ask for the remembered Wi-Fi networks (FR-15). Read only; the
/// answer is a `wifi_list_state` frame with SSIDs and no passwords. Allowed on
/// USB and on the paired BLE channel.
pub fn wifi_list_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "wifi_list" })
}

/// PC -> board (USB only): forget one remembered network (FR-15). Needs no
/// pairing window; it only removes data.
pub fn wifi_forget_payload(ssid: &str) -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "wifi_forget", "payload": { "ssid": ssid } })
}

/// PC -> board (USB only): forget every remembered network (FR-15).
pub fn wifi_clear_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "wifi_clear" })
}

/// PC -> board: downlink per-gesture keymap bindings (plan A, stage 乙). Each
/// binding is a flat dotted field "<key>.<slot>" -> action wire name, where
/// slot is "kind" (value "voice"/"normal") or a gesture "single"/"double"/
/// "long" (value an action: none/cancel_char/cancel_all/confirm/chatN/
/// instanceN). Only non-empty bindings are emitted; omitted fields leave the
/// board's current binding (partial update). The firmware caches the resolved
/// table in TAL KV.
pub fn keymap_payload(bindings: &[(String, String)]) -> Value {
    let mut payload = serde_json::Map::new();
    for (field, value) in bindings {
        if !field.is_empty() && !value.is_empty() {
            payload.insert(field.clone(), Value::String(value.clone()));
        }
    }
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "keymap",
        "payload": Value::Object(payload)
    })
}

/// PC -> board: provision Wi-Fi credentials. The board connects via netmgr and
/// reports back a `wifi_status` frame.
pub fn wifi_payload(ssid: &str, pass: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "wifi",
        "payload": { "ssid": ssid, "pass": pass }
    })
}

/// PC -> board: provision the device license (UUID + AuthKey). Written to the
/// board's persistent storage; PID stays compile-time.
pub fn license_payload(uuid: &str, authkey: &str) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "license",
        "payload": { "uuid": uuid, "authkey": authkey }
    })
}

/// PC -> board: explicitly request masked License diagnostics over USB/UART0.
pub fn license_read_payload() -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "license_read"
    })
}

/// PC -> board: clear Wi-Fi/cloud binding and reboot into pairing mode
/// ("enter pairing" one-click). The board's License (UUID/AuthKey) is
/// preserved. Destructive, so the board only accepts it over USB/UART.
pub fn reset_netcfg_payload() -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "reset_netcfg"
    })
}

/// PC -> board (USB only, keyboard pairing window open): create a BLE pairing
/// key. The board answers once with `ble_pair_key` over UART0.
pub fn ble_pair_payload() -> Value {
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "ble_pair" })
}

/// PC -> board: acknowledge a board event by its sequence number.
pub fn ack_payload(seq: u64) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "ack",
        "payload": { "seq": seq }
    })
}

// ───────────────────────────── Enums ─────────────────────────────

/// `key` action. Decoded tolerantly from the wire string: unknown values map to
/// `Unknown` rather than failing. `Chat(n)` / `Instance(n)` carry the resolved
/// target the firmware put on the wire ("chat5" / "instance2"), so the keymap is
/// the single source of truth and the Companion never infers N from the key id.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum KeyAction {
    Down,
    Up,
    Long,
    /// Firmware-classified final gesture: single (see protocol.md).
    Single,
    /// Firmware-classified final gesture: a double click.
    Double,
    /// Resolved chat jump target N (1..=9) — wire "chatN". Companion -> Alt+N.
    Chat(u8),
    /// Resolved instance switch target N (1..=9) — wire "instanceN". -> Ctrl+N.
    Instance(u8),
    /// Pending-confirm (post-ASR) edit: delete one character. -> Backspace.
    CancelChar,
    /// Pending-confirm edit: delete all / cancel. -> Ctrl+A, Backspace + exit.
    CancelAll,
    /// Pending-confirm edit: confirm / submit. -> Enter + exit.
    Confirm,
    /// Open the LCD model/mode menu (default K6 long).
    ModelMenu,
    /// Toggle the current chat between Auto and the last picked model (default K6 double).
    ModelToggle,
    /// K4 single in standby while ESC'd requests wait: show them again.
    MenuRecall,
    Unknown,
}

impl KeyAction {
    fn from_wire(s: &str) -> KeyAction {
        match s {
            "down" => KeyAction::Down,
            "up" => KeyAction::Up,
            "long" => KeyAction::Long,
            "single" => KeyAction::Single,
            "double" => KeyAction::Double,
            "cancel_char" => KeyAction::CancelChar,
            "cancel_all" => KeyAction::CancelAll,
            "confirm" => KeyAction::Confirm,
            "model_menu" => KeyAction::ModelMenu,
            "model_toggle" => KeyAction::ModelToggle,
            "menu_recall" => KeyAction::MenuRecall,
            _ => {
                // chatN / instanceN with a single digit 1..=9.
                if let Some(d) = s.strip_prefix("chat") {
                    if let Ok(n) = d.parse::<u8>() {
                        if (1..=9).contains(&n) {
                            return KeyAction::Chat(n);
                        }
                    }
                }
                if let Some(d) = s.strip_prefix("instance") {
                    if let Ok(n) = d.parse::<u8>() {
                        if (1..=9).contains(&n) {
                            return KeyAction::Instance(n);
                        }
                    }
                }
                KeyAction::Unknown
            }
        }
    }

    fn to_wire(self) -> String {
        match self {
            KeyAction::Down => "down".into(),
            KeyAction::Up => "up".into(),
            KeyAction::Long => "long".into(),
            KeyAction::Single => "single".into(),
            KeyAction::Double => "double".into(),
            KeyAction::Chat(n) => format!("chat{n}"),
            KeyAction::Instance(n) => format!("instance{n}"),
            KeyAction::CancelChar => "cancel_char".into(),
            KeyAction::CancelAll => "cancel_all".into(),
            KeyAction::Confirm => "confirm".into(),
            KeyAction::ModelMenu => "model_menu".into(),
            KeyAction::ModelToggle => "model_toggle".into(),
            KeyAction::MenuRecall => "menu_recall".into(),
            KeyAction::Unknown => "unknown".into(),
        }
    }
}

impl<'de> Deserialize<'de> for KeyAction {
    fn deserialize<D: serde::Deserializer<'de>>(d: D) -> Result<KeyAction, D::Error> {
        let s = String::deserialize(d)?;
        Ok(KeyAction::from_wire(&s))
    }
}

impl Serialize for KeyAction {
    fn serialize<S: serde::Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.to_wire())
    }
}

/// Voice trigger state.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum VoiceState {
    Start,
    Stop,
    /// The keyboard gave up waiting for the PC ASR text (K1 or timeout).
    Cancel,
    #[serde(other)]
    Unknown,
}

/// Agent display state rendered on the board LCD.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum DisplayState {
    Idle,
    Running,
    Error,
    #[serde(other)]
    Unknown,
}

/// Which voice engine the companion drives.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum VoiceEngine {
    System,
    ThirdParty,
    #[serde(other)]
    Unknown,
}

// ─────────────────────── Payload structs (board -> PC) ───────────────────────

/// `key` payload: one of the six mechanical switches (`k1`..`k6`) or the EC11
/// push switch (`enc`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct KeyPayload {
    pub id: String,
    /// Raw edge (`down`/`up`/`long` on the voice key) or a gesture
    /// (`single`/`double`/`long`) on every other key and the EC11 push.
    pub action: KeyAction,
    #[serde(default)]
    pub held_ms: u32,
    /// Resolved action the firmware ran for this gesture (`chatN`,
    /// `instanceN`, `cancel_char`, `cancel_all`, `confirm`, or a local action
    /// such as `settings`). Absent when the gesture is unbound.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bind: Option<KeyAction>,
    /// `bind:"confirm"` in input confirm (③) only: the text shown on the
    /// keyboard, which the Companion sends to the selected chat. Never logged.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub text: Option<String>,
}

/// `encoder` payload: EC11 rotary step. `detents` is a signed step count
/// (positive = clockwise, negative = counter-clockwise). Replaces the v0.2
/// `nav` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct EncoderPayload {
    pub detents: i32,
}

/// `voice` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct VoicePayload {
    pub state: VoiceState,
}

/// `macro` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MacroPayload {
    pub name: String,
}

/// `asr` payload: recognized speech text from the board-side (Tuya cloud) ASR.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct AsrPayload {
    pub text: String,
}

/// `voice_source` payload: the board's current voice routing (`cloud`/`system`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct VoiceSourcePayload {
    pub source: String,
    /// Voice mode chosen on the keyboard: `asr` (speech goes to Kiro Crew) or
    /// `chat` (the keyboard talks to the Tuya agent). Read-only here.
    #[serde(default)]
    pub mode: String,
}

/// `tuya_chat` payload: one Chat-mode turn (`role` = `user` / `agent`).
/// Display only: it is never injected into Kiro Crew (CP-01).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct TuyaChatPayload {
    pub role: String,
    #[serde(default)]
    pub text: String,
    #[serde(rename = "final", default)]
    pub is_final: bool,
}

/// `wifi_status` payload: the board's Wi-Fi provisioning/connection state.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WifiStatusPayload {
    pub state: String,
    #[serde(default)]
    pub ssid: String,
}

/// One remembered network in `wifi_list_state` (FR-15). The board never sends
/// a password; there is no field for one.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WifiSavedEntry {
    pub ssid: String,
    #[serde(default)]
    pub sec: String,
    #[serde(default)]
    pub current: bool,
}

/// `wifi_list_state` payload: remembered networks, most recently connected first.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WifiListPayload {
    #[serde(default)]
    pub max: u32,
    #[serde(default)]
    pub entries: Vec<WifiSavedEntry>,
}

/// `activation_status` payload: Tuya pairing/activation lifecycle. The QR URL
/// contains PID+UUID only and is populated solely while waiting for SmartLife.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ActivationStatusPayload {
    pub state: String,
    #[serde(default)]
    pub qr_url: String,
}

/// `prov_status` payload: the keyboard's pairing window. Wi-Fi / License /
/// reset commands are accepted only while `state == "open"`; the user opens
/// the window on the keyboard (Settings -> Pairing mode). `rejected` and
/// `reason` are set when the board refused a provisioning command.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ProvStatusPayload {
    pub state: String,
    #[serde(default)]
    pub remaining_s: u32,
    #[serde(default)]
    pub rejected: String,
    #[serde(default)]
    pub reason: String,
}

/// Local firmware update progress (`ota_status`, UART0 only). `key` is sent
/// with `idle` / `installed`: `release`, `test`, or `none` (placeholder key).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct OtaStatusPayload {
    pub state: String,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub version: String,
    #[serde(default)]
    pub fw: String,
    #[serde(default)]
    pub key: String,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct OtaAckPayload {
    pub next: u64,
}

/// `ble_pair_key` payload (UART0 only): the BLE pairing key, sent once.
/// `Debug` never prints the key.
#[derive(Clone, PartialEq, Eq, Deserialize)]
pub struct BlePairKeyPayload {
    pub key: String,
    pub key_id: String,
}

impl std::fmt::Debug for BlePairKeyPayload {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("BlePairKeyPayload").field("key", &"<redacted>").field("key_id", &self.key_id).finish()
    }
}

/// `license_status` payload: device-license provisioning result.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LicenseStatusPayload {
    pub state: String,
}

/// `license_info` payload: full UUID plus only the last four AuthKey chars.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LicenseInfoPayload {
    pub source: String,
    #[serde(default)]
    pub uuid: String,
    #[serde(default)]
    pub authkey_tail: String,
}

/// `system_info` payload: consolidated diagnostics emitted on UART0.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct SystemInfoPayload {
    #[serde(default)]
    pub fw: String,
    #[serde(default)]
    pub transport: String,
    #[serde(default)]
    pub wifi_name: String,
    #[serde(default)]
    pub wifi_state: String,
    #[serde(default)]
    pub ble_name: String,
    #[serde(default)]
    pub ble_state: String,
    #[serde(default)]
    pub cloud: String,
    #[serde(default)]
    pub license: String,
}

/// `device_settings_state` payload: the values currently applied by the AK.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceSettingsPayload {
    pub volume: u8,
    pub brightness: u8,
    /// Interface language "zh" / "en" (FR-01); absent from older firmware.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub language: Option<String>,
    /// LED power "off" / "on" / "auto" and the Auto power-off delay (s).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub led_power: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub led_auto_off_s: Option<u32>,
    /// Voice mode "asr" / "chat" (FR-02); absent from older firmware.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub voice_mode: Option<String>,
    /// LCD font size in px (12 / 16); absent from older firmware.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub font_size: Option<u8>,
    /// Status only (FR-02): "present" / "missing", never the License itself.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub license: Option<String>,
    /// Status only (FR-02): whether the keyboard holds a BLE pairing key.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ble_paired: Option<bool>,
}

/// `menu_result` payload: the user's answer to LCD menu `id`. `selected` holds
/// global item indexes (ascending); `dismissed` + `reason` (`esc`/`timeout`)
/// means the menu was closed without a decision. `text` is the answer the
/// user dictated and confirmed on the keyboard (voice item only).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MenuResultPayload {
    pub id: String,
    #[serde(default)]
    pub selected: Vec<u32>,
    #[serde(default)]
    pub dismissed: bool,
    #[serde(default)]
    pub reason: String,
    #[serde(default)]
    pub text: String,
}

/// `bind_session` (board -> PC): the slot picked on the Bind session page.
#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct BindSessionPayload {
    pub slot: u8,
}

// ─────────────────────── Payload structs (PC -> board) ───────────────────────

/// `display` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DisplayPayload {
    pub agent_name: String,
    pub state: DisplayState,
    #[serde(default)]
    pub text: String,
}

/// `led` payload. `state`/`color` are kept as free strings for tolerance.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct LedPayload {
    pub state: String,
    #[serde(default)]
    pub color: String,
}

/// `config` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ConfigPayload {
    pub voice_engine: VoiceEngine,
}

/// `ack` payload.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct AckPayload {
    pub seq: u64,
}

// ───────────────────────── Handshake payloads ─────────────────────────

/// `hello` payload (companion -> board).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct HelloPayload {
    pub client: String,
    pub version: String,
}

/// `hello_ack` payload (board -> companion).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct HelloAckPayload {
    #[serde(default)]
    pub fw: String,
    #[serde(default)]
    pub transport: String,
    #[serde(default)]
    pub capabilities: Vec<String>,
}

// ───────────────────────── Decoded inbound frames ─────────────────────────

/// A decoded frame the companion cares about. Anything else (including
/// PC -> board command echoes) decodes to [`Inbound::Unknown`] and is ignored
/// by the receiver, per the spec.
#[derive(Clone, Debug, PartialEq)]
pub enum Inbound {
    Key { seq: Option<u64>, payload: KeyPayload },
    Encoder { seq: Option<u64>, payload: EncoderPayload },
    Voice { seq: Option<u64>, payload: VoicePayload },
    Macro { seq: Option<u64>, payload: MacroPayload },
    /// Recognized speech text from the board-side (Tuya cloud) ASR.
    Asr { seq: Option<u64>, payload: AsrPayload },
    /// The board's current voice routing, reported on change / on hello.
    VoiceSource { seq: Option<u64>, payload: VoiceSourcePayload },
    /// A Chat-mode turn from the keyboard's Tuya agent conversation. Display
    /// only; the router must never turn it into input (CP-01).
    TuyaChat { seq: Option<u64>, payload: TuyaChatPayload },
    /// The board's Wi-Fi provisioning/connection status.
    WifiStatus { seq: Option<u64>, payload: WifiStatusPayload },
    /// The remembered Wi-Fi networks (FR-15): SSIDs only.
    WifiList { seq: Option<u64>, payload: WifiListPayload },
    /// Tuya pairing, QR scan, activation and cloud-ready lifecycle.
    ActivationStatus { seq: Option<u64>, payload: ActivationStatusPayload },
    /// The board's device-license provisioning result.
    LicenseStatus { seq: Option<u64>, payload: LicenseStatusPayload },
    /// A new BLE pairing key from the keyboard (USB only, sent once).
    BlePairKey { seq: Option<u64>, payload: BlePairKeyPayload },
    /// The keyboard's pairing window (open / closed / confirming).
    ProvStatus { seq: Option<u64>, payload: ProvStatusPayload },
    /// Explicit UART0-only development diagnostic; never carries full AuthKey.
    LicenseInfo { seq: Option<u64>, payload: LicenseInfoPayload },
    /// Consolidated firmware/network/radio diagnostics emitted on UART0.
    SystemInfo { seq: Option<u64>, payload: SystemInfoPayload },
    /// The volume and brightness currently applied by the board.
    DeviceSettings { seq: Option<u64>, payload: DeviceSettingsPayload },
    /// The user's answer to an LCD menu (physical keys on the keyboard).
    MenuResult { seq: Option<u64>, payload: MenuResultPayload },
    /// Settings > Bind session on the keyboard picked a slot (1..3).
    BindSession { seq: Option<u64>, payload: BindSessionPayload },
    /// Local firmware update status / transfer ack (USB only).
    OtaStatus(OtaStatusPayload),
    OtaAck(OtaAckPayload),
    HelloAck(HelloAckPayload),
    Ping,
    Pong,
    /// A syntactically valid frame with a `type` we do not handle.
    Unknown { msg_type: String },
}

/// Decode a single received line into an [`Inbound`] frame.
///
/// Returns `None` when the line should simply be ignored: blank, not a JSON
/// object, invalid JSON, missing `type`, or a *known* type whose payload is
/// malformed. Never panics.
pub fn decode_line(line: &str) -> Option<Inbound> {
    let trimmed = line.trim();
    if trimmed.is_empty() || !trimmed.starts_with('{') {
        return None;
    }
    let value: Value = serde_json::from_str(trimmed).ok()?;
    decode_value(&value)
}

/// Decode an already-parsed JSON value. Shared by [`decode_line`] and tests.
pub fn decode_value(value: &Value) -> Option<Inbound> {
    let msg_type = value.get("type").and_then(Value::as_str)?;
    let seq = value.get("seq").and_then(Value::as_u64);
    // Missing payload -> null; known-type decoders that require fields will
    // simply fail (`ok()` -> None) rather than panic.
    let payload = value.get("payload").cloned().unwrap_or(Value::Null);

    match msg_type {
        "key" => serde_json::from_value::<KeyPayload>(payload)
            .ok()
            .map(|p| Inbound::Key { seq, payload: p }),
        "encoder" => serde_json::from_value::<EncoderPayload>(payload)
            .ok()
            .map(|p| Inbound::Encoder { seq, payload: p }),
        "voice" => serde_json::from_value::<VoicePayload>(payload)
            .ok()
            .map(|p| Inbound::Voice { seq, payload: p }),
        "macro" => serde_json::from_value::<MacroPayload>(payload)
            .ok()
            .map(|p| Inbound::Macro { seq, payload: p }),
        "asr" => serde_json::from_value::<AsrPayload>(payload)
            .ok()
            .map(|p| Inbound::Asr { seq, payload: p }),
        "voice_source" => serde_json::from_value::<VoiceSourcePayload>(payload)
            .ok()
            .map(|p| Inbound::VoiceSource { seq, payload: p }),
        "tuya_chat" => serde_json::from_value::<TuyaChatPayload>(payload)
            .ok()
            .map(|p| Inbound::TuyaChat { seq, payload: p }),
        "wifi_status" => serde_json::from_value::<WifiStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::WifiStatus { seq, payload: p }),
        "wifi_list_state" => serde_json::from_value::<WifiListPayload>(payload)
            .ok()
            .map(|p| Inbound::WifiList { seq, payload: p }),
        "activation_status" => serde_json::from_value::<ActivationStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::ActivationStatus { seq, payload: p }),
        "license_status" => serde_json::from_value::<LicenseStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::LicenseStatus { seq, payload: p }),
        "ble_pair_key" => serde_json::from_value::<BlePairKeyPayload>(payload)
            .ok()
            .map(|p| Inbound::BlePairKey { seq, payload: p }),
        "prov_status" => serde_json::from_value::<ProvStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::ProvStatus { seq, payload: p }),
        "license_info" => serde_json::from_value::<LicenseInfoPayload>(payload)
            .ok()
            .map(|p| Inbound::LicenseInfo { seq, payload: p }),
        "system_info" => serde_json::from_value::<SystemInfoPayload>(payload)
            .ok()
            .map(|p| Inbound::SystemInfo { seq, payload: p }),
        "device_settings_state" => serde_json::from_value::<DeviceSettingsPayload>(payload)
            .ok()
            .map(|p| Inbound::DeviceSettings { seq, payload: p }),
        "menu_result" => serde_json::from_value::<MenuResultPayload>(payload)
            .ok()
            .map(|p| Inbound::MenuResult { seq, payload: p }),
        "bind_session" => serde_json::from_value::<BindSessionPayload>(payload)
            .ok()
            .filter(|p| (1..=3).contains(&p.slot))
            .map(|p| Inbound::BindSession { seq, payload: p }),
        "ota_status" => serde_json::from_value::<OtaStatusPayload>(payload)
            .ok()
            .map(Inbound::OtaStatus),
        "ota_ack" => serde_json::from_value::<OtaAckPayload>(payload)
            .ok()
            .map(Inbound::OtaAck),
        "hello_ack" => serde_json::from_value::<HelloAckPayload>(payload)
            .ok()
            .map(Inbound::HelloAck),
        "ping" => Some(Inbound::Ping),
        "pong" => Some(Inbound::Pong),
        other => Some(Inbound::Unknown {
            msg_type: other.to_string(),
        }),
    }
}

// ───────────────────────── Byte-stream reassembly ─────────────────────────

/// Reassembles raw byte chunks (e.g. from a serial port) into complete
/// newline-delimited lines. A partial trailing line is retained until the
/// terminating `\n` arrives. Mirrors the firmware line accumulator.
#[derive(Debug, Default)]
pub struct LineAccumulator {
    buf: Vec<u8>,
}

impl LineAccumulator {
    pub fn new() -> Self {
        Self::default()
    }

    /// Feed raw bytes; return any complete lines (without the trailing `\n`).
    /// Carriage returns are trimmed so CRLF streams decode cleanly.
    pub fn push(&mut self, bytes: &[u8]) -> Vec<String> {
        let mut out = Vec::new();
        for &b in bytes {
            match b {
                b'\n' => {
                    let line = String::from_utf8_lossy(&self.buf).trim().to_string();
                    self.buf.clear();
                    if !line.is_empty() {
                        out.push(line);
                    }
                }
                _ => {
                    if self.buf.len() < MAX_LINE_BYTES {
                        self.buf.push(b);
                    } else {
                        // Overlong line with no terminator: drop it and resync
                        // on the next newline rather than growing unbounded.
                        self.buf.clear();
                    }
                }
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Encoding ─────────────────────────────────────────────

    #[test]
    fn encode_appends_newline() {
        let line = encode_json_line(&serde_json::json!({"a": 1})).unwrap();
        assert_eq!(*line.last().unwrap(), b'\n');
        assert!(line.starts_with(b"{"));
    }

    #[test]
    fn hello_has_version_and_type() {
        let hello = hello_payload();
        assert_eq!(hello["v"], PROTOCOL_VERSION);
        assert_eq!(hello["type"], "hello");
        assert_eq!(hello["payload"]["pc_asr"], true);
    }

    #[test]
    fn voice_cancel_decodes_and_unknown_states_stay_unknown() {
        let f = decode_line(r#"{"v":1,"seq":4,"type":"voice","payload":{"state":"cancel"}}"#).unwrap();
        assert!(matches!(f, Inbound::Voice { payload: VoicePayload { state: VoiceState::Cancel }, .. }));
        let f = decode_line(r#"{"v":1,"type":"voice","payload":{"state":"later"}}"#).unwrap();
        assert!(matches!(f, Inbound::Voice { payload: VoicePayload { state: VoiceState::Unknown }, .. }));
    }

    #[test]
    fn ping_is_versioned() {
        let ping = ping_payload();
        assert_eq!(ping["v"], PROTOCOL_VERSION);
        assert_eq!(ping["type"], "ping");
    }

    #[test]
    fn pc_to_board_encoders_carry_type_and_version() {
        assert_eq!(pong_payload()["type"], "pong");
        let display = display_payload(2, "planner", "running", "thinking…");
        assert_eq!(display["v"], PROTOCOL_VERSION);
        assert_eq!(display["type"], "display");
        assert_eq!(display["payload"]["chat_slot"], 2);
        assert_eq!(display["payload"]["agent_name"], "planner");
        assert_eq!(display["payload"]["state"], "running");
        assert_eq!(led_payload("on", "green")["payload"]["color"], "green");
        assert_eq!(config_payload("system")["payload"]["voice_engine"], "system");
        let aliases = bindings_payload(&["Plan".into(), "".into()], &["".into(), "Ki-board Mate".into()]);
        assert_eq!(aliases["type"], "bindings");
        assert_eq!(aliases["payload"]["alias_a"], "Plan");
        assert_eq!(aliases["payload"]["alias_b"], "");
        assert_eq!(aliases["payload"]["alias_c"], "");
        assert_eq!(aliases["payload"]["name_a"], "");
        assert_eq!(aliases["payload"]["name_b"], "Ki-board Mate");
        assert_eq!(aliases["payload"]["name_c"], "");
        let long = bindings_payload(&[], &["会话".repeat(20)]);
        assert!(long["payload"]["name_a"].as_str().unwrap().len() <= BIND_NAME_MAX);
        assert!(matches!(
            decode_line(r#"{"v":1,"seq":4,"type":"bind_session","payload":{"slot":2}}"#),
            Some(Inbound::BindSession { seq: Some(4), payload: BindSessionPayload { slot: 2 } })
        ));
        assert!(decode_line(r#"{"v":1,"type":"bind_session","payload":{"slot":4}}"#).is_none());
        let done = bind_done_payload(2, "bound", "");
        assert_eq!(done, serde_json::json!({"v":1,"type":"bind_done","payload":{"slot":2,"result":"bound"}}));
        assert_eq!(bind_done_payload(1, "error", "Crew offline")["payload"]["msg"], "Crew offline");
        let settings = device_settings_payload(70, 90);
        assert_eq!(settings["type"], "device_settings");
        assert_eq!(settings["payload"]["volume"], 70);
        assert_eq!(settings["payload"]["brightness"], 90);
        assert_eq!(ack_payload(42)["payload"]["seq"], 42);
    }

    // ── Decoding: happy path ─────────────────────────────────

    #[test]
    fn decode_key_frame() {
        let frame = decode_line(
            r#"{"v":1,"seq":1,"type":"key","payload":{"id":"k1","action":"down","held_ms":0}}"#,
        )
        .expect("key frame");
        assert_eq!(
            frame,
            Inbound::Key {
                seq: Some(1),
                payload: KeyPayload {
                    id: "k1".into(),
                    action: KeyAction::Down,
                    held_ms: 0,
                    bind: None,
                    text: None,
                },
            }
        );
    }

    #[test]
    fn decode_gesture_frames_with_and_without_bind() {
        let bound = decode_line(
            r#"{"v":1,"seq":4,"type":"key","payload":{"id":"k1","action":"single","bind":"chat1","held_ms":0}}"#,
        );
        match bound {
            Some(Inbound::Key { payload, .. }) => {
                assert_eq!(payload.action, KeyAction::Single);
                assert_eq!(payload.bind, Some(KeyAction::Chat(1)));
            }
            other => panic!("bound: {other:?}"),
        }
        let bare = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k2","action":"long"}}"#);
        match bare {
            Some(Inbound::Key { payload, .. }) => {
                assert_eq!(payload.action, KeyAction::Long);
                assert_eq!(payload.bind, None);
            }
            other => panic!("bare: {other:?}"),
        }
        // v0.2 names are not part of the v0.3 protocol.
        let old = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k1","action":"dclick"}}"#);
        assert!(matches!(old, Some(Inbound::Key { payload, .. }) if payload.action == KeyAction::Unknown));
    }

    #[test]
    fn decode_encoder_and_voice_and_macro() {
        let enc = decode_line(
            r#"{"v":1,"seq":2,"type":"encoder","payload":{"detents":-2}}"#,
        )
        .unwrap();
        assert_eq!(
            enc,
            Inbound::Encoder {
                seq: Some(2),
                payload: EncoderPayload { detents: -2 },
            }
        );

        let voice =
            decode_line(r#"{"v":1,"type":"voice","payload":{"state":"start"}}"#).unwrap();
        assert_eq!(
            voice,
            Inbound::Voice {
                seq: None,
                payload: VoicePayload {
                    state: VoiceState::Start,
                },
            }
        );

        let mac = decode_line(r#"{"v":1,"type":"macro","payload":{"name":"send"}}"#).unwrap();
        assert_eq!(
            mac,
            Inbound::Macro {
                seq: None,
                payload: MacroPayload { name: "send".into() },
            }
        );
    }

    #[test]
    fn decode_hello_ack_and_ping_pong() {
        let ack = decode_line(
            r#"{"v":1,"type":"hello_ack","payload":{"fw":"0.1.0","transport":"uart","capabilities":["uart","voice_state"]}}"#,
        )
        .unwrap();
        assert_eq!(
            ack,
            Inbound::HelloAck(HelloAckPayload {
                fw: "0.1.0".into(),
                transport: "uart".into(),
                capabilities: vec!["uart".into(), "voice_state".into()],
            })
        );
        assert_eq!(decode_line(r#"{"v":1,"type":"ping"}"#).unwrap(), Inbound::Ping);
        assert_eq!(decode_line(r#"{"v":1,"type":"pong"}"#).unwrap(), Inbound::Pong);
    }

    #[test]
    fn held_ms_defaults_to_zero_when_absent() {
        let frame =
            decode_line(r#"{"v":1,"type":"key","payload":{"id":"k3","action":"up"}}"#).unwrap();
        match frame {
            Inbound::Key { payload, .. } => assert_eq!(payload.held_ms, 0),
            other => panic!("expected key, got {other:?}"),
        }
    }

    // ── Decoding: tolerant / bad frames ──────────────────────

    #[test]
    fn decode_asr_and_voice_source() {
        let asr = decode_line(r#"{"v":1,"seq":11,"type":"asr","payload":{"text":"hello world"}}"#)
            .unwrap();
        assert_eq!(
            asr,
            Inbound::Asr {
                seq: Some(11),
                payload: AsrPayload { text: "hello world".into() },
            }
        );
        let vs = decode_line(
            r#"{"v":1,"seq":12,"type":"voice_source","payload":{"source":"cloud"}}"#,
        )
        .unwrap();
        assert_eq!(
            vs,
            Inbound::VoiceSource {
                seq: Some(12),
                payload: VoiceSourcePayload { source: "cloud".into(), mode: String::new() },
            }
        );
        let vs = decode_line(
            r#"{"v":1,"seq":13,"type":"voice_source","payload":{"source":"cloud","mode":"chat"}}"#,
        )
        .unwrap();
        match vs {
            Inbound::VoiceSource { payload, .. } => assert_eq!(payload.mode, "chat"),
            other => panic!("unexpected {other:?}"),
        }
        let chat = decode_line(
            r#"{"v":1,"seq":30,"type":"tuya_chat","payload":{"role":"agent","text":"Split the parser first.","final":true}}"#,
        )
        .unwrap();
        assert_eq!(
            chat,
            Inbound::TuyaChat {
                seq: Some(30),
                payload: TuyaChatPayload {
                    role: "agent".into(),
                    text: "Split the parser first.".into(),
                    is_final: true,
                },
            }
        );
    }

    #[test]
    fn wifi_payload_and_status_roundtrip() {
        let p = wifi_payload("MyAP", "secret");
        assert_eq!(p["type"], "wifi");
        assert_eq!(p["payload"]["ssid"], "MyAP");
        assert_eq!(p["payload"]["pass"], "secret");

        let ws = decode_line(
            r#"{"v":1,"seq":13,"type":"wifi_status","payload":{"state":"connected","ssid":"MyAP"}}"#,
        )
        .unwrap();
        assert_eq!(
            ws,
            Inbound::WifiStatus {
                seq: Some(13),
                payload: WifiStatusPayload { state: "connected".into(), ssid: "MyAP".into() },
            }
        );
    }

    #[test]
    fn wifi_list_frames() {
        assert_eq!(wifi_list_payload()["type"], "wifi_list");
        let f = wifi_forget_payload("Lab AP");
        assert_eq!(f["type"], "wifi_forget");
        assert_eq!(f["payload"]["ssid"], "Lab AP");
        assert_eq!(wifi_clear_payload()["type"], "wifi_clear");
        // No password field exists in either direction of the list.
        assert!(f.to_string().find("pass").is_none());

        let list = decode_line(
            r#"{"v":1,"seq":4,"type":"wifi_list_state","payload":{"max":10,"entries":[{"ssid":"Home","sec":"wpa2","current":true},{"ssid":"Cafe","sec":"unknown","current":false}]}}"#,
        )
        .unwrap();
        match list {
            Inbound::WifiList { seq, payload } => {
                assert_eq!(seq, Some(4));
                assert_eq!(payload.max, 10);
                assert_eq!(payload.entries.len(), 2);
                assert!(payload.entries[0].current);
                assert_eq!(payload.entries[1].ssid, "Cafe");
            }
            other => panic!("unexpected {other:?}"),
        }
        let empty = decode_line(r#"{"v":1,"type":"wifi_list_state","payload":{"max":10,"entries":[]}}"#).unwrap();
        assert!(matches!(empty, Inbound::WifiList { ref payload, .. } if payload.entries.is_empty()));
    }

    #[test]
    fn decode_activation_status_with_qr_url() {
        let frame = decode_line(
            r#"{"v":1,"seq":20,"type":"activation_status","payload":{"state":"scan_qr","qr_url":"https://smartapp.tuya.com/s/p?p=pid&uuid=uuid123&v=2.0"}}"#,
        )
        .unwrap();
        assert_eq!(
            frame,
            Inbound::ActivationStatus {
                seq: Some(20),
                payload: ActivationStatusPayload {
                    state: "scan_qr".into(),
                    qr_url: "https://smartapp.tuya.com/s/p?p=pid&uuid=uuid123&v=2.0".into(),
                },
            }
        );
    }

    #[test]
    fn ble_pair_key_decodes_and_debug_redacts_the_key() {
        let f = decode_line(
            r#"{"v":1,"seq":18,"type":"ble_pair_key","payload":{"key":"00ff","key_id":"02d449a3"}}"#,
        )
        .unwrap();
        match &f {
            Inbound::BlePairKey { payload, .. } => {
                assert_eq!(payload.key, "00ff");
                assert_eq!(payload.key_id, "02d449a3");
            }
            other => panic!("unexpected {other:?}"),
        }
        assert!(!format!("{f:?}").contains("00ff"));
        assert_eq!(ble_pair_payload()["type"], "ble_pair");
    }

    #[test]
    fn prov_status_decodes_open_and_rejection() {
        let open = decode_line(
            r#"{"v":1,"seq":16,"type":"prov_status","payload":{"state":"open","remaining_s":172}}"#,
        )
        .unwrap();
        assert_eq!(
            open,
            Inbound::ProvStatus {
                seq: Some(16),
                payload: ProvStatusPayload {
                    state: "open".into(),
                    remaining_s: 172,
                    rejected: String::new(),
                    reason: String::new(),
                },
            }
        );
        let refused = decode_line(
            r#"{"v":1,"seq":17,"type":"prov_status","payload":{"state":"closed","remaining_s":0,"rejected":"wifi","reason":"closed"}}"#,
        )
        .unwrap();
        match refused {
            Inbound::ProvStatus { payload, .. } => {
                assert_eq!(payload.state, "closed");
                assert_eq!(payload.rejected, "wifi");
                assert_eq!(payload.reason, "closed");
            }
            other => panic!("unexpected {other:?}"),
        }
    }

    #[test]
    fn license_payload_and_status_roundtrip() {
        let p = license_payload("uuid123", "authkey456");
        assert_eq!(p["type"], "license");
        assert_eq!(p["payload"]["uuid"], "uuid123");
        assert_eq!(p["payload"]["authkey"], "authkey456");

        let ls = decode_line(
            r#"{"v":1,"seq":14,"type":"license_status","payload":{"state":"written"}}"#,
        )
        .unwrap();
        assert_eq!(
            ls,
            Inbound::LicenseStatus {
                seq: Some(14),
                payload: LicenseStatusPayload { state: "written".into() },
            }
        );

        let read = license_read_payload();
        assert_eq!(read["type"], "license_read");

        let info = decode_line(
            r#"{"v":1,"seq":15,"type":"license_info","payload":{"source":"flashed","uuid":"uuid123","authkey_tail":"CDEF"}}"#,
        )
        .unwrap();
        assert_eq!(
            info,
            Inbound::LicenseInfo {
                seq: Some(15),
                payload: LicenseInfoPayload {
                    source: "flashed".into(),
                    uuid: "uuid123".into(),
                    authkey_tail: "CDEF".into(),
                },
            }
        );
    }

    #[test]
    fn system_info_decodes_all_feedback_fields() {
        let info = decode_line(
            r#"{"v":1,"seq":15,"type":"system_info","payload":{"fw":"0.1.1","transport":"dual","wifi_name":"MyAP","wifi_state":"connected","ble_name":"Agentic Keyboard-A1B2C3","ble_state":"advertising","cloud":"ready","license":"present"}}"#,
        )
        .unwrap();
        assert_eq!(
            info,
            Inbound::SystemInfo {
                seq: Some(15),
                payload: SystemInfoPayload {
                    fw: "0.1.1".into(),
                    transport: "dual".into(),
                    wifi_name: "MyAP".into(),
                    wifi_state: "connected".into(),
                    ble_name: "Agentic Keyboard-A1B2C3".into(),
                    ble_state: "advertising".into(),
                    cloud: "ready".into(),
                    license: "present".into(),
                },
            }
        );
    }

    #[test]
    fn device_settings_state_with_led_power_still_parses() {
        // 0.3.52+ firmware adds led_power / led_auto_off_s.
        let state = decode_line(
            r#"{"v":1,"seq":16,"type":"device_settings_state","payload":{"volume":60,"brightness":80,"led_power":"auto","led_auto_off_s":60}}"#,
        )
        .unwrap();
        assert_eq!(
            state,
            Inbound::DeviceSettings {
                seq: Some(16),
                payload: DeviceSettingsPayload {
                    volume: 60,
                    brightness: 80,
                    led_power: Some("auto".into()),
                    led_auto_off_s: Some(60),
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn device_settings_fr02_fields_parse_and_build() {
        let state = decode_line(
            r#"{"v":1,"seq":18,"type":"device_settings_state","payload":{"volume":60,"brightness":80,"led_power":"off","led_auto_off_s":900,"language":"en","voice_mode":"chat","license":"present","ble_paired":true}}"#,
        )
        .unwrap();
        match state {
            Inbound::DeviceSettings { payload, .. } => {
                assert_eq!(payload.voice_mode.as_deref(), Some("chat"));
                assert_eq!(payload.license.as_deref(), Some("present"));
                assert_eq!(payload.ble_paired, Some(true));
                assert_eq!(payload.led_auto_off_s, Some(900));
            }
            other => panic!("{other:?}"),
        }
        let down = device_settings_update_payload(&DeviceSettingsUpdate {
            voice_mode: Some("asr".into()),
            led_power: Some("on".into()),
            ..Default::default()
        });
        assert_eq!(down["type"], "device_settings");
        assert_eq!(down["payload"], serde_json::json!({ "voice_mode": "asr", "led_power": "on" }));
        let font = decode_line(
            r#"{"v":1,"seq":19,"type":"device_settings_state","payload":{"volume":60,"brightness":80,"font_size":16}}"#,
        )
        .unwrap();
        match font {
            Inbound::DeviceSettings { payload, .. } => assert_eq!(payload.font_size, Some(16)),
            other => panic!("{other:?}"),
        }
        let down = device_settings_update_payload(&DeviceSettingsUpdate { font_size: Some(12), ..Default::default() });
        assert_eq!(down["payload"], serde_json::json!({ "font_size": 12 }));
        assert_eq!(prov_open_payload()["type"], "prov_open");
        assert_eq!(ble_unpair_payload()["type"], "ble_unpair");
    }

    #[test]
    fn device_settings_update_is_validated() {
        let ok = DeviceSettingsUpdate {
            volume: Some(100),
            brightness: Some(10),
            language: Some("zh".into()),
            voice_mode: Some("chat".into()),
            led_power: Some("auto".into()),
            led_auto_off_s: Some(300),
            font_size: Some(16),
        };
        assert_eq!(ok.validate(), Ok(()));
        for bad in [
            DeviceSettingsUpdate { volume: Some(101), ..Default::default() },
            DeviceSettingsUpdate { brightness: Some(5), ..Default::default() },
            DeviceSettingsUpdate { language: Some("fr".into()), ..Default::default() },
            DeviceSettingsUpdate { voice_mode: Some("yolo".into()), ..Default::default() },
            DeviceSettingsUpdate { led_power: Some("Auto".into()), ..Default::default() },
            DeviceSettingsUpdate { led_auto_off_s: Some(45), ..Default::default() },
            DeviceSettingsUpdate { font_size: Some(14), ..Default::default() },
            DeviceSettingsUpdate::default(),
        ] {
            assert!(bad.validate().is_err(), "{bad:?}");
        }
    }

    #[test]
    fn device_settings_language_round_trips() {
        let state = decode_line(
            r#"{"v":1,"seq":17,"type":"device_settings_state","payload":{"volume":60,"brightness":80,"led_power":"auto","led_auto_off_s":60,"language":"zh"}}"#,
        )
        .unwrap();
        assert_eq!(
            state,
            Inbound::DeviceSettings {
                seq: Some(17),
                payload: DeviceSettingsPayload {
                    volume: 60,
                    brightness: 80,
                    language: Some("zh".into()),
                    led_power: Some("auto".into()),
                    led_auto_off_s: Some(60),
                    ..Default::default()
                },
            }
        );
        let down = device_language_payload("en");
        assert_eq!(down["type"], "device_settings");
        assert_eq!(down["payload"], serde_json::json!({ "language": "en" }));
    }

    #[test]
    fn device_settings_payload_and_state_roundtrip() {
        let state = decode_line(
            r#"{"v":1,"seq":15,"type":"device_settings_state","payload":{"volume":60,"brightness":80}}"#,
        )
        .unwrap();
        assert_eq!(
            state,
            Inbound::DeviceSettings {
                seq: Some(15),
                payload: DeviceSettingsPayload {
                    volume: 60,
                    brightness: 80,
                    language: None,
                    ..Default::default()
                },
            }
        );
    }

    #[test]
    fn key_action_chat_instance_wire_round_trips() {
        // Firmware puts the resolved target on the wire; the Companion decodes
        // chatN/instanceN into KeyAction::Chat(n)/Instance(n), N 1..=9.
        let f = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k1","action":"chat5"}}"#).unwrap();
        match f {
            Inbound::Key { payload, .. } => assert_eq!(payload.action, KeyAction::Chat(5)),
            _ => panic!("expected key"),
        }
        let g = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k2","action":"instance3"}}"#).unwrap();
        match g {
            Inbound::Key { payload, .. } => assert_eq!(payload.action, KeyAction::Instance(3)),
            _ => panic!("expected key"),
        }
        // Out-of-range / multi-digit decode to Unknown (tolerant), no panic.
        for bad in ["chat0", "chat10", "instance0", "chat"] {
            let line = format!(r#"{{"v":1,"type":"key","payload":{{"id":"k1","action":"{bad}"}}}}"#);
            match decode_line(&line).unwrap() {
                Inbound::Key { payload, .. } => assert_eq!(payload.action, KeyAction::Unknown, "{bad}"),
                _ => panic!("expected key"),
            }
        }
    }

    #[test]
    fn keymap_payload_emits_dotted_bindings_and_omits_empty() {
        let bindings = vec![
            ("k1.single".to_string(), "chat1".to_string()),
            ("k1.double".to_string(), "instance1".to_string()),
            ("k5.kind".to_string(), "voice".to_string()),
            ("k4.single".to_string(), "cancel_char".to_string()),
            ("k6.single".to_string(), "confirm".to_string()),
            ("k1.long".to_string(), "".to_string()), // empty -> omitted
        ];
        let v = keymap_payload(&bindings);
        assert_eq!(v["type"], "keymap");
        assert_eq!(v["payload"]["k1.single"], "chat1");
        assert_eq!(v["payload"]["k1.double"], "instance1");
        assert_eq!(v["payload"]["k5.kind"], "voice");
        assert_eq!(v["payload"]["k4.single"], "cancel_char");
        assert_eq!(v["payload"]["k6.single"], "confirm");
        assert!(v["payload"].get("k1.long").is_none(), "empty binding omitted");
    }

    #[test]
    fn unknown_type_is_surfaced_not_dropped() {
        let frame = decode_line(r#"{"v":1,"type":"future_thing","payload":{"x":1}}"#).unwrap();
        assert_eq!(
            frame,
            Inbound::Unknown {
                msg_type: "future_thing".into(),
            }
        );
    }

    #[test]
    fn unknown_enum_values_decode_to_unknown_variant() {
        let frame =
            decode_line(r#"{"v":1,"type":"key","payload":{"id":"k1","action":"triple"}}"#).unwrap();
        match frame {
            Inbound::Key { payload, .. } => assert_eq!(payload.action, KeyAction::Unknown),
            other => panic!("expected key, got {other:?}"),
        }
    }

    #[test]
    fn bad_frames_never_panic_and_are_ignored() {
        // Blank / whitespace.
        assert_eq!(decode_line(""), None);
        assert_eq!(decode_line("   \r\n"), None);
        // Not a JSON object.
        assert_eq!(decode_line("garbage"), None);
        assert_eq!(decode_line("[1,2,3]"), None);
        // Truncated / partial JSON.
        assert_eq!(decode_line(r#"{"v":1,"type":"key""#), None);
        // Missing type.
        assert_eq!(decode_line(r#"{"v":1,"payload":{}}"#), None);
        // Known type, malformed payload (missing required `id`).
        assert_eq!(
            decode_line(r#"{"v":1,"type":"key","payload":{"action":"down"}}"#),
            None
        );
        // Known type, wrong payload shape entirely.
        assert_eq!(decode_line(r#"{"v":1,"type":"encoder","payload":42}"#), None);
    }

    // ── Byte-stream reassembly ───────────────────────────────

    #[test]
    fn line_accumulator_reassembles_split_chunks() {
        let mut acc = LineAccumulator::new();
        // A frame split across two reads, then a whole frame, then a partial
        // trailing frame with no terminator yet.
        let mut frames = Vec::new();
        frames.extend(acc.push(b"{\"v\":1,\"type\":\"key\",\"pay"));
        assert!(frames.is_empty(), "no complete line yet");
        frames.extend(acc.push(b"load\":{\"id\":\"k1\",\"action\":\"down\"}}\n"));
        frames.extend(acc.push(b"{\"v\":1,\"type\":\"pong\"}\r\n{\"v\":1,\"type\":\"vo"));
        // Two complete lines delivered so far; "vo…" is still buffered.
        assert_eq!(frames.len(), 2);

        let decoded: Vec<Inbound> = frames.iter().filter_map(|l| decode_line(l)).collect();
        assert!(matches!(decoded[0], Inbound::Key { .. }));
        assert_eq!(decoded[1], Inbound::Pong);

        // Finish the trailing frame.
        let tail = acc.push(b"ice\",\"payload\":{\"state\":\"stop\"}}\n");
        assert_eq!(tail.len(), 1);
        assert_eq!(
            decode_line(&tail[0]).unwrap(),
            Inbound::Voice {
                seq: None,
                payload: VoicePayload {
                    state: VoiceState::Stop,
                },
            }
        );
    }

    #[test]
    fn line_accumulator_drops_overlong_unterminated_line() {
        let mut acc = LineAccumulator::new();
        let junk = vec![b'x'; MAX_LINE_BYTES + 512];
        assert!(acc.push(&junk).is_empty(), "no newline -> no lines yet");
        // After the overlong run, a fresh terminated frame is still recoverable.
        // The leftover junk tail may surface as one line, but it is non-JSON and
        // decodes to None, so exactly one valid frame comes through.
        let out = acc.push(b"\n{\"v\":1,\"type\":\"ping\"}\n");
        let decoded: Vec<Inbound> = out.iter().filter_map(|l| decode_line(l)).collect();
        assert_eq!(decoded, vec![Inbound::Ping]);
    }

    #[test]
    fn decode_menu_result_and_dictated_text() {
        let sel = decode_line(r#"{"v":1,"seq":51,"type":"menu_result","payload":{"id":"a1b2c3","selected":[0,2]}}"#);
        assert_eq!(
            sel,
            Some(Inbound::MenuResult {
                seq: Some(51),
                payload: MenuResultPayload {
                    id: "a1b2c3".into(),
                    selected: vec![0, 2],
                    dismissed: false,
                    reason: String::new(),
                    text: String::new(),
                },
            })
        );
        let esc = decode_line(r#"{"v":1,"seq":53,"type":"menu_result","payload":{"id":"a1","dismissed":true,"reason":"esc"}}"#);
        assert!(matches!(esc, Some(Inbound::MenuResult { payload, .. }) if payload.dismissed && payload.reason == "esc" && payload.selected.is_empty()));
        // Missing id or a negative index is malformed -> ignored.
        assert_eq!(decode_line(r#"{"v":1,"type":"menu_result","payload":{"selected":[0]}}"#), None);
        assert_eq!(decode_line(r#"{"v":1,"type":"menu_result","payload":{"id":"a","selected":[-1]}}"#), None);
        let v = decode_line(r#"{"v":1,"seq":54,"type":"menu_result","payload":{"id":"q9","selected":[3],"text":"use main"}}"#);
        assert!(matches!(v, Some(Inbound::MenuResult { payload, .. }) if payload.text == "use main" && payload.selected == vec![3]));
        // The retired menu_voice frame is an unknown type now.
        assert!(matches!(
            decode_line(r#"{"v":1,"type":"menu_voice","payload":{"id":"q9","state":"start"}}"#),
            Some(Inbound::Unknown { .. })
        ));
        let r = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k4","action":"single","bind":"menu_recall"}}"#);
        assert!(matches!(r, Some(Inbound::Key { payload, .. }) if payload.bind == Some(KeyAction::MenuRecall)));
        assert_eq!(serde_json::to_value(KeyAction::MenuRecall).unwrap(), "menu_recall");
    }

    #[test]
    fn model_actions_round_trip() {
        let f = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k6","action":"long","bind":"model_menu"}}"#);
        assert!(matches!(f, Some(Inbound::Key { payload, .. }) if payload.bind == Some(KeyAction::ModelMenu)));
        let f = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k6","action":"double","bind":"model_toggle"}}"#);
        assert!(matches!(f, Some(Inbound::Key { payload, .. }) if payload.bind == Some(KeyAction::ModelToggle)));
        assert_eq!(serde_json::to_value(KeyAction::ModelMenu).unwrap(), "model_menu");
        assert_eq!(serde_json::to_value(KeyAction::ModelToggle).unwrap(), "model_toggle");
    }
}
