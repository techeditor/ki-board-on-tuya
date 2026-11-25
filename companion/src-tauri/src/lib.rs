//! Companion app library entry.
//!
//! Layering:
//! * [`protocol`] — pure v1 wire codec (no IO, unit-tested on any host).
//! * [`transport`] — `serialport`-based UART backend + a stable transport trait.
//! * [`inject`] / [`asr`] — Windows-only key injection and system speech input,
//!   each a trait with a real `#[cfg(windows)]` impl and a non-Windows stub.
//! * [`status_sink`] — FP-4b STATUS ("the eyes"): a pure `map_hook_event` (T5)
//!   plus a cross-platform `tiny_http` localhost sink (T6) that ingests Kiro Crew
//!   agent-hook events (delivered by this same binary's `--hook` mode) and a
//!   `{port, token}` handoff file the `--hook` mode reads. Only the callback
//!   that emits a `display` frame is `#[cfg(windows)]` (in the app module).
//! * [`describe_inbound`] / [`inbound_to_json`] — pure frame-routing helpers,
//!   shared by the Tauri reader thread and testable on Linux.
//! * The Tauri application itself (`app` module + [`run`]) is `#[cfg(windows)]`
//!   because Tauri needs WebKitGTK, which is absent on the Linux CI host.

pub mod app_dirs;
pub mod asr;
pub mod diagnostics;
pub mod inject;
pub mod kirocrew_control;
pub mod menu_wire;
pub mod bind_flow;
pub mod bind_store;
pub mod crew_api;
pub mod crew_identity;
pub mod crew_menus;
pub mod handshake;
pub mod lcd_text;
pub mod hook_install;
pub mod pc_asr;
pub mod protocol;
pub mod scene;
pub mod scene_store;
pub mod scene_sync;
pub mod secrets;
pub mod stream_asr;
pub mod ble_sec;
pub mod ble_pair;
pub mod ble_pairing;
pub mod connection_prefs;
pub mod ota;
pub mod status_sink;
pub mod transport;
pub mod voice_send;

use protocol::Inbound;

/// Render a decoded frame as a compact human-readable log line.
pub fn describe_inbound(frame: &Inbound) -> String {
    match frame {
        Inbound::Key { seq, payload } => format!(
            "key {} {:?} ({}ms){}",
            payload.id,
            payload.action,
            payload.held_ms,
            seq_suffix(*seq)
        ),
        Inbound::Encoder { seq, payload } => format!(
            "encoder {:+}{}",
            payload.detents,
            seq_suffix(*seq)
        ),
        Inbound::Voice { seq, payload } => {
            format!("voice {:?}{}", payload.state, seq_suffix(*seq))
        }
        Inbound::Macro { seq, payload } => {
            format!("macro {}{}", payload.name, seq_suffix(*seq))
        }
        Inbound::Asr { seq, payload } => {
            format!("asr {:?}{}", payload.text, seq_suffix(*seq))
        }
        Inbound::VoiceSource { seq, payload } => {
            format!("voice_source {}{}", payload.source, seq_suffix(*seq))
        }
        Inbound::TuyaChat { seq, payload } => {
            format!("tuya_chat {} {}B{}", payload.role, payload.text.len(), seq_suffix(*seq))
        }
        Inbound::BlePairKey { seq, payload } => {
            format!("ble_pair_key id={}{}", payload.key_id, seq_suffix(*seq))
        }
        Inbound::WifiStatus { seq, payload } => {
            format!("wifi_status {} {}{}", payload.state, payload.ssid, seq_suffix(*seq))
        }
        Inbound::WifiList { seq, payload } => {
            format!("wifi_list_state {} saved{}", payload.entries.len(), seq_suffix(*seq))
        }
        Inbound::ActivationStatus { seq, payload } => format!(
            "activation_status {} qr={}{}",
            payload.state,
            if payload.qr_url.is_empty() { "no" } else { "yes" },
            seq_suffix(*seq)
        ),
        Inbound::LicenseStatus { seq, payload } => {
            format!("license_status {}{}", payload.state, seq_suffix(*seq))
        }
        Inbound::PidStatus { seq, payload } => {
            format!("pid_status {} {}{}", payload.state, payload.reason, seq_suffix(*seq))
        }
        Inbound::ProvStatus { seq, payload } => format!(
            "prov_status {} {}s{}{}",
            payload.state,
            payload.remaining_s,
            if payload.rejected.is_empty() {
                String::new()
            } else {
                format!(" rejected={} reason={}", payload.rejected, payload.reason)
            },
            seq_suffix(*seq)
        ),
        Inbound::LicenseInfo { seq, payload } => format!(
            "license_info source={} uuid={} authkey=****{}{}",
            payload.source,
            payload.uuid,
            payload.authkey_tail,
            seq_suffix(*seq)
        ),
        Inbound::SystemInfo { seq, payload } => format!(
            "system_info fw={} transport={} wifi={}({}) ble={}({}) cloud={} license={}{}",
            payload.fw,
            payload.transport,
            payload.wifi_name,
            payload.wifi_state,
            payload.ble_name,
            payload.ble_state,
            payload.cloud,
            payload.license,
            seq_suffix(*seq)
        ),
        Inbound::DeviceInfo { seq, payload } => format!(
            // No SSID, IP or MAC in the log line: it is shown in the Logs view and copied into reports.
            "device_info fw={} companion={} handshake={} cloud={} wifi={} ble_paired={}{}",
            payload.fw,
            payload.companion,
            flag(payload.handshake),
            payload.cloud,
            if payload.wifi_ssid.is_empty() { "off" } else { "on" },
            flag(payload.ble_paired),
            seq_suffix(*seq)
        ),
        Inbound::DeviceSettings { seq, payload } => format!(
            "device_settings volume={} brightness={}{}",
            payload.volume,
            payload.brightness,
            seq_suffix(*seq)
        ),
        Inbound::MenuResult { seq, payload } => format!(
            "menu_result id={} selected={:?}{}{}",
            payload.id,
            payload.selected,
            if payload.dismissed { format!(" dismissed={}", payload.reason) } else { String::new() },
            seq_suffix(*seq)
        ),
        Inbound::BindSession { seq, payload } => format!("bind_session slot={}{}", payload.slot, seq_suffix(*seq)),
        Inbound::SceneState { seq, payload } => format!(
            "scene_state active={}{}{}{}",
            payload.active,
            if payload.dirty { " dirty" } else { "" },
            payload.error.as_deref().map(|e| format!(" error={e}")).unwrap_or_default(),
            seq_suffix(*seq)
        ),
        Inbound::SceneData { seq, payload } => format!("scene_data slot={} id={}{}", payload.slot, payload.id, seq_suffix(*seq)),
        Inbound::OtaStatus(p) => format!("ota_status {} {} v={} fw={}", p.state, p.reason, p.version, p.fw),
        Inbound::OtaAck(p) => format!("ota_ack next={}", p.next),
        Inbound::HelloAck(ack) => format!(
            "hello_ack fw={} transport={} caps=[{}]",
            ack.fw,
            ack.transport,
            ack.capabilities.join(",")
        ),
        Inbound::Ping => "ping".to_string(),
        Inbound::Pong => "pong".to_string(),
        Inbound::Unknown { msg_type } => format!("unknown type={msg_type}"),
    }
}

fn seq_suffix(seq: Option<u64>) -> String {
    match seq {
        Some(n) => format!(" seq={n}"),
        None => String::new(),
    }
}

/// A flag a frame may leave out: "yes" / "no" / "?".
fn flag(value: Option<bool>) -> &'static str {
    match value {
        Some(true) => "yes",
        Some(false) => "no",
        None => "?",
    }
}

#[cfg(any(test, target_os = "windows"))]
fn ensure_usb_provisioning_transport(
    connected: bool,
    kind: transport::TransportKind,
) -> Result<(), String> {
    if connected && kind == transport::TransportKind::Usb {
        Ok(())
    } else {
        Err("WiFi and UUID/AuthKey settings require an active USB connection".to_string())
    }
}

/// Held for the whole of one `connect_device` call (CP-05). A second connect
/// that starts while one is in flight is refused instead of opening the same
/// port or BLE device twice. Released on drop, including early returns.
#[cfg(any(test, target_os = "windows"))]
struct ConnectClaim<'a>(&'a std::sync::atomic::AtomicBool);

#[cfg(any(test, target_os = "windows"))]
impl<'a> ConnectClaim<'a> {
    fn try_claim(flag: &'a std::sync::atomic::AtomicBool) -> Option<Self> {
        use std::sync::atomic::Ordering;
        flag.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| ConnectClaim(flag))
    }
}

#[cfg(any(test, target_os = "windows"))]
impl Drop for ConnectClaim<'_> {
    fn drop(&mut self) {
        self.0.store(false, std::sync::atomic::Ordering::Release);
    }
}

/// Convert a decoded frame into a flat JSON object for the UI event stream.
pub fn inbound_to_json(frame: &Inbound) -> serde_json::Value {
    use serde_json::json;
    match frame {
        Inbound::Key { seq, payload } => json!({
            "kind": "key", "seq": seq, "id": payload.id,
            "action": payload.action, "held_ms": payload.held_ms, "bind": payload.bind
        }),
        Inbound::Encoder { seq, payload } => json!({
            "kind": "encoder", "seq": seq, "detents": payload.detents
        }),
        Inbound::Voice { seq, payload } => json!({
            "kind": "voice", "seq": seq, "state": payload.state
        }),
        Inbound::Macro { seq, payload } => json!({
            "kind": "macro", "seq": seq, "name": payload.name
        }),
        Inbound::Asr { seq, payload } => json!({
            "kind": "asr", "seq": seq, "text": payload.text
        }),
        Inbound::VoiceSource { seq, payload } => json!({
            "kind": "voice_source", "seq": seq, "source": payload.source, "mode": payload.mode
        }),
        Inbound::BlePairKey { seq, payload } => json!({
            "kind": "ble_pair_key", "seq": seq, "key_id": payload.key_id
        }),
        Inbound::TuyaChat { seq, payload } => json!({
            "kind": "tuya_chat", "seq": seq, "role": payload.role,
            "bytes": payload.text.len()
        }),
        Inbound::WifiStatus { seq, payload } => json!({
            "kind": "wifi_status", "seq": seq, "state": payload.state, "ssid": payload.ssid
        }),
        Inbound::WifiList { seq, payload } => json!({
            "kind": "wifi_list_state", "seq": seq, "max": payload.max, "entries": payload.entries
        }),
        Inbound::ActivationStatus { seq, payload } => json!({
            "kind": "activation_status", "seq": seq,
            "state": payload.state, "qr_url": payload.qr_url
        }),
        Inbound::LicenseStatus { seq, payload } => json!({
            "kind": "license_status", "seq": seq, "state": payload.state
        }),
        Inbound::PidStatus { seq, payload } => json!({
            "kind": "pid_status", "seq": seq, "state": payload.state, "reason": payload.reason
        }),
        Inbound::ProvStatus { seq, payload } => json!({
            "kind": "prov_status", "seq": seq, "state": payload.state,
            "remaining_s": payload.remaining_s, "rejected": payload.rejected,
            "reason": payload.reason
        }),
        Inbound::LicenseInfo { seq, payload } => json!({
            "kind": "license_info", "seq": seq,
            "source": payload.source, "uuid": payload.uuid,
            "authkey_tail": payload.authkey_tail
        }),
        Inbound::SystemInfo { seq, payload } => json!({
            "kind": "system_info", "seq": seq,
            "fw": payload.fw, "transport": payload.transport,
            "wifi_name": payload.wifi_name, "wifi_state": payload.wifi_state,
            "ble_name": payload.ble_name, "ble_state": payload.ble_state,
            "cloud": payload.cloud, "license": payload.license
        }),
        Inbound::DeviceInfo { seq, payload } => json!({
            "kind": "device_info", "seq": seq,
            "fw": payload.fw, "companion": payload.companion, "handshake": payload.handshake,
            "cloud": payload.cloud,
            "wifi_ssid": payload.wifi_ssid, "wifi_ip": payload.wifi_ip,
            "wifi_rssi": payload.wifi_rssi, "wifi_mac": payload.wifi_mac,
            "ble_mac": payload.ble_mac, "ble_name": payload.ble_name, "ble_paired": payload.ble_paired,
            "pid": payload.pid, "uuid": payload.uuid, "authkey": payload.authkey, "license": payload.license
        }),
        Inbound::DeviceSettings { seq, payload } => json!({
            "kind": "device_settings", "seq": seq,
            "volume": payload.volume, "brightness": payload.brightness,
            "language": payload.language, "voice_mode": payload.voice_mode,
            "led_power": payload.led_power, "led_auto_off_s": payload.led_auto_off_s,
            "font_size": payload.font_size, "animation": payload.animation,
            "license": payload.license, "ble_paired": payload.ble_paired
        }),
        Inbound::MenuResult { seq, payload } => json!({
            "kind": "menu_result", "seq": seq, "id": payload.id,
            "selected": payload.selected, "dismissed": payload.dismissed, "reason": payload.reason,
            // The dictated text is an answer: logged as present, never shown.
            "dictated": !payload.text.is_empty()
        }),
        Inbound::BindSession { seq, payload } => json!({ "kind": "bind_session", "seq": seq, "slot": payload.slot }),
        Inbound::SceneState { seq, payload } => json!({ "kind": "scene_state", "seq": seq, "state": payload }),
        Inbound::SceneData { seq, payload } => json!({ "kind": "scene_data", "seq": seq, "scene": payload }),
        Inbound::OtaStatus(p) => json!({
            "kind": "ota_status", "state": p.state, "reason": p.reason, "version": p.version, "fw": p.fw
        }),
        Inbound::OtaAck(p) => json!({ "kind": "ota_ack", "next": p.next }),
        Inbound::HelloAck(ack) => json!({
            "kind": "hello_ack", "fw": ack.fw,
            "transport": ack.transport, "capabilities": ack.capabilities
        }),
        Inbound::Ping => json!({ "kind": "ping" }),
        Inbound::Pong => json!({ "kind": "pong" }),
        Inbound::Unknown { msg_type } => json!({ "kind": "unknown", "type": msg_type }),
    }
}

/// Launch the desktop application (Windows).
#[cfg(target_os = "windows")]
pub fn run() {
    app::run();
}

/// Keep the pure Rust core testable on the Linux build host without GTK/WebKit.
#[cfg(not(target_os = "windows"))]
pub fn run() {
    eprintln!("the Companion desktop application is supported on Windows");
}

// ───────────────────────── Tauri application (Windows) ─────────────────────────

#[cfg(target_os = "windows")]
mod app {
    use crate::ble_pair;
    use crate::ble_pairing;
    use crate::ble_sec;
    use crate::diagnostics;
    use crate::crew_api::{
        slot_display_status, CrewApiClient, CrewConnection, CrewEvent, CrewRealtime, CrewSlot,
        SendReceipt,
    };
    use crate::crew_menus::{self, MenuBoard};
    use crate::menu_wire::{notice_payload, NoticeLevel};
    use crate::voice_send::{self, VoiceDirect};
    use crate::pc_asr::VoiceSource;
    use crate::protocol::{self, Inbound, KeyAction, VoiceState};
    use crate::secrets;
    use crate::stream_asr::capture::Capture;
    use crate::stream_asr::{self, AsrSettings, EngineFactory, EngineKind, Session, SessionOutput, StartOptions};
    use std::sync::atomic::{AtomicBool, Ordering};
    use crate::hook_install;
    use crate::kirocrew_control::{Chord, ControlAction, ControlRouter, KiroCrewController};
    use crate::bind_store::BindStore;
    use crate::status_sink::{DisplayState as StatusDisplay, StatusSink, DEFAULT_STATUS_PORT};
    use crate::transport::ble_gatt::{self, BleDeviceInfo, BleGattTransport};
    use crate::transport::usb_serial::{self, SerialPortInfo, UsbSerialTransport};
    use crate::transport::{BoardTransport, TransportKind};
    use crate::{describe_inbound, inbound_to_json};
    use serde::Serialize;
    use serde_json::Value;
    use std::collections::HashMap;
    use std::sync::mpsc::{self, Receiver};
    use std::sync::{Arc, Mutex, TryLockError};
    use std::thread;
    use std::time::{Duration, Instant};
    use tauri::{AppHandle, Emitter, Manager, State};


    struct Inner {
        // NOTE: the board transport is NOT here — it lives in
        // `AppState.transport` behind its own lock so blocking serial writes
        // never hold the global `inner` mutex. `kind`/`endpoint` stay here as
        // cheap metadata describing the current connection.
        kind: TransportKind,
        endpoint: String,
        firmware: String,
        connection_id: u64,
        logs: Vec<String>,
        last_error: String,
        voice_source: VoiceSource,
        /// Last settings state reported by the AK. Defaults match firmware and
        /// is replaced by the board's authoritative values after each hello.
        device_volume: u8,
        device_brightness: u8,
        device_settings_synced: bool,
        /// FP-4b (route B): the pure router that maps board events to control
        /// actions. The window-focus + SendInput controller lives in
        /// `AppState::controller`, outside this lock (CP-04).
        kc_router: ControlRouter,
        /// Enabled by default so board events immediately drive the local Kiro
        /// Crew window. When false, board key/nav frames only reflect trigger
        /// state in the INPUT view; every board-triggered PC action, including
        /// PC ASR, is blocked until control is enabled again.
        control_enabled: bool,
        /// FP-4b STATUS ("the eyes"): the running localhost hook sink, if
        /// started. `status_sink_start` sets it; `status_sink_stop` / drop
        /// tears it down (releasing the port + removing the handoff file).
        status_sink: Option<StatusSink>,
        /// The sink's token, cached so `status_token()` can return it without
        /// re-reading the handoff file. Empty when no sink is running.
        status_token: String,
        /// Last mapped status per Kiro Crew `session_id` (design: keep last-status
        /// per session, emit a `display` frame for it, push to the UI).
        kc_status: HashMap<String, StatusDisplay>,
        /// FP-4b Route STATUS — persisted "watched chat" filter. `None` = follow
        /// every session (only the UI live-list keys them apart). `Some(id)` = pin ONE
        /// Kiro Crew `session_id`: only that session's status is forwarded as a
        /// `display` frame to the board, so a background chat's activity no
        /// longer clobbers the LCD. The `kc-status` UI event is ALWAYS pushed
        /// (pinned or not) so the picker can list every active session and the
        /// user can choose which one to pin.
        kc_pinned_session: Option<String>,
        /// FP-4b M4 (§3.2): per-key chat-number store for K1-K3, loaded from
        /// `~/.ak-companion/binds.json` at startup. Each key binds to a
        /// chat NUMBER (1..=9); pressing it fires `Alt+N` (jump to chat N). The
        /// numbers here are mirrored into `kc_router.ctx.jump_chats`.
        kc_binds: BindStore,
        /// Local Kiro Crew REST client. Authentication bootstraps from the
        /// gateway's rotating `.local_secret` before every operation.
        crew_api: Option<CrewApiClient>,
        /// Background `/api/ws` connection. Kept separate from the Hook sink:
        /// API status is primary, Hook remains an independent fallback.
        crew_realtime: Option<CrewRealtime>,
        crew_api_connected: bool,
        crew_api_base_url: String,
        crew_api_last_error: String,
        crew_api_slots: Vec<CrewSlot>,
        /// Stable Crew slot selected by K1/K2/K3 while API is connected.
        crew_api_active_slot: Option<String>,
        /// Review-before-send drafts keyed by stable Crew slot. ASR populates a
        /// draft; K5 submits it through REST, preserving the existing UX.
        crew_api_drafts: HashMap<String, String>,
        /// LCD menus (approvals, options, questions, model/mode).
        menus: MenuBoard,
        /// Settings > Bind session on the keyboard, while a step is open.
        bind_flow: Option<crate::bind_flow::BindFlow>,
        /// The keyboard advertised `menu` in hello_ack.
        menu_capable: bool,
        /// The last Crew connection attempt failed the gateway identity check
        /// (CP-09); the keyboard is told once per failure streak.
        crew_identity_failed: bool,
        /// Local OTA over USB: the verified package the user picked, the
        /// running transfer, and the key the keyboard reported after hello
        /// (empty = firmware without OTA).
        ota_package: Option<crate::ota::Package>,
        ota: Option<crate::ota::Sender>,
        ota_device_key: String,
        /// Last `ota-progress` pushed to the UI ("state:percent:reason").
        ota_last_emit: String,
        /// PC streaming ASR (docs/pc-asr.md): settings without secrets, the
        /// utterance in progress, and the local model download's cancel flag.
        asr_settings: AsrSettings,
        pc_asr: Option<PcAsrRun>,
        pc_asr_next_id: u64,
        asr_download: Option<Arc<AtomicBool>>,
        /// The keyboard waits in ② for the PC ASR text (`pc_asr_wait` in
        /// hello_ack); it must then always get an `input_restore` back.
        board_pc_wait: bool,
        /// Why the last keyboard utterance failed before the voice key was
        /// released (sent when `voice:stop` arrives).
        pc_asr_failed: Option<&'static str>,
        /// PC mode: the Crew input box was filled; K6 / K4 / K1 act on it until
        /// this time (`voice_send::PC_FILL_WINDOW`).
        pc_fill_until: Option<Instant>,
        /// FR-03 scenes: the store and the keyboard's slots (scene_sync.rs).
        scenes: crate::scene_sync::SceneManager,
        /// Whether the keyboard answered this connection's `hello` in time.
        handshake: crate::handshake::HandshakeWatch,
    }

