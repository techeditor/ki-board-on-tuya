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
        "payload": { "client": "companion", "version": env!("CARGO_PKG_VERSION") }
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

/// PC -> board: synchronize optional display aliases for slots A, B, and C.
/// Empty strings tell the AK to render the corresponding default letter.
pub fn binding_aliases_payload(aliases: &[String]) -> Value {
    let alias = |index: usize| aliases.get(index).map(String::as_str).unwrap_or("");
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "bindings",
        "payload": {
            "alias_a": alias(0),
            "alias_b": alias(1),
            "alias_c": alias(2)
        }
    })
}

/// PC -> board: set the current device volume and screen brightness.
pub fn device_settings_payload(volume: u8, brightness: u8) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "device_settings",
        "payload": { "volume": volume, "brightness": brightness }
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

/// PC -> board: acknowledge a board event by its sequence number.
pub fn ack_payload(seq: u64) -> Value {
    serde_json::json!({
        "v": PROTOCOL_VERSION,
        "type": "ack",
        "payload": { "seq": seq }
    })
}

// ───────────────────────────── Enums ─────────────────────────────

/// `key` / `nav` action. `#[serde(other)]` keeps decode tolerant to values the
/// firmware may add later.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum KeyAction {
    Down,
    Up,
    Long,
    #[serde(other)]
    Unknown,
}

/// 5-way navigation direction.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NavDir {
    Up,
    Down,
    Left,
    Right,
    Center,
    #[serde(other)]
    Unknown,
}

/// Voice trigger state.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum VoiceState {
    Start,
    Stop,
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

/// `key` payload: one of the five mechanical switches (`k1`..`k5`).
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct KeyPayload {
    pub id: String,
    pub action: KeyAction,
    #[serde(default)]
    pub held_ms: u32,
}

/// `nav` payload: the 5-way joystick.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct NavPayload {
    pub dir: NavDir,
    pub action: KeyAction,
    #[serde(default)]
    pub held_ms: u32,
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
}

/// `wifi_status` payload: the board's Wi-Fi provisioning/connection state.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WifiStatusPayload {
    pub state: String,
    #[serde(default)]
    pub ssid: String,
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
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceSettingsPayload {
    pub volume: u8,
    pub brightness: u8,
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
    Nav { seq: Option<u64>, payload: NavPayload },
    Voice { seq: Option<u64>, payload: VoicePayload },
    Macro { seq: Option<u64>, payload: MacroPayload },
    /// Recognized speech text from the board-side (Tuya cloud) ASR.
    Asr { seq: Option<u64>, payload: AsrPayload },
    /// The board's current voice routing, reported on change / on hello.
    VoiceSource { seq: Option<u64>, payload: VoiceSourcePayload },
    /// The board's Wi-Fi provisioning/connection status.
    WifiStatus { seq: Option<u64>, payload: WifiStatusPayload },
    /// The board's device-license provisioning result.
    LicenseStatus { seq: Option<u64>, payload: LicenseStatusPayload },
    /// Explicit UART0-only development diagnostic; never carries full AuthKey.
    LicenseInfo { seq: Option<u64>, payload: LicenseInfoPayload },
    /// Consolidated firmware/network/radio diagnostics emitted on UART0.
    SystemInfo { seq: Option<u64>, payload: SystemInfoPayload },
    /// The volume and brightness currently applied by the board.
    DeviceSettings { seq: Option<u64>, payload: DeviceSettingsPayload },
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
        "nav" => serde_json::from_value::<NavPayload>(payload)
            .ok()
            .map(|p| Inbound::Nav { seq, payload: p }),
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
        "wifi_status" => serde_json::from_value::<WifiStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::WifiStatus { seq, payload: p }),
        "license_status" => serde_json::from_value::<LicenseStatusPayload>(payload)
            .ok()
            .map(|p| Inbound::LicenseStatus { seq, payload: p }),
        "license_info" => serde_json::from_value::<LicenseInfoPayload>(payload)
            .ok()
            .map(|p| Inbound::LicenseInfo { seq, payload: p }),
        "system_info" => serde_json::from_value::<SystemInfoPayload>(payload)
            .ok()
            .map(|p| Inbound::SystemInfo { seq, payload: p }),
        "device_settings_state" => serde_json::from_value::<DeviceSettingsPayload>(payload)
            .ok()
            .map(|p| Inbound::DeviceSettings { seq, payload: p }),
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
        let aliases = binding_aliases_payload(&["Plan".into(), "".into()]);
        assert_eq!(aliases["type"], "bindings");
        assert_eq!(aliases["payload"]["alias_a"], "Plan");
        assert_eq!(aliases["payload"]["alias_b"], "");
        assert_eq!(aliases["payload"]["alias_c"], "");
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
                },
            }
        );
    }

    #[test]
    fn decode_nav_and_voice_and_macro() {
        let nav = decode_line(
            r#"{"v":1,"seq":2,"type":"nav","payload":{"dir":"up","action":"long","held_ms":900}}"#,
        )
        .unwrap();
        assert_eq!(
            nav,
            Inbound::Nav {
                seq: Some(2),
                payload: NavPayload {
                    dir: NavDir::Up,
                    action: KeyAction::Long,
                    held_ms: 900,
                },
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
                payload: VoiceSourcePayload { source: "cloud".into() },
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
                },
            }
        );
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
        assert_eq!(decode_line(r#"{"v":1,"type":"nav","payload":42}"#), None);
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
}