    impl Inner {
        /// The `bindings` frame: aliases, session names and each slot's voice
        /// input mode (`voice_pc`).
        fn bindings_frame(&self) -> Value {
            let b = &self.kc_binds;
            protocol::bindings_payload(&b.aliases, &b.names, b.voice_pc(self.control_enabled))
        }

        fn pc_fill_active(&self) -> bool {
            self.pc_fill_until.is_some_and(|t| t > Instant::now())
        }
    }

    /// One PC ASR utterance: the voice key held (or the settings-page test).
    struct PcAsrRun {
        id: u64,
        /// The chat slot (1..=3) the keyboard named in `voice:start`; 0 = none.
        slot: u8,
        /// Set right after registration (the run exists first so an instant
        /// failure still finds it).
        session: Option<Session>,
        capture: Option<Capture>,
        /// `false` for the settings-page test: the text stays in the UI.
        to_keyboard: bool,
        fell_back: bool,
    }

    struct AppState {
        inner: Mutex<Inner>,
        /// The board transport lives behind its OWN lock, separate from the
        /// `inner` state mutex. Serial `write_json` is a blocking IO call that
        /// can stall for seconds when the link is congested; keeping it out of
        /// the global `inner` lock means a slow/stuck write no longer freezes
        /// the UI, the watchdog, or any other command. The two locks are never
        /// held at the same time (see `write_board_guarded`).
        transport: Mutex<Option<Box<dyn BoardTransport>>>,
        /// Window focus + SendInput controller ("the hands"). Focus and key
        /// injection sleep for tens to hundreds of milliseconds, so they run
        /// under this dedicated lock, never under `inner` (CP-04). Lock order:
        /// never take `inner` or `transport` while holding this lock.
        controller: Mutex<KiroCrewController>,
        /// Set while a `connect_device` call is opening a transport (CP-05).
        connecting: std::sync::atomic::AtomicBool,
        /// Auto-connect preferences and the manual-disconnect latch.
        link: Mutex<crate::connection_prefs::LinkState>,
    }

    type Shared = Arc<AppState>;

    #[derive(Serialize)]
    struct Status {
        device_connected: bool,
        transport: TransportKind,
        endpoint: String,
        firmware_version: String,
        last_error: String,
        voice_source: String,
        device_volume: u8,
        device_brightness: u8,
        device_settings_synced: bool,
        control_enabled: bool,
        crew_api_connected: bool,
        crew_api_base_url: String,
        crew_api_last_error: String,
        crew_api_slots: usize,
        focus_on_select: bool,
        show_workers: bool,
        voice_force_keyboard: bool,
        /// Connected, but `hello` got no `hello_ack` within the timeout.
        handshake_missing: bool,
    }

    fn log_line(app: &AppHandle, message: impl Into<String>) {
        let message = message.into();
        if let Some(state) = app.try_state::<Shared>() {
            if let Ok(mut inner) = state.inner.lock() {
                inner.logs.insert(0, message.clone());
                inner.logs.truncate(200);
            }
        }
        let _ = app.emit("companion-log", message);
    }

    fn payload_kind(payload: &Value) -> &str {
        payload.get("type").and_then(Value::as_str).unwrap_or("unknown")
    }

    fn write_board_guarded(
        state: &Shared,
        payload: &Value,
        require_usb: bool,
    ) -> Result<(), String> {
        let kind = payload_kind(payload);
        let routine = matches!(kind, "ack" | "pong");
        let started = Instant::now();
        if !routine {
            diagnostics::record("DEBUG", "board-tx", &format!("begin type={kind}"));
        }
        // Read the connection kind under a SHORT hold of the inner lock, then
        // release it immediately — the blocking serial write below must never
        // run while holding the global state mutex.
        let kind_enum = {
            let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kind
        };
        // Now take the dedicated transport lock for the (possibly slow) write.
        // A congested serial link stalls only this lock, not `inner`.
        let lock_started = Instant::now();
        let mut transport_guard = state
            .transport
            .lock()
            .map_err(|_| "transport lock poisoned")?;
        let lock_wait = lock_started.elapsed();
        if lock_wait >= Duration::from_millis(250) {
            diagnostics::record(
                "WARN",
                "board-tx",
                &format!("board write waited {}ms for transport lock type={kind}", lock_wait.as_millis()),
            );
        }
        if require_usb {
            crate::ensure_usb_provisioning_transport(transport_guard.is_some(), kind_enum)?;
        }
        let transport = transport_guard.as_mut().ok_or("device is not connected")?;
        let result = transport.write_json(payload);
        let elapsed = started.elapsed();
        let slow_or_failed = result.is_err() || elapsed >= Duration::from_millis(250);
        if !routine || slow_or_failed {
            diagnostics::record(
                if slow_or_failed { "WARN" } else { "DEBUG" },
                "board-tx",
                &format!(
                    "end type={kind} elapsed_ms={} result={}",
                    elapsed.as_millis(),
                    if result.is_ok() { "ok" } else { "error" }
                ),
            );
        }
        result
    }

    fn write_board(state: &Shared, payload: &Value) -> Result<(), String> {
        write_board_guarded(state, payload, false)
    }

    fn write_board_usb_only(state: &Shared, payload: &Value) -> Result<(), String> {
        write_board_guarded(state, payload, true)
    }

    fn spawn_state_watchdog(state: Shared) {
        let result = thread::Builder::new()
            .name("companion-watchdog".to_string())
            .spawn(move || {
                let mut busy_since: Option<Instant> = None;
                let mut last_report = Instant::now();
                loop {
                    thread::sleep(Duration::from_secs(1));
                    match state.inner.try_lock() {
                        Ok(_) => {
                            if let Some(started) = busy_since.take() {
                                diagnostics::record(
                                    "INFO",
                                    "state-lock",
                                    &format!("mutex recovered after {}ms", started.elapsed().as_millis()),
                                );
                            }
                        }
                        Err(TryLockError::WouldBlock) => {
                            let started = busy_since.get_or_insert_with(Instant::now);
                            if started.elapsed() >= Duration::from_secs(2)
                                && last_report.elapsed() >= Duration::from_secs(5)
                            {
                                diagnostics::record(
                                    "WARN",
                                    "state-lock",
                                    &format!("mutex busy for {}ms", started.elapsed().as_millis()),
                                );
                                last_report = Instant::now();
                            }
                        }
                        Err(TryLockError::Poisoned(_)) => {
                            diagnostics::record("ERROR", "state-lock", "mutex poisoned");
                            return;
                        }
                    }
                }
            });
        if let Err(error) = result {
            diagnostics::record(
                "ERROR",
                "watchdog",
                &format!("failed to start state watchdog: {error}"),
            );
        }
    }

    #[tauri::command]
    fn list_serial_ports() -> Result<Vec<SerialPortInfo>, String> {
        usb_serial::list_serial_ports()
    }

    /// Scan ~4s for BLE peripherals matching the board's advertisement
    /// (local name `Ki-board-XXXXXX`, older `Agentic Keyboard-XXXXXX` / `AK-KB`, or
    /// a supported service) and return their id/name.
    #[tauri::command]
    fn scan_ble() -> Result<Vec<BleDeviceInfo>, String> {
        ble_gatt::scan()
    }

    #[tauri::command]
    fn get_logs(state: State<Shared>) -> Result<Vec<String>, String> {
        Ok(state.inner.lock().map_err(|_| "state lock poisoned")?.logs.clone())
    }

    #[tauri::command]
    fn clear_logs(state: State<Shared>) -> Result<(), String> {
        state.inner.lock().map_err(|_| "state lock poisoned")?.logs.clear();
        Ok(())
    }

    /// Enable/disable routing board key/nav frames to Kiro Crew control. Default
    /// is ON; while disabled the board only reports trigger state to the UI and
    /// never drives the local Kiro Crew window.
    #[tauri::command]
    fn set_control_enabled(app: AppHandle, state: State<Shared>, enabled: bool) -> Result<(), String> {
        let frame = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.control_enabled = enabled;
            if !enabled {
                inner.pc_fill_until = None;
            }
            inner.bindings_frame() // PC mode needs the keyboard to control the PC
        };
        let _ = write_board(state.inner(), &frame);
        log_line(
            &app,
            format!("[KC-CTL] board control {}", if enabled { "ENABLED" } else { "disabled" }),
        );
        Ok(())
    }

    /// Whether a K1-K3 chat select also brings the Crew window to the front.
    /// Persisted in the Companion config file; default on.
    #[tauri::command]
    fn set_focus_on_select(app: AppHandle, state: State<Shared>, enabled: bool) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let mut next = inner.kc_binds.clone();
        next.focus_on_select = enabled;
        next.save()?;
        inner.kc_binds = next;
        drop(inner);
        log_line(&app, format!("[KC-CTL] bring Crew to front on chat select {}", if enabled { "on" } else { "off" }));
        Ok(())
    }

    /// List worker sessions (Crew Mode / Bot workers) in the keyboard's Bind
    /// session menu. Persisted in binds.json; off by default.
    #[tauri::command]
    fn set_show_workers(app: AppHandle, state: State<Shared>, enabled: bool) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let mut next = inner.kc_binds.clone();
        next.show_workers = enabled;
        next.save()?;
        inner.kc_binds = next;
        drop(inner);
        log_line(&app, format!("[BIND] worker sessions in the keyboard list {}", if enabled { "shown" } else { "hidden" }));
        Ok(())
    }

    /// Voice input: `true` = keyboard mode for every slot (text on the
    /// keyboard's input strip, K6 sends through the Crew API); `false` =
    /// automatic, a slot with an Alt+N number uses PC mode. Persisted in
    /// binds.json.
    #[tauri::command]
    fn set_voice_force_keyboard(app: AppHandle, state: State<Shared>, enabled: bool) -> Result<(), String> {
        let frame = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            let mut next = inner.kc_binds.clone();
            next.voice_force_keyboard = enabled;
            next.save()?;
            inner.kc_binds = next;
            inner.bindings_frame()
        };
        let _ = write_board(state.inner(), &frame);
        log_line(
            &app,
            format!("[VOICE] voice input: {}", if enabled { "keyboard mode for every slot" } else { "automatic (Alt+N slots use PC mode)" }),
        );
        Ok(())
    }

    #[tauri::command]
    fn get_status(state: State<Shared>) -> Result<Status, String> {
        // Read connectedness from the transport lock first, then release it
        // before taking the inner lock (never hold both at once).
        let device_connected = state
            .transport
            .lock()
            .map(|t| t.is_some())
            .unwrap_or(false);
        let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        Ok(Status {
            device_connected,
            transport: inner.kind,
            endpoint: inner.endpoint.clone(),
            firmware_version: inner.firmware.clone(),
            last_error: inner.last_error.clone(),
            voice_source: inner.voice_source.as_str().to_string(),
            device_volume: inner.device_volume,
            device_brightness: inner.device_brightness,
            device_settings_synced: inner.device_settings_synced,
            control_enabled: inner.control_enabled,
            focus_on_select: inner.kc_binds.focus_on_select,
            show_workers: inner.kc_binds.show_workers,
            voice_force_keyboard: inner.kc_binds.voice_force_keyboard,
            crew_api_connected: inner.crew_api_connected,
            crew_api_base_url: inner.crew_api_base_url.clone(),
            crew_api_last_error: inner.crew_api_last_error.clone(),
            crew_api_slots: inner.crew_api_slots.len(),
            handshake_missing: device_connected && inner.handshake.missing(inner.connection_id),
        })
    }

    /// Explicitly (re)start the Crew WebSocket worker. Startup invokes this
    /// automatically; the command is exposed for UI diagnostics/recovery.
    #[tauri::command]
    fn crew_api_start(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        start_crew_realtime(&app, state.inner())
    }

    #[tauri::command]
    fn crew_api_stop(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let (worker, base_url) = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.crew_api_connected = false;
            (inner.crew_realtime.take(), inner.crew_api_base_url.clone())
        };
        if let Some(mut worker) = worker {
            worker.stop();
        }
        let connection = CrewConnection {
            connected: false,
            base_url,
            error: "stopped by user".to_string(),
            retry_secs: 0,
        };
        let _ = app.emit("crew-api-connection", &connection);
        log_line(&app, "[CREW-API] stopped · Hook fallback remains active");
        Ok(())
    }

    #[tauri::command]
    fn crew_api_list_slots(app: AppHandle, state: State<Shared>) -> Result<Vec<CrewSlot>, String> {
        let client = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .crew_api
            .clone()
            .ok_or("Kiro Crew API client is unavailable")?;
        let slots = client.list_slots()?;
        if let Ok(mut inner) = state.inner.lock() {
            inner.crew_api_slots = slots.clone();
            inner.crew_api_last_error.clear();
        }
        let _ = app.emit("crew-api-slots", &slots);
        Ok(slots)
    }

    #[tauri::command]
    fn crew_api_send_message(
        app: AppHandle,
        state: State<Shared>,
        slot: String,
        message: String,
    ) -> Result<SendReceipt, String> {
        let client = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .crew_api
            .clone()
            .ok_or("Kiro Crew API client is unavailable")?;
        let receipt = client.send_message(&slot, &message)?;
        if let Ok(mut inner) = state.inner.lock() {
            inner.crew_api_drafts.remove(&slot);
            inner.crew_api_active_slot = Some(slot.clone());
        }
        emit_crew_draft(&app, &slot, "");
        log_line(
            &app,
            format!("[CREW-API] message accepted for slot {slot} (content withheld from logs)"),
        );
        Ok(receipt)
    }

    #[tauri::command]
    fn crew_api_slot_action(
        app: AppHandle,
        state: State<Shared>,
        slot: String,
        action: String,
    ) -> Result<(), String> {
        let client = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .crew_api
            .clone()
            .ok_or("Kiro Crew API client is unavailable")?;
        match action.as_str() {
            "continue" => client.continue_slot(&slot)?,
            "interrupt" => client.interrupt_slot(&slot)?,
            "stop" => client.stop_slot(&slot)?,
            _ => return Err(format!("unsupported Kiro Crew slot action {action:?}")),
        }
        log_line(&app, format!("[CREW-API] {action} accepted for slot {slot}"));
        Ok(())
    }

    #[tauri::command]
    fn connect_device(
        app: AppHandle,
        state: State<Shared>,
        transport: Option<String>,
        port_name: Option<String>,
    ) -> Result<(), String> {
        // A user Connect re-enables auto-reconnect after a manual Disconnect.
        if let Ok(mut link) = state.link.lock() {
            link.manual_disconnect = false;
        }
        connect_transport(app, state.inner(), transport.as_deref().unwrap_or("usb"), port_name)
    }

    /// Open the transport and bring the board link up. Used by the Connect
    /// command and by the reconnect supervisor.
    fn connect_transport(
        app: AppHandle,
        shared: &Shared,
        transport_kind: &str,
        port_name: Option<String>,
    ) -> Result<(), String> {
        diagnostics::record(
            "INFO",
            "connect",
            &format!("begin transport={transport_kind}"),
        );
        // One connect at a time (CP-05): the presence check below and the
        // install after opening the port are separate lock holds, so without
        // this claim two calls could both pass the check and open the same
        // port or BLE device.
        let _claim = crate::ConnectClaim::try_claim(&shared.connecting)
            .ok_or("A connection attempt is already in progress.")?;
        // Only one transport at a time. If something is already connected, do
        // NOT silently tear it down — tell the user to disconnect first. This
        // makes switching USB<->BLE an explicit two-step action and avoids
        // yanking a live link out from under the reader thread.
        {
            let already = shared
                .transport
                .lock()
                .map(|t| t.is_some())
                .unwrap_or(false);
            if already {
                let inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
                let current = match inner.kind {
                    TransportKind::Usb => "USB",
                    TransportKind::Ble => "BLE",
                    TransportKind::None => "a transport",
                };
                let want = if transport_kind == "ble" { "BLE" } else { "USB" };
                return Err(format!(
                    "Already connected over {current} ({}). Disconnect first before connecting over {want}.",
                    if inner.endpoint.is_empty() { "—" } else { inner.endpoint.as_str() }
                ));
            }
        }

        let (line_tx, line_rx) = mpsc::channel::<String>();
        // Build the selected backend. Both hand complete JSONL lines to the
        // same LineSender, so the reader thread below is transport-agnostic.
        let boxed: Box<dyn BoardTransport> = match transport_kind {
            "ble" => {
                let device_id = port_name
                    .clone()
                    .filter(|s| !s.is_empty())
                    .ok_or("BLE connect requires a device id (scan and pick one)")?;
                let key = ble_pairing::load().ok_or(
                    "This PC is not paired with the keyboard for BLE. Open Settings > BLE pairing on the keyboard and use Pair over Bluetooth, or connect over USB and pair.",
                )?;
                let transport =
                    BleGattTransport::connect(device_id, line_tx, key).map_err(|err| {
                        diagnostics::record("ERROR", "connect", &format!("BLE failed: {err}"));
                        log_line(&app, format!("[ERROR] ble transport: {err}"));
                        err
                    })?;
                Box::new(transport)
            }
            _ => {
                let transport =
                    UsbSerialTransport::connect(port_name.clone(), line_tx).map_err(|err| {
                        diagnostics::record("ERROR", "connect", &format!("USB failed: {err}"));
                        log_line(&app, format!("[ERROR] transport: {err}"));
                        err
                    })?;
                Box::new(transport)
            }
        };
        let kind = boxed.kind();
        let endpoint = boxed.endpoint();

        // Install the transport under its own lock first, then update the inner
        // metadata. The two locks are taken sequentially, never nested.
        {
            let mut transport_guard =
                shared.transport.lock().map_err(|_| "transport lock poisoned")?;
            *transport_guard = Some(boxed);
        }
        let connection_id = {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kind = kind;
            inner.endpoint = endpoint.clone();
            inner.last_error.clear();
            inner.device_settings_synced = false;
            inner.connection_id = inner.connection_id.wrapping_add(1);
            // Armed before the reader starts, so an instant ack is not missed.
            let id = inner.connection_id;
            inner.handshake.begin(id, Instant::now());
            id
        };

        // Start the reader BEFORE greeting so the board's hello_ack is not
        // missed. USB/UART is physically trusted and greets immediately; for
        // BLE the subscription is already confirmed live inside
        // `BleGattTransport::connect`, so it is likewise safe to greet now
        // (BLE, unlike USB, is not greet-immediately at the wire level — the
        // greeting deliberately waits until after notify-subscription).
        // The Device info shown for the previous keyboard must not outlive its connection.
        let _ = app.emit("board-info-reset", ());
        spawn_board_reader(app.clone(), shared.clone(), line_rx, connection_id);
        spawn_handshake_timer(app.clone(), shared.clone(), connection_id);
        write_board(shared, &protocol::hello_payload())?;
        let (aliases, status_payloads) = {
            let inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            let aliases = inner.kc_binds.aliases.clone();
            let bound = inner.kc_binds.status_ids();
            let status_payloads = (0..3)
                .map(|index| {
                    let session_id = bound.get(index).map(|value| value.trim()).unwrap_or("");
                    if let Some(status) = inner
                        .kc_status
                        .get(session_id)
                        .filter(|_| !session_id.is_empty())
                    {
                        protocol::display_payload(
                            index as u8 + 1,
                            &status.agent_name,
                            &status.state,
                            &status.text,
                        )
                    } else {
                        let alias = aliases.get(index).map(String::as_str).unwrap_or("");
                        protocol::display_payload(index as u8 + 1, alias, "unbound", "")
                    }
                })
                .collect::<Vec<_>>();
            (inner.bindings_frame(), status_payloads)
        };
        if let Err(err) = write_board(shared, &aliases) {
            log_line(&app, format!("[WARN] binding alias sync failed: {err}"));
        }
        // Replay all three slots on every board link. Explicit unbound frames
        // clear stale colors for slots that have no current cached status.
        for payload in status_payloads {
            if let Err(err) = write_board(shared, &payload) {
                log_line(&app, format!("[WARN] status replay failed: {err}"));
            }
        }
        if transport_kind == "ble" {
            remember_ble_device(shared, port_name.clone());
        }
        log_line(
            &app,
            format!("[INFO] connected transport={kind:?} endpoint={endpoint}"),
        );
        diagnostics::record("INFO", "connect", &format!("ready transport={kind:?}"));
        Ok(())
    }

    /// Remember the BLE device this PC connected to, so auto mode can return to it.
    fn remember_ble_device(shared: &Shared, device_id: Option<String>) {
        let Some(device_id) = device_id.filter(|id| !id.is_empty()) else {
            return;
        };
        let snapshot = match shared.link.lock() {
            Ok(mut link) => {
                link.prefs.ble_device = device_id;
                link.prefs.clone()
            }
            Err(_) => return,
        };
        if let Err(err) = crate::connection_prefs::save(&snapshot) {
            diagnostics::record("WARN", "connect", &format!("save BLE device failed: {err}"));
        }
    }

    /// One timer per connection: when `hello` is still unanswered after
    /// `HELLO_ACK_TIMEOUT`, send it once more (a keyboard still on its boot
    /// splash answers late); when that one is unanswered too, log it once.
    /// The UI reads the flag from `get_status`.
    fn spawn_handshake_timer(app: AppHandle, state: Shared, connection_id: u64) {
        thread::spawn(move || {
            thread::sleep(crate::handshake::HELLO_ACK_TIMEOUT);
            let resend = state
                .inner
                .lock()
                .map(|mut inner| inner.handshake.resend(connection_id, Instant::now()))
                .unwrap_or(false);
            if !resend {
                return;
            }
            diagnostics::record("INFO", "connect", "no hello_ack within 3 s; hello sent again");
            if let Err(err) = write_board(&state, &protocol::hello_payload()) {
                log_line(&app, format!("[WARN] hello resend failed: {err}"));
            }
            thread::sleep(crate::handshake::HELLO_ACK_TIMEOUT);
            let overdue = state
                .inner
                .lock()
                .map(|mut inner| inner.handshake.poll(connection_id, Instant::now()))
                .unwrap_or(false);
            if overdue {
                log_line(
                    &app,
                    "[WARN] keyboard did not answer hello within 6 s (sent twice); menus and PC recognition are unavailable".to_string(),
                );
                diagnostics::record("WARN", "connect", "no hello_ack within 6 s");
            }
        });
    }

    /// The board's line channel closed while this connection was still current:
    /// the cable came out or the BLE link dropped. Clear the transport so the UI
    /// shows offline and the reconnect supervisor takes over.
    // shortcut: the "still current" check and the clear are two lock holds, so a
    // connect that lands between them could be cleared. Upgrade to one lock if
    // reconnect races show up in the Windows logs.
    fn drop_lost_link(app: &AppHandle, state: &Shared, connection_id: u64) {
        let current = state
            .inner
            .lock()
            .map(|inner| inner.connection_id == connection_id)
            .unwrap_or(false);
        if !current {
            return;
        }
        let taken = state.transport.lock().ok().and_then(|mut slot| slot.take());
        if let Some(mut transport) = taken {
            transport.disconnect();
        }
        if let Ok(mut inner) = state.inner.lock() {
            inner.kind = TransportKind::None;
            inner.endpoint.clear();
            inner.device_settings_synced = false;
            inner.connection_id = inner.connection_id.wrapping_add(1);
        }
        log_line(app, "[WARN] board link lost; reconnecting when a device is available".to_string());
        diagnostics::record("WARN", "connect", "link lost");
    }

    /// Auto-reconnect. While the link is down and auto-connect is on, try the
    /// preferred device on the backoff schedule in `connection_prefs`. Ports are
    /// enumerated only when an attempt is due.
    fn spawn_reconnect_supervisor(app: AppHandle, state: Shared) {
        use crate::connection_prefs::{Reconnect, Step, Target};
        thread::spawn(move || {
            let mut policy = Reconnect::default();
            loop {
                thread::sleep(std::time::Duration::from_secs(1));
                let (enabled, prefs) = match state.link.lock() {
                    Ok(link) => (link.auto_enabled(), link.prefs.clone()),
                    Err(_) => continue,
                };
                let connected = state.transport.lock().map(|t| t.is_some()).unwrap_or(false);
                let now = std::time::Instant::now();
                let due = !connected && enabled && policy.due(now);
                let (ports, ble_paired) = if due {
                    (crate::transport::usb_serial::scan_ports(), crate::ble_pairing::load().is_some())
                } else {
                    (Vec::new(), false)
                };
                match policy.step(now, connected, enabled, &prefs, &ports, ble_paired) {
                    Step::Wait => {}
                    Step::Attempt(target) => {
                        let result = match target {
                            Target::Usb(port) => connect_transport(app.clone(), &state, "usb", port),
                            Target::Ble(id) => connect_transport(app.clone(), &state, "ble", Some(id)),
                        };
                        // On success the next tick sees the link up and resets the backoff.
                        if let Err(err) = result {
                            log_line(&app, format!("[INFO] auto-reconnect failed: {err}"));
                            policy.failed(std::time::Instant::now());
                        }
                    }
                }
            }
        });
    }

    #[tauri::command]
    fn get_connection_prefs(state: State<Shared>) -> Result<crate::connection_prefs::Prefs, String> {
        state
            .link
            .lock()
            .map(|link| link.prefs.clone())
            .map_err(|_| "link lock poisoned".to_string())
    }

    #[tauri::command]
    fn set_connection_prefs(
        state: State<Shared>,
        prefs: crate::connection_prefs::Prefs,
    ) -> Result<(), String> {
        let prefs = prefs.normalized();
        crate::connection_prefs::save(&prefs)?;
        let mut link = state.link.lock().map_err(|_| "link lock poisoned")?;
        link.prefs = prefs;
        Ok(())
    }

    #[tauri::command]
    fn disconnect_device(state: State<Shared>) -> Result<(), String> {
        diagnostics::record("INFO", "disconnect", "begin");
        // A user Disconnect stops auto-reconnect until the user connects again
        // or the program restarts.
        if let Ok(mut link) = state.link.lock() {
            link.manual_disconnect = true;
        }
        // Take the transport out under its OWN lock, then say bye and close it
        // with no lock held: the closing flush can block 0.3-2 s on Windows
        // CH34x drivers and must not stall other board writers (CP-04).
        let taken = state
            .transport
            .lock()
            .map_err(|_| "transport lock poisoned")?
            .take();
        if let Some(mut transport) = taken {
            // Best-effort graceful goodbye: plain UART has no hardware
            // disconnect signal, so without this the board's link header would
            // stay "USB Connected" forever.
            let _ = transport.write_json(&protocol::bye_payload());
            // Sets the transport stop flag so the low-level serial reader
            // thread exits and releases the OS port; dropping `transport`
            // closes the port handle. The app-level board reader then sees
            // the transport gone on its next liveness check and also exits.
            transport.disconnect();
        }
        // Clear the connection metadata under the inner lock.
        {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kind = TransportKind::None;
            inner.endpoint.clear();
            inner.device_settings_synced = false;
            inner.connection_id = inner.connection_id.wrapping_add(1);
            inner.menu_capable = false;
            inner.board_pc_wait = false;
            inner.pc_fill_until = None;
            inner.menus.reset_display();
            inner.scenes.on_disconnect();
        }
        diagnostics::record("INFO", "disconnect", "complete");
        Ok(())
    }

    #[tauri::command]
    fn send_display(
        state: State<Shared>,
        chat_slot: u8,
        agent_name: String,
        display_state: String,
        text: String,
    ) -> Result<(), String> {
        if !(1..=3).contains(&chat_slot) {
            return Err(format!("chat slot must be 1..=3 (got {chat_slot})"));
        }
        write_board(
            state.inner(),
            &protocol::display_payload(chat_slot, &agent_name, &display_state, &text),
        )
    }

    #[tauri::command]
    fn send_led(state: State<Shared>, led_state: String, color: String) -> Result<(), String> {
        write_board(state.inner(), &protocol::led_payload(&led_state, &color))
    }

    #[tauri::command]
    fn send_config(state: State<Shared>, voice_engine: String) -> Result<(), String> {
        write_board(state.inner(), &protocol::config_payload(&voice_engine))
    }

    /// Set both runtime device values. The AK applies the command and reports a
    /// `device_settings_state` frame; the UI updates from that authoritative echo.
    #[tauri::command]
    fn set_device_settings(
        state: State<Shared>,
        volume: u8,
        brightness: u8,
    ) -> Result<(), String> {
        if volume > 100 {
            return Err(format!("volume must be 0..=100 (got {volume})"));
        }
        if !(10..=100).contains(&brightness) {
            return Err(format!("brightness must be 10..=100 (got {brightness})"));
        }
        write_board(
            state.inner(),
            &protocol::device_settings_payload(volume, brightness),
        )
    }

    /// FR-02: change any of the normal keyboard settings (volume, brightness,
    /// language, voice mode, LED power, Auto power-off). Allowed over USB and
    /// the BLE secure channel; the board echoes `device_settings_state`.
    #[tauri::command]
    fn update_device_settings(
        state: State<Shared>,
        update: protocol::DeviceSettingsUpdate,
    ) -> Result<(), String> {
        update.validate()?;
        write_board(state.inner(), &protocol::device_settings_update_payload(&update))
    }

    /// FR-02: ask the keyboard to open its pairing window. USB only; the
    /// keyboard asks the user (K6 / K1) before anything opens.
    #[tauri::command]
    fn keyboard_open_pairing(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::prov_open_payload())
    }

    /// FR-02: ask the keyboard to clear its BLE pairing key. USB only; the
    /// keyboard asks the user (K6 / K1) first.
    #[tauri::command]
    fn keyboard_ble_unpair(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::ble_unpair_payload())
    }

    /// Set the shared interface language ("zh" / "en", FR-01). The board
    /// applies it, stores it and echoes `device_settings_state`; the UI keeps
    /// following that echo, so the last change on either side wins.
    #[tauri::command]
    fn set_device_language(state: State<Shared>, language: String) -> Result<(), String> {
        if language != "zh" && language != "en" {
            return Err(format!("language must be zh or en (got {language})"));
        }
        write_board(state.inner(), &protocol::device_language_payload(&language))
    }

    /// Downlink per-gesture keymap bindings to the board (plan A, stage 乙).
    /// `bindings` is a list of (field, value) where field is "<key>.kind"
    /// (value "voice"/"normal") or "<key>.<gesture>" (gesture single/double/long,
    /// value an action: none/cancel_char/cancel_all/confirm/chatN/instanceN).
    /// An empty value leaves that binding unchanged. The board caches the
    /// resolved table in TAL KV and acks.
    #[tauri::command]
    fn set_keymap(state: State<Shared>, bindings: Vec<(String, String)>) -> Result<(), String> {
        const KEYS: [&str; 7] = ["k1", "k2", "k3", "k4", "k5", "k6", "enc"];
        const GESTURES: [&str; 4] = ["kind", "single", "double", "long"];
        const ACTIONS: [&str; 6] = ["none", "cancel_char", "cancel_all", "confirm", "model_menu", "model_toggle"];
        for (field, value) in &bindings {
            // field must be "<key>.<slot>" with a known key and slot.
            let (key, slot) = field
                .split_once('.')
                .ok_or_else(|| format!("invalid keymap field '{field}' (expected <key>.<slot>)"))?;
            if !KEYS.contains(&key) {
                return Err(format!("invalid keymap key '{key}'"));
            }
            if !GESTURES.contains(&slot) {
                return Err(format!("invalid keymap slot '{slot}'"));
            }
            if value.is_empty() {
                continue;
            }
            // Validate the value per slot.
            let ok = if slot == "kind" {
                value == "voice" || value == "normal"
            } else {
                // An action: a base action, or chat/instance followed by EXACTLY
                // one digit 1-9 (matches the firmware's ak_keymap_action_from_str
                // grammar, so the companion never emits a value the board drops).
                ACTIONS.contains(&value.as_str())
                    || value
                        .strip_prefix("chat")
                        .or_else(|| value.strip_prefix("instance"))
                        .is_some_and(|n| n.len() == 1 && matches!(n.as_bytes()[0], b'1'..=b'9'))
            };
            if !ok {
                return Err(format!("invalid keymap value '{value}' for {field}"));
            }
        }
        write_board(state.inner(), &protocol::keymap_payload(&bindings))?;
        Ok(())
    }

    /// Provision Wi-Fi on the board: send SSID + password over the JSONL
    /// channel. The board applies it via netmgr and reports back a wifi_status
    /// frame (surfaced to the UI as a 'wifi-status' event).
    #[tauri::command]
    fn send_wifi(state: State<Shared>, ssid: String, pass: String) -> Result<(), String> {
        if ssid.trim().is_empty() {
            return Err("SSID is required".to_string());
        }
        write_board_usb_only(state.inner(), &protocol::wifi_payload(ssid.trim(), &pass))
    }

    /// FR-15: ask the keyboard for its remembered Wi-Fi networks (SSIDs only).
    /// Works over USB and the paired BLE channel; the answer arrives as the
    /// `wifi-saved` event.
    #[tauri::command]
    fn wifi_saved_list(state: State<Shared>) -> Result<(), String> {
        write_board(state.inner(), &protocol::wifi_list_payload())
    }

    /// FR-15: forget one remembered network. USB only; the keyboard answers
    /// with the new list.
    #[tauri::command]
    fn wifi_saved_forget(state: State<Shared>, ssid: String) -> Result<(), String> {
        if ssid.is_empty() || ssid.len() > 32 {
            return Err("SSID must be 1..32 bytes".to_string());
        }
        write_board_usb_only(state.inner(), &protocol::wifi_forget_payload(&ssid))
    }

    /// FR-15: forget every remembered network. USB only.
    #[tauri::command]
    fn wifi_saved_clear(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::wifi_clear_payload())
    }

    /// Provision the device license (UUID + AuthKey) on the board. The board
    /// persists it via tuya_authorize_write and reports back a license_status
    /// frame (surfaced as a 'license-status' event). The secret only travels
    /// board-ward; it is never stored by the companion or echoed back.
    #[tauri::command]
    fn send_license(state: State<Shared>, uuid: String, authkey: String) -> Result<(), String> {
        if uuid.trim().is_empty() || authkey.trim().is_empty() {
            return Err("UUID and AuthKey are both required".to_string());
        }
        write_board_usb_only(
            state.inner(),
            &protocol::license_payload(uuid.trim(), authkey.trim()),
        )
    }

    /// Write the Tuya product ID once. USB only. The keyboard accepts it only
    /// while it has no PID (never replaces or clears one), and applies it after
    /// a restart; the answer arrives as the 'pid-status' event. The PID is not
    /// kept by the Companion and not logged.
    #[tauri::command]
    fn send_pid(state: State<Shared>, pid: String) -> Result<(), String> {
        let pid = pid.trim();
        if !protocol::pid_is_valid(pid) {
            return Err("PID must be 16 characters of a-z and 0-9".to_string());
        }
        write_board_usb_only(state.inner(), &protocol::pid_payload(pid))
    }

    // ── Local firmware update over USB ───────────────────────

    /// Push transfer state to the UI when the state or the percent changes.
    fn emit_ota(app: &AppHandle, state: &Shared, progress: &crate::ota::Progress) {
        let percent = if progress.total == 0 { 0 } else { progress.acked * 100 / progress.total };
        let key = format!("{}:{percent}:{}", progress.state, progress.reason);
        let changed = state
            .inner
            .lock()
            .map(|mut inner| std::mem::replace(&mut inner.ota_last_emit, key.clone()) != key)
            .unwrap_or(true);
        if changed {
            let _ = app.emit("ota-progress", progress.clone());
            if progress.state == "failed" || progress.state == "installed" {
                log_line(app, format!("[OTA] {} {} {}", progress.state, progress.version, progress.reason));
            }
        }
    }

    /// OTA frames go out over USB only, in order; stop at the first error.
    fn write_ota_frames(app: &AppHandle, state: &Shared, frames: Vec<Value>) {
        for frame in frames {
            if let Err(e) = write_board_usb_only(state, &frame) {
                log_line(app, format!("[OTA] send failed: {e}"));
                break;
            }
        }
    }

    /// Check a picked `.akota` file (base64 of its bytes) and keep it ready.
    #[tauri::command]
    fn ota_load(state: State<Shared>, data_b64: String) -> Result<crate::ota::PackageInfo, String> {
        use base64::Engine as _;
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(data_b64.trim())
            .map_err(|_| "the file could not be read".to_string())?;
        let pkg = crate::ota::Package::parse(&bytes, &crate::ota::trusted_keys())?;
        let info = pkg.info.clone();
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        if inner.ota.as_ref().is_some_and(|s| s.active()) {
            return Err("an update is already running".to_string());
        }
        inner.ota_package = Some(pkg);
        Ok(info)
    }

    /// Send the loaded package; the keyboard then asks for K6 / K1.
    #[tauri::command]
    fn ota_start(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let shared = state.inner().clone();
        let (begin, progress) = {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            if inner.kind != TransportKind::Usb {
                return Err("firmware updates need the USB connection".to_string());
            }
            match inner.ota_device_key.as_str() {
                "" => return Err("this keyboard firmware cannot update over USB".to_string()),
                "none" => return Err("this keyboard firmware has no update key (placeholder build)".to_string()),
                _ => {}
            }
            if inner.ota.as_ref().is_some_and(|s| s.active()) {
                return Err("an update is already running".to_string());
            }
            let pkg = inner.ota_package.clone().ok_or("pick an update file first")?;
            if pkg.info.key.as_str() != inner.ota_device_key {
                return Err(format!(
                    "the file is signed with the {} key; this keyboard takes {} packages",
                    pkg.info.key.as_str(),
                    inner.ota_device_key
                ));
            }
            let (sender, begin) = crate::ota::Sender::start(pkg, now_ms());
            let progress = sender.progress();
            inner.ota = Some(sender);
            (begin, progress)
        };
        emit_ota(&app, &shared, &progress);
        write_board_usb_only(&shared, &begin)?;
        log_line(&app, format!("[OTA] sent {}; confirm with K6 on the keyboard", progress.version));
        spawn_ota_ticker(app, shared);
        Ok(())
    }

    #[tauri::command]
    fn ota_cancel(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let shared = state.inner().clone();
        let (abort, progress) = {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            let sender = inner.ota.as_mut().ok_or("no update is running")?;
            (sender.abort(), sender.progress())
        };
        emit_ota(&app, &shared, &progress);
        match abort {
            Some(frame) => write_board_usb_only(&shared, &frame),
            None => Err("the keyboard is already installing; do not unplug it".to_string()),
        }
    }

    /// Resend after a silent stall and greet the rebooted keyboard; ends with
    /// the transfer.
    fn spawn_ota_ticker(app: AppHandle, state: Shared) {
        let _ = thread::Builder::new().name("ota-ticker".to_string()).spawn(move || loop {
            thread::sleep(Duration::from_millis(500));
            let (frames, progress) = {
                let Ok(mut inner) = state.inner.lock() else { return };
                match inner.ota.as_mut() {
                    Some(s) if s.active() => (s.tick(now_ms()), s.progress()),
                    _ => return,
                }
            };
            emit_ota(&app, &state, &progress);
            write_ota_frames(&app, &state, frames);
        });
    }

    fn handle_ota_frame(app: &AppHandle, state: &Shared, frame: &Inbound) {
        let (frames, progress) = {
            let Ok(mut inner) = state.inner.lock() else { return };
            if let Inbound::OtaStatus(p) = frame {
                if p.state == "idle" || p.state == "installed" {
                    inner.ota_device_key = p.key.clone();
                }
            }
            let Some(sender) = inner.ota.as_mut() else { return };
            if !sender.active() {
                return;
            }
            let frames = match frame {
                Inbound::OtaStatus(p) => sender.on_status(&p.state, &p.reason, &p.fw, now_ms()),
                Inbound::OtaAck(a) => sender.on_ack(a.next as usize, now_ms()),
                _ => Vec::new(),
            };
            (frames, sender.progress())
        };
        emit_ota(app, state, &progress);
        write_ota_frames(app, state, frames);
    }

    /// Request development-safe License diagnostics. The board replies only on
    /// UART0 with the full UUID, AuthKey last four characters, and value source.
    #[tauri::command]
    fn read_license(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::license_read_payload())
    }

    /// One-click "enter pairing": clear the board's Wi-Fi/cloud binding and
    /// reboot it into provisioning. The board's License (UUID/AuthKey) is
    /// preserved — only Wi-Fi credentials and cloud binding are cleared. This is
    /// destructive (the board reboots and the link drops), so it travels only
    /// over the physically trusted USB/UART transport; the UI must confirm first.
    #[tauri::command]
    fn enter_pairing(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::reset_netcfg_payload())
    }

    /// Ask the keyboard (USB only, pairing window open on the device) for a new
    /// BLE pairing key. The key arrives as `ble_pair_key` and is stored by
    /// `ble_pairing`; only its id reaches the UI.
    #[tauri::command]
    fn ble_pair(state: State<Shared>) -> Result<(), String> {
        write_board_usb_only(state.inner(), &protocol::ble_pair_payload())
    }

    #[tauri::command]
    fn ble_pairing_status() -> ble_pairing::PairingStatus {
        ble_pairing::status()
    }

    /// The UI's answer channel of the over-the-air pairing in progress.
    static AIR_PAIR_ANSWER: std::sync::Mutex<Option<std::sync::mpsc::Sender<bool>>> = std::sync::Mutex::new(None);

    /// Over-the-air BLE pairing (no USB): run the exchange with `device_id`
    /// (Settings > BLE pairing must be open on the keyboard). Progress arrives
    /// as `ble-air-pair` events: `{state:"code", code}`, then `{state:"done",
    /// key_id}` or `{state:"failed", reason}`. The key never reaches the UI.
    #[tauri::command]
    fn ble_pair_air(app: AppHandle, device_id: String) -> Result<(), String> {
        if device_id.trim().is_empty() {
            return Err("scan and pick the keyboard first".into());
        }
        let (tx, rx) = std::sync::mpsc::channel::<bool>();
        {
            let mut slot = AIR_PAIR_ANSWER.lock().map_err(|_| "pairing state poisoned")?;
            if slot.is_some() {
                return Err("a Bluetooth pairing is already running".into());
            }
            *slot = Some(tx);
        }
        std::thread::Builder::new()
            .name("ble-air-pair".into())
            .spawn(move || {
                let code_app = app.clone();
                let result = ble_gatt::pair_over_air(
                    &device_id,
                    move |code| {
                        let _ = code_app.emit(
                            "ble-air-pair",
                            serde_json::json!({"state": "code", "code": ble_pair::format_code(code)}),
                        );
                    },
                    rx,
                )
                .and_then(|(key, key_id)| {
                    let saved = ble_pairing::save(&ble_sec::hex(&key), &key_id);
                    let mut key = key;
                    key.fill(0);
                    saved
                });
                if let Ok(mut slot) = AIR_PAIR_ANSWER.lock() {
                    *slot = None;
                }
                match result {
                    Ok(status) => {
                        log_line(&app, format!("[BLE] paired over Bluetooth with keyboard key {}", status.key_id));
                        let _ = app.emit("ble-air-pair", serde_json::json!({"state": "done", "key_id": status.key_id}));
                        let _ = app.emit("ble-pairing", status);
                    }
                    Err(reason) => {
                        log_line(&app, format!("[BLE] pairing over Bluetooth failed: {reason}"));
                        let _ = app.emit("ble-air-pair", serde_json::json!({"state": "failed", "reason": reason}));
                    }
                }
            })
            .map_err(|err| {
                if let Ok(mut slot) = AIR_PAIR_ANSWER.lock() {
                    *slot = None;
                }
                format!("spawn pairing thread failed: {err}")
            })?;
        Ok(())
    }

    /// The user's answer to the code shown during `ble_pair_air`.
    #[tauri::command]
    fn ble_pair_air_answer(confirm: bool) -> Result<(), String> {
        let slot = AIR_PAIR_ANSWER.lock().map_err(|_| "pairing state poisoned")?;
        slot.as_ref().ok_or("no Bluetooth pairing is running")?.send(confirm).map_err(|_| "pairing already ended".into())
    }

    /// Forget the key on this PC. The keyboard keeps its copy until it is
    /// cleared on the device (Settings > Credentials).
    #[tauri::command]
    fn ble_forget(app: AppHandle) -> Result<(), String> {
        ble_pairing::forget()?;
        let _ = app.emit("ble-pairing", ble_pairing::status());
        Ok(())
    }

    /// FP-5: choose which ASR handles a board voice event. `cloud` = board mic +
    /// Tuya cloud ASR (FP-3); `system` = activate the PC's own speech input here.
    /// Mirrors the choice to the board via the config frame so the LCD can show it.
    #[tauri::command]
    fn set_voice_source(state: State<Shared>, source: String) -> Result<(), String> {
        let src = VoiceSource::parse(&source);
        {
            let shared = state.inner();
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.voice_source = src;
        }
        // Best-effort mirror to the board (ignored if not connected).
        let _ = write_board(state.inner(), &protocol::config_payload(src.as_str()));
        Ok(())
    }

    // ───────────────────── FP-4b: Kiro Crew control (route B — "the hands") ─────────────────────

    /// route B: point the controller at a window whose title contains `title`
    /// (empty → default candidates "Kiro Crew" / "Agentic"). Verifies a match
    /// by focusing immediately, so the UI can show a bind result (AC1.5).
    #[tauri::command]
    fn kc_bind_window(state: State<Shared>, title: String) -> Result<(), String> {
        let mut controller = state.controller.lock().map_err(|_| "controller lock poisoned")?;
        controller.set_title_match(title);
        // Focus now to verify the window exists; bubbles up the "no match" Err.
        controller.focus()
    }

    /// route B: focus the bound window and type `text` (no command bar, no Enter).
    #[tauri::command]
    fn kc_send_text(state: State<Shared>, text: String) -> Result<(), String> {
        state
            .controller
            .lock()
            .map_err(|_| "controller lock poisoned")?
            .send_text(&text)
    }

    /// route B: focus, open the command bar (Ctrl+K), type `cmd`, press Enter.
    #[tauri::command]
    fn kc_command_bar(state: State<Shared>, cmd: String) -> Result<(), String> {
        state
            .controller
            .lock()
            .map_err(|_| "controller lock poisoned")?
            .command_bar(&cmd)
    }

    /// route B: focus and inject a single named chord
    /// (`enter`/`esc`/`up`/`down`/`left`/`right`/`ctrlk`).
    #[tauri::command]
    fn kc_chord(state: State<Shared>, name: String) -> Result<(), String> {
        let chord = Chord::parse(&name).ok_or_else(|| format!("unknown chord {name:?}"))?;
        state
            .controller
            .lock()
            .map_err(|_| "controller lock poisoned")?
            .chord(chord)
    }

    // ─────────────── FP-4b M4: per-key chat-number bind slots (§3.2) ───────────────

    /// M4: return the three per-key chat NUMBERS (index 0 = K1 … index 2 = K3)
    /// as a JSON array of `u8` for the UI.
    #[tauri::command]
    fn kc_bind_get(state: State<Shared>) -> Result<Vec<u8>, String> {
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_binds
            .chats
            .clone())
    }

    /// Return optional display aliases for slots A, B, and C. Empty entries use
    /// the default letter on the AK.
    #[tauri::command]
    fn kc_bind_aliases_get(state: State<Shared>) -> Result<Vec<String>, String> {
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_binds
            .aliases
            .clone())
    }

    /// Return the Crew session associated with each K1-K3 slot. Empty entries
    /// have not been quick-bound from Watched Chat yet.
    #[tauri::command]
    fn kc_bind_sessions_get(state: State<Shared>) -> Result<Vec<String>, String> {
        // Local sessions by slot key (as the Crew API lists them), remote ones
        // by row identity.
        Ok(state.inner.lock().map_err(|_| "state lock poisoned")?.kc_binds.status_ids())
    }

    /// Persist one slot alias and immediately synchronize all aliases to the AK
    /// when connected. Saving while offline still succeeds; connect_device sends
    /// the stored values on the next connection.
    #[tauri::command]
    fn kc_bind_alias_set(
        state: State<Shared>,
        slot: u8,
        alias: String,
    ) -> Result<Vec<String>, String> {
        let (aliases, frame) = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_binds.set_alias(slot, &alias)?;
            inner.kc_binds.save()?;
            (inner.kc_binds.aliases.clone(), inner.bindings_frame())
        };
        let _ = write_board(state.inner(), &frame);
        Ok(aliases)
    }

    /// M4: bind slot `slot` (1-based) to chat `number` (`1..=9`). Validates the
    /// number, persists the store, and updates the LIVE router `ControlCtx` so
    /// the next board press jumps to the new number without a restart.
    #[tauri::command]
    fn kc_bind_set(state: State<Shared>, slot: u8, number: u8) -> Result<(), String> {
        // 0 = not set (no Alt+N jump); 1..=9 = jump to that chat. Reject >9.
        if number > 9 {
            return Err(format!("chat number must be 0..=9 (got {number})"));
        }
        let frame = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_binds.set(slot, number);
            inner.kc_binds.save()?;
            // Keep the live router context in sync with the persisted numbers.
            inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
            inner.bindings_frame() // a slot's voice input mode follows its Alt+N number
        };
        let _ = write_board(state.inner(), &frame);
        Ok(())
    }

    /// Quick-bind one observed Crew session and its real dashboard Chat # to a
    /// K1-K3 slot atomically. The session id is presentation metadata; the live
    /// router continues to inject Alt+`number` as before.
    #[tauri::command]
    fn kc_bind_session_set(
        state: State<Shared>,
        slot: u8,
        number: u8,
        session_id: String,
    ) -> Result<(), String> {
        // 0 = not set (no Alt+N jump); 1..=9 = jump to that chat. Reject >9.
        if number > 9 {
            return Err(format!("chat number must be 0..=9 (got {number})"));
        }
        let frame = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            let key = session_id.trim().to_string();
            let name = inner.crew_api_slots.iter().find(|s| s.key == key).map(|s| s.title.clone()).unwrap_or_default();
            inner.kc_binds.set_session_named(slot, &session_id, &name)?;
            inner.kc_binds.set(slot, number);
            inner.kc_binds.save()?;
            inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
            inner.bindings_frame()
        };
        let _ = write_board(state.inner(), &frame);
        Ok(())
    }

    /// Remove only a slot's Watched Chat association, preserving its Chat # and
    /// AK alias.
    #[tauri::command]
    fn kc_bind_session_clear(state: State<Shared>, slot: u8) -> Result<(), String> {
        let (alias, frame) = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_binds.clear_session(slot)?;
            inner.kc_binds.save()?;
            // Alias for the now-unbound slot (empty falls back to A/B/C on AK).
            (
                inner.kc_binds.aliases.get((slot as usize).saturating_sub(1)).cloned().unwrap_or_default(),
                inner.bindings_frame(),
            )
        };
        let _ = write_board(state.inner(), &frame);
        // Tell the board this slot is unbound so its sidebar icon goes purple.
        // Best-effort: ignored when the board is offline or slot is out of 1..=3.
        if (1..=3).contains(&slot) {
            let _ = write_board(
                state.inner(),
                &protocol::display_payload(slot, &alias, "unbound", ""),
            );
        }
        Ok(())
    }

    /// M4: reset slot `slot` (1-based) to its default chat number (slot index+1:
    /// K1->1, K2->2, K3->3), persist, and update the live router context.
    #[tauri::command]
    fn kc_bind_clear(state: State<Shared>, slot: u8) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.clear(slot);
        inner.kc_binds.save()?;
        inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
        Ok(())
    }

    // ─────────────── FP-4b: STATUS sink ("the eyes") — T7 wiring ───────────────


    /// Apply one event from the Crew REST/WebSocket client. API status enters
    /// the same `apply_status` path as Hook events, so existing Watched Chat,
    /// Chat A/B/C bindings, UI events, and LCD forwarding continue to work.
    /// Hook remains live and can still update status whenever API is offline.
    fn apply_crew_event(app: &AppHandle, state: &Shared, event: CrewEvent) {
        match event {
            CrewEvent::Connection(connection) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.crew_api_connected = connection.connected;
                    inner.crew_api_base_url = connection.base_url.clone();
                    inner.crew_api_last_error = connection.error.clone();
                    if connection.connected {
                        // Treat the first slots frame on every WebSocket
                        // connection as a full projection, even when its data
                        // matches the snapshot cached before reconnecting.
                        inner.crew_api_slots.clear();
                    }
                }
                let _ = app.emit("crew-api-connection", &connection);
                // CP-09: tell the keyboard once when the identity check starts
                // failing (the reconnect loop retries every few seconds).
                let identity_failed = connection.error.starts_with(crate::crew_identity::IDENTITY_FAILED);
                let first_failure = state
                    .inner
                    .lock()
                    .map(|mut inner| {
                        let first = identity_failed && !inner.crew_identity_failed;
                        inner.crew_identity_failed = identity_failed;
                        first
                    })
                    .unwrap_or(false);
                if first_failure {
                    write_menu_frames(
                        app,
                        state,
                        vec![notice_payload(
                            crate::lcd_text::lcd(crate::crew_identity::IDENTITY_FAILED),
                            NoticeLevel::Error,
                        )],
                    );
                }
                if connection.connected {
                    log_line(app, format!("[CREW-API] connected {}", connection.base_url));
                } else {
                    log_line(
                        app,
                        format!(
                            "[CREW-API] disconnected: {} · retry {}s · Hook fallback remains active",
                            connection.error, connection.retry_secs
                        ),
                    );
                }
            }
            CrewEvent::Slots(slots) => {
                // Only feed changed projections into apply_status; the gateway
                // periodically rebroadcasts the full list and replaying every
                // unchanged idle row would spam logs/LCD writes.
                let changed = if let Ok(mut inner) = state.inner.lock() {
                    let previous: HashMap<_, _> = inner
                        .crew_api_slots
                        .iter()
                        .map(|slot| (slot.key.as_str(), slot))
                        .collect();
                    let changed = slots
                        .iter()
                        .filter(|slot| previous.get(slot.key.as_str()).copied() != Some(slot))
                        .cloned()
                        .collect::<Vec<_>>();
                    inner.crew_api_slots = slots.clone();
                    changed
                } else {
                    Vec::new()
                };
                let _ = app.emit("crew-api-slots", &slots);
                for slot in changed {
                    apply_status(app, state, slot_display_status(&slot));
                }
                let fetch = state
                    .inner
                    .lock()
                    .map(|mut inner| {
                        sync_menu_scope(&mut inner);
                        inner.menus.update_slots(&slots)
                    })
                    .unwrap_or_default();
                for slot in fetch {
                    fetch_questions(app, state, slot);
                }
                refresh_menus(app, state);
            }
            CrewEvent::Followup(card) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.menus.add_followup(card, now_ms());
                }
                refresh_menus(app, state);
            }
            CrewEvent::ChatMessage(data) => {
                let _ = app.emit("crew-api-message", data);
            }
            CrewEvent::Dashboard(data) => {
                let _ = app.emit("crew-api-dashboard", data);
            }
            CrewEvent::SlotTitle(data) => {
                let _ = app.emit("crew-api-slot-title", data);
            }
            CrewEvent::Other { event_type, data } => {
                if event_type == "decode_error" {
                    log_line(app, format!("[CREW-API] WebSocket decode warning: {data}"));
                }
                // A question card appeared or was answered elsewhere: refetch
                // that chat's pending cards (one-shot frames are not trusted
                // as the source of truth).
                if event_type == "question_card" || event_type == "question_card_resolved" {
                    let slot = data.get("slot").and_then(Value::as_str).map(str::to_string);
                    let targets = match slot {
                        Some(s) => vec![s],
                        None => state
                            .inner
                            .lock()
                            .map(|inner| inner.menus.slots_needing_questions())
                            .unwrap_or_default(),
                    };
                    for slot in targets {
                        fetch_questions(app, state, slot);
                    }
                }
            }
        }
    }

    /// Monotonic milliseconds since the first call (menu expiry clock).
    fn now_ms() -> u64 {
        static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
        START.get_or_init(Instant::now).elapsed().as_millis() as u64
    }

    /// Menu scope = K1..K3 bound chats + the current chat; dictated answers
    /// need the keyboard's cloud ASR.
    fn sync_menu_scope(inner: &mut Inner) {
        let bound = inner.kc_binds.status_ids();
        let current = inner.crew_api_active_slot.clone();
        inner.menus.set_scope(&bound, current.as_deref());
        let voice_ok = inner.voice_source == VoiceSource::Cloud;
        inner.menus.set_voice_ok(voice_ok);
    }

    /// Write menu frames to the keyboard (only one that advertised `menu`) and
    /// mirror the current menu to the UI (read-only, for debugging).
    fn write_menu_frames(app: &AppHandle, state: &Shared, frames: Vec<Value>) {
        if frames.is_empty() {
            return;
        }
        let (capable, mirror) = state
            .inner
            .lock()
            .map(|inner| (inner.menu_capable, inner.menus.active_frames()))
            .unwrap_or((false, None));
        if capable {
            for frame in &frames {
                if let Err(e) = write_board(state, frame) {
                    log_line(app, format!("[MENU] write failed: {e}"));
                    break;
                }
            }
        }
        let _ = app.emit(
            "board-menu",
            serde_json::json!({
                "capable": capable,
                "active": mirror.map(|(id, pages)| serde_json::json!({ "id": id, "pages": pages })),
            }),
        );
    }

    fn refresh_menus(app: &AppHandle, state: &Shared) {
        let frames = state
            .inner
            .lock()
            .map(|mut inner| {
                sync_menu_scope(&mut inner);
                inner.menus.reconcile(now_ms())
            })
            .unwrap_or_default();
        write_menu_frames(app, state, frames);
    }

    fn crew_client(state: &Shared) -> Option<CrewApiClient> {
        state.inner.lock().ok().and_then(|inner| inner.crew_api.clone())
    }

    fn fetch_questions(app: &AppHandle, state: &Shared, slot: String) {
        let Some(client) = crew_client(state) else { return };
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || match client.slot_questions(&slot) {
            Ok(cards) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.menus.set_questions(&slot, cards);
                }
                refresh_menus(&app, &state);
            }
            Err(e) => log_line(&app, format!("[MENU] question cards for {slot}: {e}")),
        });
    }

    /// Run a validated menu answer against Crew off the board-reader thread,
    /// then close the menu with a short status and show the next one.
    fn run_menu_outcome(app: &AppHandle, state: &Shared, id: String, outcome: crew_menus::Outcome) {
        let Some(client) = crew_client(state) else {
            let frames = state
                .inner
                .lock()
                .map(|mut inner| inner.menus.finish(&id, &outcome, Err(crate::lcd_text::lcd("Crew offline")), now_ms()))
                .unwrap_or_default();
            write_menu_frames(app, state, frames);
            return;
        };
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || {
            let previous_model = match &outcome {
                crew_menus::Outcome::SetModel { slot, .. } => {
                    state.inner.lock().map(|inner| inner.menus.slot_model(slot)).unwrap_or_default()
                }
                _ => String::new(),
            };
            let result = crew_menus::run_outcome(&client, &outcome);
            // Log the kind and chat only, never the answer text.
            log_line(&app, format!("[MENU] {} -> {}", crew_menus::describe_outcome(&outcome), match &result {
                Ok(_) => "ok".to_string(),
                Err(e) => e.clone(),
            }));
            let frames = state
                .inner
                .lock()
                .map(|mut inner| {
                    if let (Ok(_), crew_menus::Outcome::SetModel { slot, model }) = (&result, &outcome) {
                        remember_model(&mut inner, slot, &previous_model, model);
                    }
                    let now = now_ms();
                    match &result {
                        Ok(deferred) => {
                            let msg = crew_menus::success_message(&outcome, *deferred);
                            inner.menus.finish(&id, &outcome, Ok(msg), now)
                        }
                        Err(e) => inner.menus.finish(&id, &outcome, Err(e.as_str()), now),
                    }
                })
                .unwrap_or_default();
            write_menu_frames(&app, &state, frames);
        });
    }

    /// Record a model change for the Auto toggle and persist the chat's last
    /// hand-picked model to the Companion config file (`binds.json`).
    fn remember_model(inner: &mut Inner, slot: &str, previous: &str, chosen: &str) {
        if let Some(model) = inner.menus.note_model_change(slot, previous, chosen) {
            let mut next = inner.kc_binds.clone();
            if next.set_last_model(slot, &model).is_ok() && next.save().is_ok() {
                inner.kc_binds = next;
            }
        }
    }

    /// K6 long (`model_menu`): fetch the model list, then open the menu for
    /// the current chat.
    fn open_model_menu(app: &AppHandle, state: &Shared) {
        let (client, slot) = match state.inner.lock() {
            Ok(inner) => (inner.crew_api.clone(), inner.crew_api_active_slot.clone()),
            Err(_) => return,
        };
        let (Some(client), Some(slot)) = (client, slot) else {
            write_menu_frames(app, state, vec![notice_payload(crate::lcd_text::lcd("Select a chat first"), NoticeLevel::Warn)]);
            return;
        };
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || {
            let frames = match client.list_models() {
                Ok(models) => state
                    .inner
                    .lock()
                    .map(|mut inner| inner.menus.open_model_menu(&slot, &models, now_ms()))
                    .unwrap_or_default(),
                Err(e) => {
                    log_line(&app, format!("[MENU] model list: {e}"));
                    vec![notice_payload(crate::lcd_text::lcd("Model list unavailable"), NoticeLevel::Error)]
                }
            };
            write_menu_frames(&app, &state, frames);
        });
    }

    /// K6 double (`model_toggle`): Auto <-> last hand-picked model.
    fn toggle_model(app: &AppHandle, state: &Shared) {
        let picked = state.inner.lock().ok().map(|inner| {
            let slot = inner.crew_api_active_slot.clone();
            let target = slot.as_deref().map(|s| inner.menus.toggle_target(s));
            (inner.crew_api.clone(), slot, target)
        });
        let Some((Some(client), Some(slot), Some(target))) = picked else {
            write_menu_frames(app, state, vec![notice_payload(crate::lcd_text::lcd("Select a chat first"), NoticeLevel::Warn)]);
            return;
        };
        let model = match target {
            Ok(m) => m,
            Err(msg) => {
                write_menu_frames(app, state, vec![notice_payload(msg, NoticeLevel::Warn)]);
                return;
            }
        };
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || {
            let previous = state.inner.lock().map(|inner| inner.menus.slot_model(&slot)).unwrap_or_default();
            let notice = match client.set_model(&slot, &model) {
                Ok(receipt) => {
                    if let Ok(mut inner) = state.inner.lock() {
                        remember_model(&mut inner, &slot, &previous, &model);
                    }
                    let name = if model == "auto" { crate::lcd_text::lcd("Auto") } else { model.as_str() };
                    let when = if receipt.deferred { crate::lcd_text::lcd(" (next turn)") } else { "" };
                    notice_payload(&format!("{}{name}{when}", crate::lcd_text::lcd("Model: ")), NoticeLevel::Info)
                }
                Err(e) => {
                    log_line(&app, format!("[MENU] model toggle: {e}"));
                    notice_payload(crate::lcd_text::lcd("Model change failed"), NoticeLevel::Error)
                }
            };
            write_menu_frames(&app, &state, vec![notice]);
        });
    }

    /// Board frames that belong to the LCD menu. Returns true when consumed.
    fn handle_menu_frame(app: &AppHandle, state: &Shared, frame: &Inbound) -> bool {
        match frame {
            Inbound::BindSession { payload, .. } => {
                start_bind_flow(app, state, payload.slot);
                true
            }
            Inbound::MenuResult { payload, .. } if on_bind_result(app, state, payload) => true,
            Inbound::MenuResult { payload, .. } => {
                let reply = state
                    .inner
                    .lock()
                    .map(|mut inner| inner.menus.on_result(payload, now_ms()))
                    .unwrap_or_default();
                write_menu_frames(app, state, reply.frames);
                if reply.front_crew {
                    // "Show me the diff"-type choice: the content is read in
                    // the Crew window, never on the keyboard.
                    let fronted = state
                        .controller
                        .lock()
                        .map_err(|_| "controller lock poisoned".to_string())
                        .and_then(|mut controller| controller.bring_to_front());
                    if let Err(e) = fronted {
                        log_line(app, format!("[MENU] bring Crew window to front failed: {e}"));
                    }
                }
                if let Some((id, outcome)) = reply.outcome {
                    run_menu_outcome(app, state, id, outcome);
                }
                true
            }
            Inbound::Key { payload, .. } if payload.bind == Some(KeyAction::MenuRecall) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.menus.recall();
                }
                refresh_menus(app, state);
                true
            }
            Inbound::Key { payload, .. } if payload.bind == Some(KeyAction::ModelMenu) => {
                open_model_menu(app, state);
                true
            }
            Inbound::Key { payload, .. } if payload.bind == Some(KeyAction::ModelToggle) => {
                toggle_model(app, state);
                true
            }
            _ => false,
        }
    }


    // ─────────────── Settings > Bind session (bind_flow) ───────────────

    fn bind_seed() -> u64 {
        std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0)
    }

    /// Sessions of `instance` (`local` or a remote crew id) for the menu.
    fn bind_list(client: &CrewApiClient, instance: &str) -> Result<Vec<CrewSlot>, String> {
        if instance == crate::bind_store::LOCAL_INSTANCE {
            client.list_slots()
        } else {
            client.list_remote_slots(instance)
        }
    }

    fn write_frames(state: &Shared, frames: &[Value]) {
        for f in frames {
            if write_board(state, f).is_err() {
                break;
            }
        }
    }

    /// End the flow: menus may show again, the keyboard gets `bind_done`.
    fn end_bind_flow(app: &AppHandle, state: &Shared, slot: u8, result: &str, msg: &str) {
        if let Ok(mut inner) = state.inner.lock() {
            inner.bind_flow = None;
            inner.menus.set_hold(false);
        }
        let _ = write_board(state, &protocol::bind_done_payload(slot, result, msg));
        log_line(app, format!("[BIND] slot {slot}: {result}"));
        refresh_menus(app, state);
    }

    /// The slot's current row identity ("" = not bound).
    fn bound_row(inner: &Inner, slot: u8) -> String {
        inner.kc_binds.sessions.get((slot as usize).saturating_sub(1)).cloned().unwrap_or_default()
    }

    /// `bind_session {slot}`: first menu of the flow (instances only when
    /// remote crews exist), or `bind_done error`.
    fn start_bind_flow(app: &AppHandle, state: &Shared, slot: u8) {
        let Some(client) = crew_client(state) else {
            let _ = write_board(state, &protocol::bind_done_payload(slot, "error", crate::lcd_text::lcd("Crew offline")));
            return;
        };
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || {
            let instances = client.list_instances().unwrap_or_else(|e| {
                log_line(&app, format!("[BIND] remote crews unavailable: {e}"));
                Vec::new()
            });
            let mut flow = crate::bind_flow::BindFlow::new(slot, bind_seed(), crate::lcd_text::board_zh(), instances);
            let (bound, show_workers) =
                state.inner.lock().map(|inner| (bound_row(&inner, slot), inner.kc_binds.show_workers)).unwrap_or_default();
            let frames = if flow.needs_instance() {
                flow.instance_menu(&bound)
            } else {
                match bind_list(&client, crate::bind_store::LOCAL_INSTANCE) {
                    Ok(rows) => flow.session_menu(crate::bind_store::LOCAL_INSTANCE, &rows, show_workers, &bound),
                    Err(e) => {
                        log_line(&app, format!("[BIND] session list: {e}"));
                        let _ = write_board(&state, &protocol::bind_done_payload(slot, "error", crate::lcd_text::lcd("Session list unavailable")));
                        return;
                    }
                }
            };
            if let Ok(mut inner) = state.inner.lock() {
                inner.bind_flow = Some(flow);
                inner.menus.set_hold(true);
            }
            write_frames(&state, &frames);
            log_line(&app, format!("[BIND] slot {slot}: menu sent"));
        });
    }

    /// A menu_result for the flow's menu. False when the flow does not own it.
    fn on_bind_result(app: &AppHandle, state: &Shared, payload: &crate::protocol::MenuResultPayload) -> bool {
        use crate::bind_flow::Next;
        let step = state.inner.lock().ok().and_then(|mut inner| {
            let slot = inner.bind_flow.as_ref()?.slot;
            let bound = bound_row(&inner, slot);
            let next = inner.bind_flow.as_mut()?.on_result(payload, &bound);
            Some((slot, next, inner.kc_binds.show_workers))
        });
        let Some((slot, next, show_workers)) = step else { return false };
        match next {
            Next::NotOurs => return false,
            Next::Frames(frames) => write_frames(state, &frames),
            Next::Done(result) => end_bind_flow(app, state, slot, result, ""),
            Next::ListSessions { instance } => {
                let Some(client) = crew_client(state) else {
                    end_bind_flow(app, state, slot, "error", crate::lcd_text::lcd("Crew offline"));
                    return true;
                };
                let (app, state) = (app.clone(), state.clone());
                thread::spawn(move || match bind_list(&client, &instance) {
                    Ok(rows) => {
                        let frames = state.inner.lock().ok().and_then(|mut inner| {
                            let bound = bound_row(&inner, slot);
                            let flow = inner.bind_flow.as_mut().filter(|f| f.slot == slot)?;
                            Some(flow.session_menu(&instance, &rows, show_workers, &bound))
                        });
                        write_frames(&state, &frames.unwrap_or_default());
                    }
                    Err(e) => {
                        log_line(&app, format!("[BIND] session list of {instance}: {e}"));
                        end_bind_flow(&app, &state, slot, "error", crate::lcd_text::lcd("Session list unavailable"));
                    }
                });
            }
            Next::Bind { row, name } => {
                let saved = state.inner.lock().map_err(|_| "state lock poisoned".to_string()).and_then(|mut inner| {
                    let mut next = inner.kc_binds.clone();
                    next.set_session_named(slot, &row, &name)?;
                    next.save()?;
                    inner.kc_binds = next;
                    Ok(())
                });
                match saved {
                    Ok(()) => {
                        after_binding_change(app, state, slot);
                        end_bind_flow(app, state, slot, "bound", &format!("{} {name}", crate::lcd_text::lcd("Bound")));
                    }
                    Err(e) => {
                        log_line(app, format!("[BIND] save failed: {e}"));
                        end_bind_flow(app, state, slot, "error", crate::lcd_text::lcd("Binding not saved"));
                    }
                }
            }
            Next::Unbind => {
                let saved = state.inner.lock().map_err(|_| "state lock poisoned".to_string()).and_then(|mut inner| {
                    let mut next = inner.kc_binds.clone();
                    next.clear_session(slot)?;
                    next.save()?;
                    inner.kc_binds = next;
                    Ok(())
                });
                if let Err(e) = saved {
                    log_line(app, format!("[BIND] save failed: {e}"));
                    end_bind_flow(app, state, slot, "error", crate::lcd_text::lcd("Binding not saved"));
                } else {
                    after_binding_change(app, state, slot);
                    end_bind_flow(app, state, slot, "unbound", crate::lcd_text::lcd("Unbound"));
                }
            }
        }
        true
    }

    /// A slot's binding changed on the keyboard: names to the keyboard, the
    /// slot's status colour, and the UI.
    fn after_binding_change(app: &AppHandle, state: &Shared, slot: u8) {
        let frames = state.inner.lock().ok().map(|inner| {
            let b = &inner.kc_binds;
            let i = (slot as usize).saturating_sub(1);
            let id = b.status_ids().get(i).cloned().unwrap_or_default();
            let alias = b.aliases.get(i).cloned().unwrap_or_default();
            let display = if id.is_empty() {
                protocol::display_payload(slot, &alias, "unbound", "")
            } else if let Some(st) = inner.kc_status.get(&id) {
                protocol::display_payload(slot, &st.agent_name, &st.state, &st.text)
            } else if let Some(row) = inner.crew_api_slots.iter().find(|s| s.key == id) {
                let st = slot_display_status(row);
                protocol::display_payload(slot, &st.agent_name, &st.state, &st.text)
            } else {
                protocol::display_payload(slot, &alias, "idle", "")
            };
            vec![inner.bindings_frame(), display]
        });
        write_frames(state, &frames.unwrap_or_default());
        let _ = app.emit("kc-binds-changed", slot);
    }

    /// Remote crews' bound sessions: status lights only (v1). Every 5 s, one
    /// `chat-slots` read per remote crew that has a bound session, mapped
    /// into the same status path as local sessions (id = row identity).
    fn spawn_remote_status_poll(app: &AppHandle, state: &Shared) {
        let (app, state) = (app.clone(), state.clone());
        thread::spawn(move || loop {
            thread::sleep(Duration::from_secs(5));
            let job = state.inner.lock().ok().and_then(|inner| {
                let client = inner.crew_api.clone().filter(|_| inner.crew_api_connected)?;
                let mut remote: Vec<String> = inner
                    .kc_binds
                    .sessions
                    .iter()
                    .filter_map(|row| crate::bind_store::split_row(row))
                    .filter(|(inst, _)| *inst != crate::bind_store::LOCAL_INSTANCE)
                    .map(|(inst, _)| inst.to_string())
                    .collect();
                remote.sort();
                remote.dedup();
                (!remote.is_empty()).then_some((client, remote))
            });
            let Some((client, remote)) = job else { continue };
            for inst in remote {
                match client.list_remote_slots(&inst) {
                    Ok(rows) => {
                        let bound = state.inner.lock().map(|inner| inner.kc_binds.sessions.clone()).unwrap_or_default();
                        for row in rows {
                            let id = format!("{inst}:{}", row.key);
                            if bound.contains(&id) {
                                let mut st = slot_display_status(&row);
                                st.session_id = id;
                                apply_status(&app, &state, st);
                            }
                        }
                    }
                    Err(e) => log_line(&app, format!("[BIND] remote status of {inst}: {e}")),
                }
            }
        });
    }

    fn start_crew_realtime(app: &AppHandle, state: &Shared) -> Result<(), String> {
        let client = {
            let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            if inner.crew_realtime.is_some() {
                return Ok(());
            }
            inner
                .crew_api
                .clone()
                .ok_or_else(|| "Kiro Crew API client is unavailable".to_string())?
        };
        let app_cb = app.clone();
        let state_cb = state.clone();
        let worker = CrewRealtime::start(client, move |event| {
            apply_crew_event(&app_cb, &state_cb, event);
        })?;
        state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .crew_realtime = Some(worker);
        Ok(())
    }

    /// Apply one mapped status update coming from the hook sink (runs on the
    /// sink's listener thread, so it takes the shared state + app handle by
    /// value/clone). Keeps last-status per `session_id`, emits a `display` frame
    /// to the board (best-effort — ignored when the board is offline), and
    /// emits a `kc-status` UI event.
    fn apply_status(app: &AppHandle, state: &Shared, status: StatusDisplay) {
        // Record last-status per session, then resolve its persisted Chat A/B/C
        // destination under the same lock as the Watch filter.
        let (watch_allows, slots) = if let Ok(mut inner) = state.inner.lock() {
            let pinned = inner.kc_pinned_session.clone();
            let bound = inner.kc_binds.status_ids();
            crate::status_sink::insert_capped(
                &mut inner.kc_status,
                status.session_id.clone(),
                status.clone(),
                crate::status_sink::MAX_TRACKED_SESSIONS,
                |sid| pinned.as_deref() == Some(sid) || bound.iter().any(|b| b.trim() == sid),
            );
            let watch_allows = crate::status_sink::should_forward(
                inner.kc_pinned_session.as_deref(),
                &status.session_id,
            );
            let slots = inner.kc_binds.slots_for_session(&status.session_id);
            (watch_allows, slots)
        } else {
            // Without a trustworthy binding lookup, fail closed rather than
            // writing this status into whichever Chat is currently selected.
            (false, Vec::new())
        };
        // One Crew session may intentionally be assigned to multiple slots.
        // Unbound sessions remain visible in the Companion but never mutate AK.
        if watch_allows {
            for chat_slot in &slots {
                let _ = write_board(
                    state,
                    &protocol::display_payload(
                        *chat_slot,
                        &status.agent_name,
                        &status.state,
                        &status.text,
                    ),
                );
            }
        }
        let forwarded = watch_allows && !slots.is_empty();
        // Push to the UI live status list ALWAYS (pinned or not) so the picker
        // can list every active session; carry `forwarded` so the UI can mark
        // which session is currently driving the board.
        let _ = app.emit(
            "kc-status",
            serde_json::json!({
                "session_id": status.session_id,
                "agent_name": status.agent_name,
                "state": status.state,
                "text": status.text,
                "event": status.event,
                "detail_text": status.detail_text,
                "forwarded": forwarded,
            }),
        );
        log_line(
            app,
            format!(
                "[KC-STATUS] {} · {} · {}{}",
                status.session_id,
                status.state,
                status.text,
                if !watch_allows {
                    " (muted — not watched)"
                } else if slots.is_empty() {
                    " (not sent — no Chat A/B/C binding)"
                } else {
                    ""
                }
            ),
        );
    }

    /// Start the localhost status sink on `port` (default when omitted). Wires a
    /// callback that forwards each mapped status through [`apply_status`]. The
    /// sink generates a token and writes the `{port, token}` handoff file the
    /// `--hook` mode reads. Returns the token so the UI can show the exact hook
    /// command to paste.
    #[tauri::command]
    fn status_sink_start(
        app: AppHandle,
        state: State<Shared>,
        port: Option<u16>,
    ) -> Result<String, String> {
        let port = port.unwrap_or(DEFAULT_STATUS_PORT);
        // Tear down any existing sink first (start is idempotent-ish).
        {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            if let Some(mut existing) = inner.status_sink.take() {
                existing.stop();
            }
        }
        let app_cb = app.clone();
        let shared_cb = state.inner().clone();
        let sink = StatusSink::start(port, move |status| {
            apply_status(&app_cb, &shared_cb, status);
        })?;
        let token = sink.token().to_string();
        let bound = sink.port();
        {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.status_sink = Some(sink);
            inner.status_token = token.clone();
        }
        log_line(&app, format!("[KC-STATUS] sink listening on 127.0.0.1:{bound}"));
        Ok(token)
    }

    /// Stop the status sink (releases the port, removes the handoff file).
    #[tauri::command]
    fn status_sink_stop(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        if let Some(mut sink) = inner.status_sink.take() {
            sink.stop();
        }
        inner.status_token.clear();
        drop(inner);
        log_line(&app, "[KC-STATUS] sink stopped");
        Ok(())
    }

    /// Return the current sink token (empty when no sink is running).
    #[tauri::command]
    fn status_token(state: State<Shared>) -> Result<String, String> {
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .status_token
            .clone())
    }

    // ─────────── FP-4b Route STATUS — "watched chat" pin (option A) ───────────

    /// One row for the UI's active-session picker: a session the sink has seen,
    /// with its last-known status and whether it is the one currently pinned.
    #[derive(Serialize)]
    struct SessionRow {
        session_id: String,
        agent_name: String,
        state: String,
        text: String,
        event: String,
        detail_text: String,
        pinned: bool,
    }

    /// List every session the sink has observed (with last status), so the UI
    /// can render a picker. The `pinned` flag marks which one is driving the
    /// board. When nothing is pinned, ALL sessions drive the board (and every
    /// row's `pinned` is false).
    #[tauri::command]
    fn kc_list_sessions(state: State<Shared>) -> Result<Vec<SessionRow>, String> {
        let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let pinned = inner.kc_pinned_session.clone();
        let mut rows: Vec<SessionRow> = inner
            .kc_status
            .values()
            .map(|s| SessionRow {
                session_id: s.session_id.clone(),
                agent_name: s.agent_name.clone(),
                state: s.state.clone(),
                text: s.text.clone(),
                event: s.event.clone(),
                detail_text: s.detail_text.clone(),
                pinned: pinned.as_deref() == Some(s.session_id.as_str()),
            })
            .collect();
        // Stable order: pinned first, then by session id.
        rows.sort_by(|a, b| b.pinned.cmp(&a.pinned).then(a.session_id.cmp(&b.session_id)));
        Ok(rows)
    }

    /// Return the currently pinned session id, or empty string when following
    /// all sessions.
    #[tauri::command]
    fn kc_get_pinned(state: State<Shared>) -> Result<String, String> {
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_pinned_session
            .clone()
            .unwrap_or_default())
    }

    /// Pin ONE session as the watched chat: only its status is forwarded to the
    /// board LCD. Immediately pushes that session's last-known status (if any)
    /// to the board so the LCD reflects the choice without waiting for the next
    /// hook event. An empty / whitespace id is treated as unpin.
    #[tauri::command]
    fn kc_pin_session(app: AppHandle, state: State<Shared>, session_id: String) -> Result<(), String> {
        let sid = session_id.trim().to_string();
        if sid.is_empty() {
            return kc_unpin_session(app, state);
        }
        // Persist the pin and resolve the pinned session's bound AK slots under
        // one lock, together with its last known status.
        let (last, slots) = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            let mut next_binds = inner.kc_binds.clone();
            next_binds.set_pinned_session(&sid)?;
            next_binds.save()?;
            let slots = next_binds.slots_for_session(&sid);
            inner.kc_binds = next_binds;
            inner.kc_pinned_session = Some(sid.clone());
            (inner.kc_status.get(&sid).cloned(), slots)
        };
        // Refresh only the pinned session's explicitly bound Chat slot(s).
        if let Some(s) = last {
            for chat_slot in slots {
                let _ = write_board(
                    &state,
                    &protocol::display_payload(chat_slot, &s.agent_name, &s.state, &s.text),
                );
            }
        }
        log_line(&app, format!("[KC-STATUS] pinned session {sid}"));
        Ok(())
    }

    /// Clear the pin: go back to following every session (all statuses drive the
    /// board again).
    #[tauri::command]
    fn kc_unpin_session(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let mut next_binds = inner.kc_binds.clone();
        next_binds.set_pinned_session("")?;
        next_binds.save()?;
        inner.kc_binds = next_binds;
        inner.kc_pinned_session = None;
        drop(inner);
        log_line(&app, "[KC-STATUS] unpinned — following all sessions");
        Ok(())
    }

    // ─────── FP-4b Route STATUS — one-click global hook install/remove ───────

    /// Report global Crew ScriptHookStore status without modifying anything.
    /// The hook command is always THIS executable (CP-07); the UI cannot pass
    /// a path.
    #[tauri::command]
    fn kc_hook_status() -> Result<hook_install::HookStatus, String> {
        hook_install::status(&hook_install::current_hook_command()?)
    }

    /// Install four lifecycle commands into Crew's shared hooks.json store.
    /// Crew must be restarted after an external file update so its in-memory
    /// ScriptHookStore reloads the entries.
    #[tauri::command]
    fn kc_hook_install(app: AppHandle) -> Result<hook_install::HookStatus, String> {
        let command = hook_install::current_hook_command()?;
        let status = hook_install::install(&command)?;
        log_line(
            &app,
            format!("[KC-HOOK] global hook installed · {}", status.hook_file),
        );
        Ok(status)
    }

    /// Remove only AK Companion entries from Crew's shared hooks.json store.
    #[tauri::command]
    fn kc_hook_remove(app: AppHandle) -> Result<hook_install::HookStatus, String> {
        let status = hook_install::remove(&hook_install::current_hook_command()?)?;
        log_line(
            &app,
            format!("[KC-HOOK] global hook removed · {}", status.hook_file),
        );
        Ok(status)
    }

    /// Open the Crew configuration directory containing hooks.json in Explorer.
    #[tauri::command]
    fn kc_open_hooks_folder() -> Result<String, String> {
        let dir = hook_install::hooks_dir().ok_or("cannot resolve Kiro Crew configuration directory")?;
        std::fs::create_dir_all(&dir)
            .map_err(|e| format!("create {}: {e}", dir.display()))?;
        std::process::Command::new("explorer.exe")
            .arg(&dir)
            .spawn()
            .map_err(|e| format!("open {} in Explorer: {e}", dir.display()))?;
        Ok(dir.display().to_string())
    }

    /// Compact human-readable description of a routed control action for logs.
    fn describe_control_action(action: &ControlAction) -> String {
        match action {
            ControlAction::CommandBar(cmd) => format!("command-bar {cmd:?}"),
            ControlAction::Chord(c) => format!("chord {c:?}"),
            ControlAction::ChordRepeat(c, n) => format!("chord {c:?} x{n}"),
            ControlAction::SendText(t) => format!("send-text ({} chars)", t.chars().count()),
            ControlAction::Compose(t) => format!("compose ({} chars)", t.chars().count()),
            ControlAction::None => "none".to_string(),
        }
    }

    /// Execute a routed control action against the controller facade. All
    /// actions (including K1-K3 `Chord(JumpChat(n))`) go straight through
    /// `apply()`; the router already resolved the per-key chat number, so there
    /// is no stateful slot special-casing left here.
    fn exec_control_action(
        _app: &AppHandle,
        state: &Shared,
        action: &ControlAction,
    ) -> Result<(), String> {
        state
            .controller
            .lock()
            .map_err(|_| "controller lock poisoned".to_string())
            .and_then(|mut controller| controller.apply(action))
    }

    fn spawn_board_reader(
        app: AppHandle,
        state: Shared,
        line_rx: Receiver<String>,
        connection_id: u64,
    ) {
        thread::spawn(move || {
            diagnostics::record(
                "INFO",
                "board-reader",
                &format!("started connection_id={connection_id}"),
            );
            while let Ok(line) = line_rx.recv() {
                // Stop if this connection has been superseded. Check the
                // transport presence and the connection id under their own
                // locks (taken one at a time, never nested).
                let transport_present = state
                    .transport
                    .lock()
                    .map(|t| t.is_some())
                    .unwrap_or(false);
                let id_matches = state
                    .inner
                    .lock()
                    .map(|inner| inner.connection_id == connection_id)
                    .unwrap_or(false);
                let live = transport_present && id_matches;
                if !live {
                    diagnostics::record("INFO", "board-reader", "stopped: connection superseded");
                    break;
                }
                if let Some(frame) = protocol::decode_line(&line) {
                    handle_frame(&app, &state, frame);
                } else {
                    diagnostics::record(
                        "WARN",
                        "board-reader",
                        &format!("discarded undecodable line bytes={}", line.len()),
                    );
                }
            }
            diagnostics::record("INFO", "board-reader", "channel closed");
            drop_lost_link(&app, &state, connection_id);
        });
    }

    fn emit_crew_draft(app: &AppHandle, slot: &str, text: &str) {
        let _ = app.emit(
            "crew-api-draft",
            serde_json::json!({ "slot": slot, "text": text }),
        );
    }

    /// API-first board control. Returns true when the frame was consumed by the
    /// stable-slot REST path. False means the caller should use the legacy
    /// focus/SendInput route as fallback.
    /// `(key, gesture)` of a K1-K3 gesture frame, for `[KEY-TIMING]`.
    fn slot_key_gesture(frame: &Inbound) -> Option<(&str, &'static str, u32, Option<u32>)> {
        let Inbound::Key { payload, .. } = frame else { return None };
        if !matches!(payload.id.as_str(), "k1" | "k2" | "k3") {
            return None;
        }
        let g = match payload.action {
            KeyAction::Single => "single",
            KeyAction::Double => "double",
            KeyAction::Long => "long",
            _ => return None,
        };
        let hold = (payload.action == KeyAction::Long).then_some(payload.held_ms);
        Some((payload.id.as_str(), g, payload.wait_ms, hold))
    }

    fn route_board_via_crew_api(app: &AppHandle, state: &Shared, frame: &Inbound, received: Instant) -> bool {
        let api_ready = state
            .inner
            .lock()
            .map(|inner| inner.control_enabled && inner.crew_api_connected)
            .unwrap_or(false);
        if !api_ready {
            return false;
        }

        match frame {
            Inbound::Key { payload, .. }
                if matches!(payload.id.as_str(), "k1" | "k2" | "k3") =>
            {
                // Which Chat A/B/C slot this gesture selects:
                //   bind chat1..chat3 -> that slot (the keymap is authoritative);
                //   no bind + single  -> the key's own slot (K1->A, K2->B, K3->C).
                // Any other gesture/bind (double, long, chat4+, instanceN, edits)
                // falls through to the router; an unbound long drives the UI's
                // long-press auto-bind via `board-event`.
                let index = match (payload.bind, payload.action) {
                    (Some(KeyAction::Chat(n)), _) if (1..=3).contains(&n) => (n - 1) as usize,
                    (None, KeyAction::Single) => match payload.id.as_str() {
                        "k1" => 0,
                        "k2" => 1,
                        _ => 2,
                    },
                    _ => return false,
                };
                // Resolve the stable slot and its optional Alt+N number under
                // one lock, then release before any window injection.
                let selected = state.inner.lock().ok().and_then(|mut inner| {
                    let key = inner.kc_binds.local_key(index)?;
                    let is_api_slot = !key.is_empty()
                        && inner.crew_api_slots.iter().any(|slot| slot.key == key);
                    if !is_api_slot {
                        return None;
                    }
                    inner.crew_api_active_slot = Some(key.clone());
                    // Optional Alt+N jump number for this slot (0 = not set).
                    let jump = inner.kc_binds.chats.get(index).copied().unwrap_or(0);
                    Some((key, jump, inner.kc_binds.focus_on_select))
                });
                if let Some((slot, jump, focus_on_select)) = selected {
                    let selected_at = Instant::now();
                    let _ = app.emit("crew-api-active-slot", slot.clone());
                    log_line(app, format!("[CREW-API] selected stable slot {slot}"));
                    // The scope changed (current chat); ESC'd requests stay
                    // parked until K4 recalls them.
                    refresh_menus(app, state);
                    let fetch = state.inner.lock().map(|inner| inner.menus.slots_needing_questions()).unwrap_or_default();
                    for s in fetch {
                        fetch_questions(app, state, s);
                    }
                    let menus_at = Instant::now();
                    // Bonus: if the user set an Alt+N for this slot, ALSO jump the
                    // Kiro Crew view there (focus the window + inject Alt+N). When
                    // unset (0) nothing is injected — selection only.
                    if (1..=9).contains(&jump) {
                        let action = ControlAction::Chord(Chord::JumpChat(jump));
                        match exec_control_action(app, state, &action) {
                            Ok(()) => log_line(app, format!("[CREW-API] jumped Kiro Crew view to Alt+{jump}")),
                            Err(e) => log_line(app, format!("[CREW-API] Alt+{jump} jump failed: {e}")),
                        }
                    } else if focus_on_select {
                        // Setting (default on): show the Crew window. Crew has no
                        // API to switch chats, so nothing is injected.
                        let fronted = state
                            .controller
                            .lock()
                            .map_err(|_| "controller lock poisoned".to_string())
                            .and_then(|mut controller| controller.bring_to_front());
                        if let Err(e) = fronted {
                            log_line(app, format!("[CREW-API] bring Crew window to front failed: {e}"));
                        }
                    }
                    // Selecting a slot is local: no Crew API request is made.
                    if let Some((key, gesture, window, hold)) = slot_key_gesture(frame) {
                        log_line(
                            app,
                            crate::kirocrew_control::timing_line(
                                key,
                                gesture,
                                "api",
                                window,
                                hold,
                                &[
                                    ("select", selected_at - received),
                                    ("menus", menus_at - selected_at),
                                    ("front", menus_at.elapsed()),
                                ],
                            ),
                        );
                    }
                    return true;
                }
                // A remote crew's session: status only in this version.
                let remote = state
                    .inner
                    .lock()
                    .map(|inner| inner.kc_binds.sessions.get(index).is_some_and(|row| !row.is_empty()) && inner.kc_binds.local_key(index).is_none())
                    .unwrap_or(false);
                if remote {
                    write_menu_frames(app, state, vec![notice_payload(crate::lcd_text::lcd("Remote chat: status only"), NoticeLevel::Info)]);
                    return true;
                }
                false
            }
            // ASR/macro text, K5, and composer edits use the same live
            // composer path. Fill-back types without sending; K5 injects Enter.
            Inbound::Asr { .. } | Inbound::Macro { .. } => false,
            Inbound::Key { payload, .. } if payload.id == "k5" => false,
            _ => false,
        }
    }

    // Kept for REST draft UI compatibility. Board fill-back and editing
    // intentionally bypass this store for the live composer.
    #[allow(dead_code)]
    fn append_crew_draft(app: &AppHandle, state: &Shared, text: &str) -> bool {
        if text.is_empty() {
            return true;
        }
        let updated = state.inner.lock().ok().and_then(|mut inner| {
            let slot = inner.crew_api_active_slot.clone()?;
            let draft = inner.crew_api_drafts.entry(slot.clone()).or_default();
            if !draft.is_empty() && !draft.ends_with(char::is_whitespace) {
                draft.push(' ');
            }
            draft.push_str(text);
            Some((slot, draft.clone()))
        });
        if let Some((slot, draft)) = updated {
            emit_crew_draft(app, &slot, &draft);
            log_line(
                app,
                format!("[CREW-API] draft updated for {slot} (content withheld from logs)"),
            );
            true
        } else {
            false
        }
    }

    #[allow(dead_code)]
    enum DraftEdit {
        Backspace,
        DeleteWord,
        Clear,
    }

    #[allow(dead_code)]
    fn edit_crew_draft(app: &AppHandle, state: &Shared, edit: DraftEdit) -> bool {
        let updated = state.inner.lock().ok().and_then(|mut inner| {
            let slot = inner.crew_api_active_slot.clone()?;
            let draft = inner.crew_api_drafts.entry(slot.clone()).or_default();
            match edit {
                DraftEdit::Backspace => {
                    draft.pop();
                }
                DraftEdit::DeleteWord => {
                    let trimmed = draft.trim_end().to_string();
                    let keep = trimmed
                        .char_indices()
                        .rev()
                        .find(|(_, ch)| ch.is_whitespace())
                        .map(|(index, _)| index)
                        .unwrap_or(0);
                    draft.truncate(keep);
                }
                DraftEdit::Clear => draft.clear(),
            }
            Some((slot, draft.clone()))
        });
        if let Some((slot, draft)) = updated {
            emit_crew_draft(app, &slot, &draft);
            true
        } else {
            false
        }
    }

    // ── FR-03 scenes (scene_sync.rs owns the logic) ──────────────────────

    /// Write scene frames to the keyboard; a failed write is logged (the next
    /// hello resyncs).
    fn scene_write(app: &AppHandle, state: &Shared, frames: &[Value]) {
        for f in frames {
            if let Err(e) = write_board(state, f) {
                log_line(app, format!("[SCENE] {} not sent: {e}", payload_kind(f)));
                break;
            }
        }
    }

    /// Push the scene view to the UI and keep the router's knob mode on the
    /// current scene.
    fn scene_emit(app: &AppHandle, state: &Shared) {
        let view = state.inner.lock().ok().map(|mut inner| {
            let knob = crate::kirocrew_control::KnobMode::parse(&inner.scenes.knob());
            inner.kc_router.ctx.knob = knob;
            inner.scenes.view()
        });
        if let Some(view) = view {
            let _ = app.emit("scenes-changed", view);
        }
    }

    fn scene_react(app: &AppHandle, state: &Shared, r: crate::scene_sync::Reaction) {
        scene_write(app, state, &r.frames);
        if let Some(msg) = &r.message {
            log_line(app, format!("[SCENE] {msg}"));
            let _ = app.emit("scene-notice", msg.clone());
        }
        if let Some(ms) = r.retry_after_ms {
            // The keyboard was busy (not in standby): ask again later. A new
            // hello or a cancel drops the pending switch, so retry() is empty.
            let app = app.clone();
            let state = state.clone();
            thread::spawn(move || {
                thread::sleep(Duration::from_millis(ms));
                let frames = state.inner.lock().map(|mut inner| inner.scenes.retry()).unwrap_or_default();
                scene_write(&app, &state, &frames);
            });
        }
        if r.changed || r.message.is_some() {
            scene_emit(app, state);
        }
    }

    /// Run a scene edit, write its frames, push the view.
    fn scene_cmd<T>(
        app: &AppHandle,
        state: &Shared,
        f: impl FnOnce(&mut crate::scene_sync::SceneManager) -> Result<(T, Vec<Value>), String>,
    ) -> Result<T, String> {
        let (out, frames) = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            f(&mut inner.scenes)?
        };
        scene_write(app, state, &frames);
        scene_emit(app, state);
        Ok(out)
    }

    #[tauri::command]
    fn scenes_list(state: State<Shared>) -> Result<crate::scene_sync::SceneView, String> {
        Ok(state.inner.lock().map_err(|_| "state lock poisoned")?.scenes.view())
    }

    #[tauri::command]
    fn scene_create(app: AppHandle, state: State<Shared>, scene: crate::scene::Scene) -> Result<String, String> {
        scene_cmd(&app, state.inner(), |m| m.create(scene))
    }

    #[tauri::command]
    fn scene_duplicate(app: AppHandle, state: State<Shared>, id: String) -> Result<String, String> {
        scene_cmd(&app, state.inner(), |m| m.duplicate(&id))
    }

    #[tauri::command]
    fn scene_update(app: AppHandle, state: State<Shared>, id: String, scene: crate::scene::Scene) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.update(&id, scene).map(|f| ((), f)))
    }

    #[tauri::command]
    fn scene_rename(app: AppHandle, state: State<Shared>, id: String, name: String) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.rename(&id, &name).map(|f| ((), f)))
    }

    #[tauri::command]
    fn scene_delete(app: AppHandle, state: State<Shared>, id: String) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.delete(&id).map(|f| ((), f)))
    }

    #[tauri::command]
    fn scene_set_favorites(app: AppHandle, state: State<Shared>, ids: Vec<String>) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.set_favorites(&ids).map(|f| ((), f)))
    }

    /// Import a scene file's text (the UI reads the file the user picked).
    /// All or nothing; returns the new ids.
    #[tauri::command]
    fn scene_import(app: AppHandle, state: State<Shared>, text: String) -> Result<Vec<String>, String> {
        scene_cmd(&app, state.inner(), |m| m.import(text.as_bytes()).map(|ids| (ids, Vec::new())))
    }

    /// Export scenes as file text (the UI saves it where the user chooses).
    #[tauri::command]
    fn scene_export(state: State<Shared>, ids: Vec<String>) -> Result<String, String> {
        state.inner.lock().map_err(|_| "state lock poisoned")?.scenes.export(&ids)
    }

    /// Export scenes to `~/.ak-companion/scene-exports/scenes-<time>.json`,
    /// open that folder in Explorer, and return the file path.
    #[tauri::command]
    fn scene_export_file(state: State<Shared>, ids: Vec<String>) -> Result<String, String> {
        let text = state.inner.lock().map_err(|_| "state lock poisoned")?.scenes.export(&ids)?;
        let dir = crate::app_dirs::companion_file("scene-exports");
        std::fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
        let path = dir.join(format!("scenes-{}.json", chrono::Local::now().format("%Y%m%d-%H%M%S")));
        std::fs::write(&path, text).map_err(|e| format!("write {}: {e}", path.display()))?;
        let _ = std::process::Command::new("explorer.exe").arg(&dir).spawn();
        Ok(path.display().to_string())
    }

    #[tauri::command]
    fn scene_switch(app: AppHandle, state: State<Shared>, id: String) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.switch(&id).map(|f| ((), f)))
    }

    #[tauri::command]
    fn scene_cancel_switch(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| {
            m.cancel_switch();
            Ok(((), Vec::new()))
        })
    }

    /// Apply the current scene again (drop changes made on the keyboard).
    #[tauri::command]
    fn scene_restore(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.restore().map(|f| ((), f)))
    }

    /// Keep the keyboard's current voice mode / LED power in the current scene.
    #[tauri::command]
    fn scene_save_live(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.save_live().map(|f| ((), f)))
    }

    /// Copy a keyboard slot made on another computer into this Companion.
    #[tauri::command]
    fn scene_import_from_board(app: AppHandle, state: State<Shared>, slot: u8) -> Result<(), String> {
        scene_cmd(&app, state.inner(), |m| m.import_from_board(slot).map(|f| ((), f)))
    }

    fn handle_frame(app: &AppHandle, state: &Shared, frame: Inbound) {
        let received = Instant::now(); // [KEY-TIMING] stage origin
        // Key and encoder frames reach the UI the moment they are read. They
        // used to be published at the end of this function, after the PC action
        // they trigger (window focus + keystrokes: tens to hundreds of ms on
        // this reader thread), so the live key view lagged behind the keyboard.
        let ui_early = matches!(frame, Inbound::Key { .. } | Inbound::Encoder { .. });
        if ui_early {
            publish_rx(app, &frame);
        }
        // Firmware update frames: only from the USB link, never logged per ack.
        if matches!(frame, Inbound::OtaStatus(_) | Inbound::OtaAck(_)) {
            let usb = state.inner.lock().map(|inner| inner.kind == TransportKind::Usb).unwrap_or(false);
            if usb {
                handle_ota_frame(app, state, &frame);
            }
            return;
        }
        // Housekeeping frames the reader answers itself.
        match &frame {
            Inbound::Ping => {
                let _ = write_board(state, &protocol::pong_payload());
            }
            Inbound::HelloAck(ack) => {
                let mut late_ack = false;
                if let Ok(mut inner) = state.inner.lock() {
                    let id = inner.connection_id;
                    late_ack = inner.handshake.ack(id);
                    inner.firmware = ack.fw.clone();
                    inner.menu_capable = ack.capabilities.iter().any(|c| c == "menu");
                    inner.board_pc_wait = ack.capabilities.iter().any(|c| c == "pc_asr_wait");
                    inner.scenes.on_hello(&ack.capabilities);
                    inner.menus.reset_display();
                    // A (re)connected keyboard has no Bind session flow open.
                    inner.bind_flow = None;
                    inner.menus.set_hold(false);
                }
                refresh_menus(app, state);
                if late_ack {
                    log_line(app, "[INFO] hello_ack arrived late; menus and PC recognition are available again".to_string());
                    diagnostics::record("INFO", "connect", "hello_ack arrived late");
                }
            }
            Inbound::SystemInfo { payload, .. } => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.firmware = payload.fw.clone();
                }
            }
            _ => {}
        }
        // Acknowledge sequenced board events.
        if let Inbound::Key { seq: Some(seq), .. }
        | Inbound::Encoder { seq: Some(seq), .. }
        | Inbound::Voice { seq: Some(seq), .. }
        | Inbound::Macro { seq: Some(seq), .. }
        | Inbound::Asr { seq: Some(seq), .. }
        | Inbound::ActivationStatus { seq: Some(seq), .. }
        | Inbound::LicenseInfo { seq: Some(seq), .. }
        | Inbound::SystemInfo { seq: Some(seq), .. }
        | Inbound::DeviceInfo { seq: Some(seq), .. }
        | Inbound::DeviceSettings { seq: Some(seq), .. }
        | Inbound::MenuResult { seq: Some(seq), .. }
        | Inbound::BindSession { seq: Some(seq), .. }
        | Inbound::SceneState { seq: Some(seq), .. }
        | Inbound::SceneData { seq: Some(seq), .. } = &frame
        {
            let _ = write_board(state, &protocol::ack_payload(*seq));
        }

        // The board reports its current voice routing (cloud/system) on change
        // and on hello — mirror it into companion state and push to the UI so
        // the selector reflects a board-side switch.
        if let Inbound::VoiceSource { payload, .. } = &frame {
            let src = VoiceSource::parse(&payload.source);
            if let Ok(mut inner) = state.inner.lock() {
                inner.voice_source = src;
            }
            let _ = app.emit("voice-source", src.as_str());
            log_line(app, format!("[VOICE] board voice source = {}", src.as_str()));
            if !payload.mode.is_empty() {
                let _ = app.emit("voice-mode", payload.mode.clone());
            }
        }

        // Chat-mode turn -> Chat panel only (CP-01: never routed to input).
        // The log records role and length, not the conversation.
        if let Inbound::TuyaChat { payload, .. } = &frame {
            let _ = app.emit("tuya-chat", payload.clone());
            log_line(app, format!("[CHAT] {} ({} bytes)", payload.role, payload.text.len()));
        }

        // The board reports Wi-Fi provisioning progress -> push to the UI so the
        // WiFi config panel shows connecting/connected/failed.
        if let Inbound::WifiStatus { payload, .. } = &frame {
            let _ = app.emit("wifi-status", serde_json::json!({
                "state": payload.state, "ssid": payload.ssid
            }));
            log_line(app, format!("[WIFI] {} {}", payload.state, payload.ssid));
        }

        // FR-15: remembered networks (SSIDs only) -> the Connection view.
        if let Inbound::WifiList { payload, .. } = &frame {
            let _ = app.emit("wifi-saved", payload.clone());
            log_line(app, format!("[WIFI] {} saved network(s)", payload.entries.len()));
        }

        // Tuya activation lifecycle -> collapsible Device-panel activation card.
        // qr_url carries only PID+UUID and is empty outside the scan step.
        if let Inbound::ActivationStatus { payload, .. } = &frame {
            let _ = app.emit("activation-status", payload.clone());
            log_line(
                app,
                format!(
                    "[ACTIVATION] {} qr={}",
                    payload.state,
                    if payload.qr_url.is_empty() { "no" } else { "yes" }
                ),
            );
        }

        // Pairing window state -> UI. Wi-Fi / License / reset only work while
        // the user has opened the window on the keyboard.
        // BLE pairing key: accepted only from the USB link (the keyboard sends
        // it on UART0 only). Stored, never logged; the UI gets the id.
        if let Inbound::BlePairKey { payload, .. } = &frame {
            let usb = state.inner.lock().map(|inner| inner.kind == TransportKind::Usb).unwrap_or(false);
            if usb {
                match ble_pairing::save(&payload.key, &payload.key_id) {
                    Ok(status) => {
                        log_line(app, format!("[BLE] paired with keyboard key {}", status.key_id));
                        let _ = app.emit("ble-pairing", status);
                    }
                    Err(err) => log_line(app, format!("[BLE] pairing key rejected: {err}")),
                }
            } else {
                log_line(app, "[BLE] ignored a pairing key that did not arrive over USB".to_string());
            }
        }

        if let Inbound::ProvStatus { payload, .. } = &frame {
            let _ = app.emit("prov-status", payload.clone());
        }

        // Device-license provisioning result -> UI (no secret is carried).
        if let Inbound::LicenseStatus { payload, .. } = &frame {
            let _ = app.emit("license-status", payload.state.clone());
            log_line(app, format!("[LICENSE] {}", payload.state));
        }

        // Product ID write result -> UI. The PID itself never comes back.
        if let Inbound::PidStatus { payload, .. } = &frame {
            let _ = app.emit("pid-status", payload.clone());
            log_line(app, format!("[PID] {} {}", payload.state, payload.reason));
        }

        // Explicit USB-only diagnostic: UUID is visible, AuthKey remains masked.
        if let Inbound::LicenseInfo { payload, .. } = &frame {
            let _ = app.emit("license-info", payload.clone());
            log_line(
                app,
                format!(
                    "[LICENSE] source={} uuid={} authkey=****{}",
                    payload.source, payload.uuid, payload.authkey_tail
                ),
            );
        }

        // AK owns the applied runtime values. Mirror every hello/local/remote
        // settings report into backend state and the Device view.
        if let Inbound::DeviceSettings { payload, .. } = &frame {
            if let Ok(mut inner) = state.inner.lock() {
                inner.device_volume = payload.volume;
                inner.device_brightness = payload.brightness;
                inner.device_settings_synced = true;
            }
            // FR-01: LCD text the Companion writes follows the board's language.
            crate::lcd_text::set_board_language(payload.language.as_deref());
            let _ = app.emit("device-settings", payload.clone());
            log_line(
                app,
                format!(
                    "[DEVICE] volume={} brightness={} language={}",
                    payload.volume,
                    payload.brightness,
                    payload.language.as_deref().unwrap_or("-")
                ),
            );
        }

        // FR-03 scenes: slot reports, slot data, and a settings report that
        // may have made the current scene dirty.
        match &frame {
            Inbound::SceneState { payload, .. } => {
                let reaction = state.inner.lock().map(|mut inner| inner.scenes.on_state(payload)).unwrap_or_default();
                scene_react(app, state, reaction);
            }
            Inbound::SceneData { payload, .. } => {
                let reaction = state.inner.lock().map(|mut inner| inner.scenes.on_data(payload)).unwrap_or_default();
                scene_react(app, state, reaction);
            }
            Inbound::DeviceSettings { payload, .. } => {
                let frames =
                    state.inner.lock().map(|mut inner| inner.scenes.on_device_settings(payload)).unwrap_or_default();
                scene_write(app, state, &frames);
            }
            _ => {}
        }

        // LCD menu answers, dictated menu answers and the K6 model actions.
        // Gated on `control_enabled` like every other board-driven PC action.
        let control_enabled = state.inner.lock().map(|inner| inner.control_enabled).unwrap_or(false);
        let menu_frame = matches!(frame, Inbound::MenuResult { .. } | Inbound::BindSession { .. });
        if (control_enabled && handle_menu_frame(app, state, &frame)) || (!control_enabled && menu_frame) {
            if !ui_early {
                publish_rx(app, &frame);
            }
            return;
        }

        // Voice input (see `voice_send`): keyboard mode keeps and edits the text
        // on the keyboard, K6 posts it through the Crew API; PC mode fills the
        // Crew input box. Nothing the keyboard sends is typed otherwise.
        if voice_send::ends_pc_fill(&frame) {
            if let Ok(mut inner) = state.inner.lock() {
                inner.pc_fill_until = None;
            }
        }
        let decision = state
            .inner
            .lock()
            .ok()
            .filter(|inner| inner.control_enabled)
            .map(|inner| {
                let b = &inner.kc_binds;
                let ctx = voice_send::VoiceCtx {
                    slot_keys: [0, 1, 2].map(|i| b.local_key(i)),
                    active_slot: inner.crew_api_active_slot.as_deref(),
                    pc: b.voice_pc(true),
                    pc_fill: inner.pc_fill_active(),
                };
                voice_send::classify(&frame, &ctx)
            });
        if let Some(decision) = decision.filter(|d| *d != VoiceDirect::NotMine) {
            handle_voice_direct(app, state, decision);
            if !ui_early {
                publish_rx(app, &frame);
            }
            return;
        }


        // PC ASR: when the keyboard's voice route is `system`, the voice key's
        // `voice:start` / `voice:stop` frame the utterance; the Companion
        // records the PC microphone and streams it to the chosen engine. The
        // text goes back to the keyboard's ③ (never typed directly). GATED on
        // `control_enabled` like every other board-driven PC action.
        if let Inbound::Voice { payload, .. } = &frame {
            let (enabled, source) = state
                .inner
                .lock()
                .map(|inner| (inner.control_enabled, inner.voice_source))
                .unwrap_or((false, VoiceSource::Cloud));
            if enabled && source == VoiceSource::System {
                match payload.state {
                    VoiceState::Start => pc_asr_begin(app, state, true, payload.slot.unwrap_or(0)),
                    VoiceState::Stop => pc_asr_end(app, state),
                    VoiceState::Cancel => {
                        pc_asr_cancel(state);
                        log_line(app, "[PC-ASR] cancelled on the keyboard");
                    }
                    VoiceState::Unknown => {}
                }
            } else if !enabled && source == VoiceSource::System {
                // The keyboard waits for an answer; without keyboard control the
                // microphone stays off, so say why instead of timing out.
                match payload.state {
                    VoiceState::Start => {
                        pc_asr_report_failure(app, state, crate::lcd_text::lcd("Voice needs keyboard control"))
                    }
                    VoiceState::Stop => pc_asr_end(app, state),
                    VoiceState::Cancel | VoiceState::Unknown => {}
                }
            }
        }

        // Primary route: stable Crew slot REST control. When API is connected
        // and Chat A/B/C has an API slot binding, consume the frame without
        // focusing a window. Otherwise preserve the legacy SendInput behavior.
        let api_consumed = route_board_via_crew_api(app, state, &frame, received);
        if !api_consumed {
            let (enabled, action) = state
                .inner
                .lock()
                .ok()
                .map(|inner| (inner.control_enabled, inner.kc_router.route(&frame)))
                .unwrap_or((false, ControlAction::None));
            if enabled && action != ControlAction::None {
                let desc = describe_control_action(&action);
                let routed = received.elapsed();
                let started = Instant::now();
                let result = exec_control_action(app, state, &action);
                if let Some((key, gesture, window, hold)) = slot_key_gesture(&frame) {
                    log_line(
                        app,
                        crate::kirocrew_control::timing_line(
                            key,
                            gesture,
                            "keys",
                            window,
                            hold,
                            &[("route", routed), ("focus+keys", started.elapsed())],
                        ),
                    );
                }
                match result {
                    Ok(()) => log_line(app, format!("[KC-CTL] fallback {desc}")),
                    Err(e) => {
                        log_line(app, format!("[KC-CTL] fallback {desc} failed: {e}"));
                        // FR-05: tell the user on the LCD when the instance
                        // switch could not reach the Crew window.
                        if matches!(action, ControlAction::Chord(Chord::SwitchInstance(_))) {
                            write_menu_frames(
                                app,
                                state,
                                vec![notice_payload(crate::lcd_text::lcd("Crew window not in front"), NoticeLevel::Warn)],
                            );
                        }
                    }
                }
            }
        }

        if !ui_early {
            publish_rx(app, &frame);
        }
    }

    /// Log a received frame and hand it to the UI event stream.
    fn publish_rx(app: &AppHandle, frame: &Inbound) {
        log_line(app, format!("[RX] {}", describe_inbound(frame)));
        let _ = app.emit("board-event", inbound_to_json(frame));
    }

    /// How long after Alt+N the Crew window gets to show the chat before the
    /// text is typed (PC mode).
    const PC_FILL_SETTLE: Duration = Duration::from_millis(150);

    /// PC mode: Alt+N to the slot's chat, then type `text` into the Crew input
    /// box without sending. Runs on the board-reader thread so a K6 that
    /// follows is handled after it. On failure (keyboard control off, the
    /// Crew window check, typing) the text goes to the keyboard's input strip.
    fn pc_fill(app: &AppHandle, state: &Shared, slot: u8, text: &str) {
        let jump = state
            .inner
            .lock()
            .ok()
            .and_then(|inner| inner.kc_binds.chats.get(slot as usize - 1).copied())
            .filter(|n| (1..=9).contains(n));
        let result = jump.ok_or_else(|| "no Alt+N number".to_string()).and_then(|n| {
            exec_control_action(app, state, &ControlAction::Chord(Chord::JumpChat(n)))?;
            thread::sleep(PC_FILL_SETTLE);
            exec_control_action(app, state, &ControlAction::Compose(text.to_string()))
        });
        match result {
            Ok(()) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.pc_fill_until = Some(Instant::now() + voice_send::PC_FILL_WINDOW);
                }
                log_line(app, format!("[VOICE] filled the Crew input box of slot {slot} ({} chars)", text.chars().count()));
                write_menu_frames(app, state, vec![voice_send::pc_filled_payload("")]);
            }
            Err(e) => {
                log_line(app, format!("[VOICE] PC fill failed, text kept on the keyboard: {e}"));
                if let Ok(mut inner) = state.inner.lock() {
                    inner.pc_fill_until = None;
                }
                let why = if e.contains("window") { "Crew window not in front" } else { "Typing failed" };
                write_menu_frames(
                    app,
                    state,
                    vec![voice_send::input_restore_payload(text, crate::lcd_text::lcd(why), voice_send::SRC_PC)],
                );
            }
        }
    }

    /// PC mode: K6 / K4 / K1 on the filled Crew input box.
    fn pc_key(app: &AppHandle, state: &Shared, key: voice_send::PcKey) {
        use voice_send::PcKey;
        let chord = match key {
            PcKey::Enter => Chord::Enter,
            PcKey::Backspace => Chord::Backspace,
            PcKey::Clear => Chord::ClearInput,
        };
        let result = exec_control_action(app, state, &ControlAction::Chord(chord));
        if let Ok(mut inner) = state.inner.lock() {
            inner.pc_fill_until = match (&result, key) {
                (Ok(()), PcKey::Backspace) => Some(Instant::now() + voice_send::PC_FILL_WINDOW),
                (Ok(()), _) => None, // sent or cleared: the box is no longer ours
                (Err(_), _) => inner.pc_fill_until,
            };
        }
        if let Err(e) = result {
            log_line(app, format!("[VOICE] {key:?} on the Crew input box failed: {e}"));
            write_menu_frames(
                app,
                state,
                vec![notice_payload(crate::lcd_text::lcd("Crew window not in front"), NoticeLevel::Error)],
            );
        }
    }

    /// Act on a classified voice-input frame (see `voice_send`). The Crew API
    /// call runs off the board-reader thread. On any failure the text goes
    /// back to the keyboard's input strip with a short reason; it is never
    /// typed into a window as a fallback to a failed send.
    fn handle_voice_direct(app: &AppHandle, state: &Shared, decision: VoiceDirect) {
        let fail = |app: &AppHandle, state: &Shared, reason: &'static str, text: Option<String>| {
            let reason = crate::lcd_text::lcd(reason);
            let mut frames = vec![notice_payload(reason, NoticeLevel::Error)];
            if let Some(text) = text {
                frames.push(voice_send::input_restore_payload(&text, reason, voice_send::SRC_RETRY));
            }
            write_menu_frames(app, state, frames);
        };
        match decision {
            VoiceDirect::NotMine | VoiceDirect::Consumed => {}
            VoiceDirect::PcFill { slot, text } => pc_fill(app, state, slot, &text),
            VoiceDirect::PcKey(key) => pc_key(app, state, key),
            VoiceDirect::Fail { reason, text } => {
                log_line(app, format!("[VOICE] not sent: {reason}"));
                fail(app, state, reason, text);
            }
            VoiceDirect::Send { slot, text } => {
                let Some(client) = crew_client(state) else {
                    log_line(app, "[VOICE] not sent: Crew API client unavailable");
                    fail(app, state, "Crew not running", Some(text));
                    return;
                };
                let (app, state) = (app.clone(), state.clone());
                thread::spawn(move || match client.send_message(&slot, &text) {
                    Ok(receipt) => {
                        // Content withheld from logs.
                        log_line(
                            &app,
                            format!(
                                "[VOICE] sent {} chars to {slot}{}",
                                text.chars().count(),
                                if receipt.queued { " (queued)" } else { "" }
                            ),
                        );
                        let msg = if receipt.queued { "Sent (queued)" } else { "Sent" };
                        write_menu_frames(&app, &state, vec![notice_payload(msg, NoticeLevel::Info)]);
                    }
                    Err(e) => {
                        log_line(&app, format!("[VOICE] send to {slot} failed: {e}"));
                        fail(&app, &state, voice_send::failure_reason(&e), Some(text));
                    }
                });
            }
        }
    }

    // ───────────────────── PC streaming ASR (docs/pc-asr.md) ─────────────────────

    /// The UI's view of the ASR settings: keys only as "stored or not".
    #[derive(Serialize)]
    struct AsrSettingsView {
        settings: AsrSettings,
        keys: HashMap<&'static str, bool>,
        local: stream_asr::model::LocalStatus,
        downloading: bool,
    }

    fn asr_view(state: &Shared) -> Result<AsrSettingsView, String> {
        let (settings, downloading) = {
            let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            (inner.asr_settings.clone(), inner.asr_download.is_some())
        };
        let store = secrets::system();
        let mut keys = HashMap::new();
        for kind in [EngineKind::Doubao, EngineKind::Aliyun] {
            let account = stream_asr::key_account(kind).unwrap_or_default();
            keys.insert(kind.as_str(), store.get(account).ok().flatten().is_some_and(|k| !k.is_empty()));
        }
        Ok(AsrSettingsView { settings, keys, local: stream_asr::model::status(), downloading })
    }

    fn emit_pc_asr(app: &AppHandle, id: u64, state: &str, engine: EngineKind, text: &str, message: &str) {
        let _ = app.emit(
            "pc-asr",
            serde_json::json!({ "id": id, "state": state, "engine": engine.as_str(), "text": text, "message": message }),
        );
    }

    /// Voice key down (or the test button): record and stream.
    fn pc_asr_begin(app: &AppHandle, state: &Shared, to_keyboard: bool, slot: u8) {
        // A new press replaces an utterance that is still finishing.
        pc_asr_cancel(state);
        let Ok((settings, id)) = state.inner.lock().map(|mut inner| {
            if to_keyboard {
                inner.pc_asr_failed = None;
            }
            inner.pc_asr_next_id += 1;
            (inner.asr_settings.clone(), inner.pc_asr_next_id)
        }) else {
            return;
        };
        let engine = settings.engine;
        let fallback = settings.fallback_local.then_some(EngineKind::Local);
        let opts = StartOptions { hotwords: settings.hotwords.clone() };
        let factory: EngineFactory =
            Box::new(move |kind| stream_asr::build_engine(kind, &settings, secrets::system().as_ref()));
        let output = {
            let (app, state) = (app.clone(), state.clone());
            Box::new(move |out| on_pc_asr_output(&app, &state, id, out))
        };
        // Register the run before the session can report (a missing key
        // fails at once).
        if let Ok(mut inner) = state.inner.lock() {
            inner.pc_asr = Some(PcAsrRun { id, slot, session: None, capture: None, to_keyboard, fell_back: false });
        }
        let session = Session::start(engine, fallback, opts, factory, output, Duration::from_secs(8));
        let feeder = session.feeder();
        if let Ok(mut inner) = state.inner.lock() {
            match inner.pc_asr.as_mut().filter(|r| r.id == id) {
                Some(run) => run.session = Some(session),
                None => return, // already finished (e.g. no key, no fallback)
            }
        }
        match stream_asr::capture::start(feeder) {
            Ok((capture, device)) => {
                log_line(app, format!("[PC-ASR] listening with {} on {device}", engine.as_str()));
                let kept = state.inner.lock().ok().and_then(|mut inner| {
                    inner.pc_asr.as_mut().filter(|r| r.id == id).map(|run| run.capture = Some(capture))
                });
                if kept.is_some() {
                    emit_pc_asr(app, id, "listening", engine, "", "");
                }
            }
            Err(e) => {
                pc_asr_cancel(state);
                log_line(app, format!("[PC-ASR] microphone: {e}"));
                emit_pc_asr(app, id, "error", engine, "", &e);
                if to_keyboard {
                    pc_asr_report_failure(app, state, crate::lcd_text::lcd("Microphone error"));
                }
            }
        }
    }

    /// Voice key up: stop recording and wait for the final text.
    fn pc_asr_end(app: &AppHandle, state: &Shared) {
        let ended = state.inner.lock().ok().and_then(|mut inner| {
            inner.pc_asr.as_mut().map(|run| {
                run.capture = None; // stops the microphone
                if let Some(s) = &run.session {
                    s.stop();
                }
                run.id
            })
        });
        if let Some(id) = ended {
            let engine = state.inner.lock().map(|i| i.asr_settings.engine).unwrap_or_default();
            emit_pc_asr(app, id, "recognizing", engine, "", "");
            return;
        }
        // The utterance already failed while the key was held: the keyboard
        // now waits in ②, so answer it.
        let failed = state.inner.lock().ok().and_then(|mut inner| inner.pc_asr_failed.take());
        if let Some(msg) = failed {
            release_board_wait(app, state, msg);
        }
    }

    /// A keyboard utterance failed: tell the keyboard now (ignored by it
    /// while the voice key is still held) and again on `voice:stop`.
    fn pc_asr_report_failure(app: &AppHandle, state: &Shared, msg: &'static str) {
        if let Ok(mut inner) = state.inner.lock() {
            inner.pc_asr_failed = Some(msg);
        }
        release_board_wait(app, state, msg);
    }

    /// End the keyboard's ② wait without text (`input_restore` with an empty
    /// PC text); older firmware never waits and gets a notice instead.
    fn release_board_wait(app: &AppHandle, state: &Shared, msg: &str) {
        let waits = state.inner.lock().map(|inner| inner.board_pc_wait).unwrap_or(false);
        let frame = if waits {
            voice_send::input_restore_payload("", msg, voice_send::SRC_PC)
        } else {
            notice_payload(msg, NoticeLevel::Error)
        };
        write_menu_frames(app, state, vec![frame]);
    }

    fn pc_asr_cancel(state: &Shared) {
        let run = state.inner.lock().ok().and_then(|mut inner| inner.pc_asr.take());
        if let Some(session) = run.and_then(|r| r.session) {
            session.cancel();
        }
    }

    /// Session output (runs on the session thread). Text is never logged.
    fn on_pc_asr_output(app: &AppHandle, state: &Shared, id: u64, out: SessionOutput) {
        let finished = matches!(out, SessionOutput::Final { .. } | SessionOutput::Failed { .. });
        let run = state.inner.lock().ok().and_then(|mut inner| {
            let current = inner.pc_asr.as_mut().filter(|r| r.id == id)?;
            if matches!(out, SessionOutput::FellBack { .. }) {
                current.fell_back = true;
            }
            let info = (current.to_keyboard, current.fell_back, current.slot);
            if finished {
                inner.pc_asr = None;
            }
            Some(info)
        });
        let Some((to_keyboard, fell_back, slot)) = run else { return };
        match out {
            SessionOutput::Partial { engine, text } => emit_pc_asr(app, id, "partial", engine, &text, ""),
            SessionOutput::FellBack { from, to, reason } => {
                diagnostics::record("WARN", "pc-asr", &format!("{} failed: {reason}", from.as_str()));
                log_line(app, format!("[PC-ASR] {} failed ({:?}); using {}", from.as_str(), reason.kind, to.as_str()));
                emit_pc_asr(app, id, "fallback", to, "", reason.short());
            }
            SessionOutput::Failed { engine, reason } => {
                diagnostics::record("WARN", "pc-asr", &format!("{} failed: {reason}", engine.as_str()));
                log_line(app, format!("[PC-ASR] {} failed ({:?})", engine.as_str(), reason.kind));
                emit_pc_asr(app, id, "error", engine, "", reason.short());
                if to_keyboard {
                    pc_asr_report_failure(app, state, crate::lcd_text::lcd(reason.short()));
                }
            }
            SessionOutput::Final { engine, text } => {
                log_line(app, format!("[PC-ASR] {} recognised {} chars", engine.as_str(), text.chars().count()));
                emit_pc_asr(app, id, "final", engine, &text, "");
                if to_keyboard {
                    deliver_pc_asr_text(app, state, &text, pc_asr_label(engine, fell_back), slot);
                }
            }
        }
    }

    fn pc_asr_label(engine: EngineKind, fell_back: bool) -> &'static str {
        let en = match (engine, fell_back) {
            (EngineKind::Local, true) => "Cloud failed, used local",
            (EngineKind::Local, false) => "PC ASR: local",
            (EngineKind::Doubao, _) => "PC ASR: Doubao",
            (EngineKind::Aliyun, _) => "PC ASR: Aliyun",
        };
        crate::lcd_text::lcd(en)
    }

    /// Same path as text from the keyboard's own ASR: in keyboard mode the text
    /// lands on the keyboard's input strip (`input_restore`; K6 sends it), in
    /// PC mode (the slot's chat has an Alt+N number) it is typed into the
    /// Crew input box.
    fn deliver_pc_asr_text(app: &AppHandle, state: &Shared, text: &str, label: &str, slot: u8) {
        let Some(text) = crate::kirocrew_control::sanitize_device_text(text) else {
            release_board_wait(app, state, crate::lcd_text::lcd("Didn't catch that"));
            return;
        };
        let pc = state
            .inner
            .lock()
            .map(|inner| inner.kc_binds.voice_pc(inner.control_enabled))
            .unwrap_or([false; 3]);
        if (1..=3).contains(&slot) && pc[slot as usize - 1] {
            pc_fill(app, state, slot, &text);
            return;
        }
        write_menu_frames(app, state, vec![voice_send::input_restore_payload(&text, label, voice_send::SRC_PC)]);
    }

    #[tauri::command]
    fn asr_get_settings(state: State<Shared>) -> Result<AsrSettingsView, String> {
        asr_view(state.inner())
    }

    #[tauri::command]
    fn asr_set_settings(app: AppHandle, state: State<Shared>, settings: AsrSettings) -> Result<AsrSettingsView, String> {
        let mut settings = settings;
        settings.normalize();
        settings.save()?;
        let local_wanted = settings.engine == EngineKind::Local;
        let summary = format!(
            "[PC-ASR] engine {} · local fallback {} · {} hotwords",
            settings.engine.as_str(),
            if settings.fallback_local { "on" } else { "off" },
            settings.hotwords.len()
        );
        state.inner.lock().map_err(|_| "state lock poisoned")?.asr_settings = settings;
        log_line(&app, summary);
        if local_wanted && stream_asr::model::status().installed {
            stream_asr::sherpa::preload(stream_asr::model::paths());
        }
        asr_view(state.inner())
    }

    /// Store a key in the system keychain. The key is never logged or
    /// returned; the UI only gets "stored".
    #[tauri::command]
    fn asr_set_key(app: AppHandle, state: State<Shared>, engine: String, key: String) -> Result<AsrSettingsView, String> {
        let kind = EngineKind::parse(&engine).ok_or("unknown engine")?;
        let account = stream_asr::key_account(kind).ok_or("this engine has no key")?;
        let key = secrets::validate_key(&key)?;
        secrets::system().set(account, &key)?;
        log_line(&app, format!("[PC-ASR] {} key saved to the system keychain", kind.as_str()));
        asr_view(state.inner())
    }

    #[tauri::command]
    fn asr_clear_key(app: AppHandle, state: State<Shared>, engine: String) -> Result<AsrSettingsView, String> {
        let kind = EngineKind::parse(&engine).ok_or("unknown engine")?;
        let account = stream_asr::key_account(kind).ok_or("this engine has no key")?;
        secrets::system().delete(account)?;
        log_line(&app, format!("[PC-ASR] {} key removed from the system keychain", kind.as_str()));
        asr_view(state.inner())
    }

    /// Download + verify the local model and runtime in the background;
    /// progress on `asr-download`.
    #[tauri::command]
    fn asr_local_download(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        let cancel = Arc::new(AtomicBool::new(false));
        {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            if inner.asr_download.is_some() {
                return Err("a download is already running".into());
            }
            inner.asr_download = Some(cancel.clone());
        }
        let shared = state.inner().clone();
        thread::Builder::new()
            .name("pc-asr-download".into())
            .spawn(move || {
                log_line(&app, "[PC-ASR] downloading the local model".to_string());
                let mut last_pct = u64::MAX;
                let result = stream_asr::model::install(
                    &mut |done, total| {
                        let pct = done * 100 / total.max(1);
                        if pct != last_pct {
                            last_pct = pct;
                            let _ = app.emit("asr-download", serde_json::json!({ "state": "running", "done": done, "total": total }));
                        }
                    },
                    &cancel,
                );
                if let Ok(mut inner) = shared.inner.lock() {
                    inner.asr_download = None;
                }
                match result {
                    Ok(()) => {
                        log_line(&app, "[PC-ASR] local model ready".to_string());
                        let _ = app.emit("asr-download", serde_json::json!({ "state": "done" }));
                        if shared.inner.lock().map(|i| i.asr_settings.engine == EngineKind::Local).unwrap_or(false) {
                            stream_asr::sherpa::preload(stream_asr::model::paths());
                        }
                    }
                    Err(e) => {
                        log_line(&app, format!("[PC-ASR] local model download failed: {e}"));
                        let _ = app.emit("asr-download", serde_json::json!({ "state": "error", "message": e }));
                    }
                }
            })
            .map_err(|e| {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.asr_download = None;
                }
                format!("start download: {e}")
            })?;
        Ok(())
    }

    #[tauri::command]
    fn asr_local_cancel(state: State<Shared>) -> Result<(), String> {
        if let Some(flag) = state.inner.lock().map_err(|_| "state lock poisoned")?.asr_download.as_ref() {
            flag.store(true, Ordering::Release);
        }
        Ok(())
    }

    #[tauri::command]
    fn asr_local_remove(app: AppHandle, state: State<Shared>) -> Result<AsrSettingsView, String> {
        if state.inner.lock().map_err(|_| "state lock poisoned")?.asr_download.is_some() {
            return Err("wait for the download to finish".into());
        }
        stream_asr::model::remove()?;
        log_line(&app, "[PC-ASR] local model deleted".to_string());
        asr_view(state.inner())
    }

    /// Settings-page test with the PC microphone; the text stays in the UI.
    #[tauri::command]
    fn asr_test_start(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        pc_asr_begin(&app, state.inner(), false, 0);
        Ok(())
    }

    #[tauri::command]
    fn asr_test_stop(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        pc_asr_end(&app, state.inner());
        Ok(())
    }

    pub fn run() {
        // Load the persisted per-key chat numbers once, and seed both the store
        // and the router's ControlCtx from them so the first board press already
        // jumps to the configured numbers.
        let kc_binds = BindStore::load();
        let mut menus = MenuBoard::new(
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0),
        );
        menus.set_last_models(kc_binds.last_models_by_status_id());
        let jump_chats = kc_binds.chats.clone();
        let scenes = crate::scene_sync::SceneManager::load();
        let knob = crate::kirocrew_control::KnobMode::parse(&scenes.knob());
        let kc_pinned_session = (!kc_binds.pinned_session.is_empty()).then(|| kc_binds.pinned_status_id());
        let crew_api = match CrewApiClient::from_env() {
            Ok(client) => Some(client),
            Err(error) => {
                diagnostics::record("ERROR", "crew-api", &format!("client init failed: {error}"));
                None
            }
        };
        let crew_api_base_url = crew_api
            .as_ref()
            .map(|client| client.config().base_url())
            .unwrap_or_default();
        let asr_settings = AsrSettings::load();
        if asr_settings.engine == EngineKind::Local && stream_asr::model::status().installed {
            // Load the local model now so the first voice press is fast (as a
            // fallback only, it loads on first use: ~300 MB of memory).
            stream_asr::sherpa::preload(stream_asr::model::paths());
        }
        let state: Shared = Arc::new(AppState {
            transport: Mutex::new(None),
            controller: Mutex::new(KiroCrewController::new(None)),
            connecting: std::sync::atomic::AtomicBool::new(false),
            link: Mutex::new(crate::connection_prefs::LinkState::load()),
            inner: Mutex::new(Inner {
                kind: TransportKind::None,
                endpoint: String::new(),
                firmware: String::new(),
                connection_id: 0,
                logs: Vec::new(),
                last_error: String::new(),
                voice_source: VoiceSource::default(),
                device_volume: 70,
                device_brightness: 100,
                device_settings_synced: false,
                kc_router: ControlRouter::new(crate::kirocrew_control::ControlCtx {
                    jump_chats,
                    knob,
                }),
                control_enabled: true,
                status_sink: None,
                status_token: String::new(),
                kc_status: HashMap::new(),
                kc_pinned_session,
                kc_binds,
                crew_api,
                crew_realtime: None,
                crew_api_connected: false,
                crew_api_base_url,
                crew_api_last_error: String::new(),
                crew_api_slots: Vec::new(),
                crew_api_active_slot: None,
                crew_api_drafts: HashMap::new(),
                menus,
                bind_flow: None,
                menu_capable: false,
                crew_identity_failed: false,
                ota_package: None,
                ota: None,
                ota_device_key: String::new(),
                ota_last_emit: String::new(),
                asr_settings,
                pc_asr: None,
                pc_asr_next_id: 0,
                asr_download: None,
                board_pc_wait: false,
                pc_asr_failed: None,
                pc_fill_until: None,
                scenes,
                handshake: crate::handshake::HandshakeWatch::default(),
            }),
        });
        spawn_state_watchdog(state.clone());
        diagnostics::record("INFO", "lifecycle", "Tauri state initialized");

        tauri::Builder::default()
            .manage(state)
            .setup(|app| {
                spawn_reconnect_supervisor(app.handle().clone(), app.state::<Shared>().inner().clone());
                let app_handle = app.handle().clone();
                let shared = app.state::<Shared>().inner().clone();
                spawn_remote_status_poll(&app_handle, &shared);
                if let Err(error) = start_crew_realtime(&app_handle, &shared) {
                    if let Ok(mut inner) = shared.inner.lock() {
                        inner.crew_api_last_error = error.clone();
                    }
                    log_line(
                        &app_handle,
                        format!("[CREW-API] startup unavailable: {error} · Hook fallback remains active"),
                    );
                }
                Ok(())
            })
            .invoke_handler(tauri::generate_handler![
                list_serial_ports,
                scan_ble,
                get_logs,
                clear_logs,
                set_control_enabled,
                set_focus_on_select,
                set_show_workers,
                set_voice_force_keyboard,
                get_connection_prefs,
                set_connection_prefs,
                asr_get_settings,
                asr_set_settings,
                asr_set_key,
                asr_clear_key,
                asr_local_download,
                asr_local_cancel,
                asr_local_remove,
                asr_test_start,
                asr_test_stop,
                get_status,
                crew_api_start,
                crew_api_stop,
                crew_api_list_slots,
                crew_api_send_message,
                crew_api_slot_action,
                connect_device,
                disconnect_device,
                send_display,
                send_led,
                send_config,
                set_device_settings,
                set_device_language,
                update_device_settings,
                keyboard_open_pairing,
                keyboard_ble_unpair,
                set_keymap,
                scenes_list,
                scene_create,
                scene_duplicate,
                scene_update,
                scene_rename,
                scene_delete,
                scene_set_favorites,
                scene_import,
                scene_export,
                scene_export_file,
                scene_switch,
                scene_cancel_switch,
                scene_restore,
                scene_save_live,
                scene_import_from_board,
                send_wifi,
                wifi_saved_list,
                wifi_saved_forget,
                wifi_saved_clear,
                send_license,
                send_pid,
                read_license,
                enter_pairing,
                ble_pair,
                ota_load,
                ota_start,
                ota_cancel,
                ble_pairing_status,
                ble_forget,
                ble_pair_air,
                ble_pair_air_answer,
                set_voice_source,
                kc_bind_window,
                kc_send_text,
                kc_command_bar,
                kc_chord,
                kc_bind_get,
                kc_bind_aliases_get,
                kc_bind_sessions_get,
                kc_bind_alias_set,
                kc_bind_set,
                kc_bind_session_set,
                kc_bind_session_clear,
                kc_bind_clear,
                status_sink_start,
                status_sink_stop,
                status_token,
                kc_list_sessions,
                kc_get_pinned,
                kc_pin_session,
                kc_unpin_session,
                kc_hook_status,
                kc_hook_install,
                kc_hook_remove,
                kc_open_hooks_folder,
            ])
            .run(tauri::generate_context!())
            .expect("error while running tauri application");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use protocol::{EncoderPayload, KeyAction, KeyPayload, VoicePayload, VoiceState};

    #[test]
    fn describe_key_and_encoder() {
        let key = Inbound::Key {
            seq: Some(7),
            payload: KeyPayload {
                id: "k2".into(),
                action: KeyAction::Long,
                held_ms: 820,
                wait_ms: 0,
                bind: None,
                text: None,
                slot: None,
            },
        };
        assert_eq!(describe_inbound(&key), "key k2 Long (820ms) seq=7");

        let enc = Inbound::Encoder {
            seq: None,
            payload: EncoderPayload { detents: -2 },
        };
        assert_eq!(describe_inbound(&enc), "encoder -2");

        let enc_cw = Inbound::Encoder {
            seq: Some(4),
            payload: EncoderPayload { detents: 1 },
        };
        assert_eq!(describe_inbound(&enc_cw), "encoder +1 seq=4");
    }

    #[test]
    fn connect_claim_allows_one_connect_at_a_time() {
        use std::sync::atomic::AtomicBool;
        let flag = AtomicBool::new(false);
        let first = ConnectClaim::try_claim(&flag).expect("first connect claims");
        assert!(ConnectClaim::try_claim(&flag).is_none(), "second concurrent connect refused");
        drop(first);
        assert!(ConnectClaim::try_claim(&flag).is_some(), "released on drop / early return");
    }

    #[test]
    fn inbound_json_is_flat_and_typed() {
        let voice = Inbound::Voice {
            seq: Some(3),
            payload: VoicePayload {
                state: VoiceState::Start,
                slot: None,
            },
        };
        let json = inbound_to_json(&voice);
        assert_eq!(json["kind"], "voice");
        assert_eq!(json["seq"], 3);
        assert_eq!(json["state"], "start");
    }

    #[test]
    fn provisioning_requires_connected_usb_transport() {
        assert!(ensure_usb_provisioning_transport(true, transport::TransportKind::Usb).is_ok());

        for (connected, kind) in [
            (false, transport::TransportKind::None),
            (false, transport::TransportKind::Usb),
            (true, transport::TransportKind::Ble),
        ] {
            assert_eq!(
                ensure_usb_provisioning_transport(connected, kind).unwrap_err(),
                "WiFi and UUID/AuthKey settings require an active USB connection"
            );
        }
    }

    #[test]
    fn device_info_reaches_the_ui_with_its_real_fields_and_no_extras() {
        // The firmware report's example frame, plus a field the Companion does not know:
        // only the declared fields are forwarded, so an unexpected value never reaches the UI or the log.
        let line = r#"{"v":1,"seq":9,"type":"device_info","payload":{"fw":"0.3.98","companion":"usb","handshake":true,"cloud":"active","wifi_ssid":"Office-5G","wifi_ip":"192.168.10.23","wifi_rssi":-61,"wifi_mac":"A0:B1:C2:D3:E4:F5","ble_mac":"A0:B1:C2:D3:E4:F6","ble_name":"Ki-board-D3E4F5","ble_paired":true,"pid":true,"uuid":true,"authkey":true,"license":true,"authkey_value":"SECRET-VALUE-123"}}"#;
        let frame = protocol::decode_line(line).unwrap();
        let json = inbound_to_json(&frame);
        assert_eq!(json["kind"], "device_info");
        assert_eq!(json["wifi_ip"], "192.168.10.23");
        assert_eq!(json["wifi_rssi"], -61);
        assert_eq!(json["wifi_mac"], "A0:B1:C2:D3:E4:F5");
        assert_eq!(json["ble_mac"], "A0:B1:C2:D3:E4:F6");
        assert_eq!(json["ble_paired"], true);
        assert_eq!(json["license"], true);
        assert!(!json.to_string().contains("SECRET-VALUE-123"));
        assert!(!describe_inbound(&frame).contains("SECRET-VALUE-123"));

        // A missing rssi is null (shown as "--"), not 0.
        let off = protocol::decode_line(r#"{"v":1,"type":"device_info","payload":{"fw":"0.3.98","wifi_ssid":""}}"#).unwrap();
        assert!(inbound_to_json(&off)["wifi_rssi"].is_null());
        assert!(inbound_to_json(&off)["pid"].is_null());
    }

    #[test]
    fn device_info_log_line_has_no_network_identifiers() {
        let line = r#"{"v":1,"seq":9,"type":"device_info","payload":{"fw":"0.3.98","companion":"usb","handshake":true,"cloud":"active","wifi_ssid":"Office-5G","wifi_ip":"192.168.10.23","wifi_rssi":-61,"wifi_mac":"A0:B1:C2:D3:E4:F5","ble_mac":"A0:B1:C2:D3:E4:F6","ble_name":"Ki-board-D3E4F5","ble_paired":true,"pid":true,"uuid":true,"authkey":true,"license":true}}"#;
        let text = describe_inbound(&protocol::decode_line(line).unwrap());
        assert_eq!(text, "device_info fw=0.3.98 companion=usb handshake=yes cloud=active wifi=on ble_paired=yes seq=9");
        for private in ["Office-5G", "192.168", "A0:B1", "D3E4F5"] {
            assert!(!text.contains(private), "{private} must not be logged");
        }
    }

    #[test]
    fn unknown_frame_describes_and_serializes() {
        let frame = Inbound::Unknown {
            msg_type: "weird".into(),
        };
        assert_eq!(describe_inbound(&frame), "unknown type=weird");
        assert_eq!(inbound_to_json(&frame)["type"], "weird");
    }
}
