//! Companion app library entry.
//!
//! Layering:
//! * [`protocol`] — pure v1 wire codec (no IO, unit-tested on any host).
//! * [`transport`] — `serialport`-based UART backend + a stable transport trait.
//! * [`inject`] / [`asr`] — Windows-only key injection and system dictation,
//!   each a trait with a real `#[cfg(windows)]` impl and a non-Windows stub.
//! * [`status_sink`] — FP-4b STATUS ("the eyes"): a pure `map_hook_event` (T5)
//!   plus a cross-platform `tiny_http` localhost sink (T6) that ingests Kiro
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
pub mod bind_store;
pub mod crew_api;
pub mod crew_menus;
pub mod lcd_text;
pub mod hook_install;
pub mod pc_asr;
pub mod protocol;
pub mod ble_sec;
pub mod ble_pairing;
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
        Inbound::ActivationStatus { seq, payload } => format!(
            "activation_status {} qr={}{}",
            payload.state,
            if payload.qr_url.is_empty() { "no" } else { "yes" },
            seq_suffix(*seq)
        ),
        Inbound::LicenseStatus { seq, payload } => {
            format!("license_status {}{}", payload.state, seq_suffix(*seq))
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
        Inbound::ActivationStatus { seq, payload } => json!({
            "kind": "activation_status", "seq": seq,
            "state": payload.state, "qr_url": payload.qr_url
        }),
        Inbound::LicenseStatus { seq, payload } => json!({
            "kind": "license_status", "seq": seq, "state": payload.state
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
        Inbound::DeviceSettings { seq, payload } => json!({
            "kind": "device_settings", "seq": seq,
            "volume": payload.volume, "brightness": payload.brightness,
            "language": payload.language
        }),
        Inbound::MenuResult { seq, payload } => json!({
            "kind": "menu_result", "seq": seq, "id": payload.id,
            "selected": payload.selected, "dismissed": payload.dismissed, "reason": payload.reason,
            // The dictated text is an answer: logged as present, never shown.
            "dictated": !payload.text.is_empty()
        }),
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
    use crate::ble_pairing;
    use crate::diagnostics;
    use crate::crew_api::{
        slot_display_status, CrewApiClient, CrewConnection, CrewEvent, CrewRealtime, CrewSlot,
        SendReceipt,
    };
    use crate::crew_menus::{self, MenuBoard};
    use crate::menu_wire::{notice_payload, NoticeLevel};
    use crate::voice_send::{self, VoiceDirect};
    use crate::pc_asr::{self, VoiceAction, VoiceSource};
    use crate::protocol::{self, Inbound, KeyAction};
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
        /// K4 / Win+H, is blocked until control is enabled again.
        control_enabled: bool,
        /// FP-4b STATUS ("the eyes"): the running localhost hook sink, if
        /// started. `status_sink_start` sets it; `status_sink_stop` / drop
        /// tears it down (releasing the port + removing the handoff file).
        status_sink: Option<StatusSink>,
        /// The sink's token, cached so `status_token()` can return it without
        /// re-reading the handoff file. Empty when no sink is running.
        status_token: String,
        /// Last mapped status per Kiro `session_id` (design: keep last-status
        /// per session, emit a `display` frame for it, push to the UI).
        kc_status: HashMap<String, StatusDisplay>,
        /// FP-4b Route STATUS — persisted "watched chat" filter. `None` = follow
        /// every session (only the UI live-list keys them apart). `Some(id)` = pin ONE
        /// Kiro `session_id`: only that session's status is forwarded as a
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
        /// The keyboard advertised `menu` in hello_ack.
        menu_capable: bool,
        /// Local OTA (docs/ota.md): the verified package the user picked, the
        /// running transfer, and the key the keyboard reported after hello
        /// (empty = firmware without OTA).
        ota_package: Option<crate::ota::Package>,
        ota: Option<crate::ota::Sender>,
        ota_device_key: String,
        /// Last `ota-progress` pushed to the UI ("state:percent:reason").
        ota_last_emit: String,
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
        voice_inject: bool,
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
    /// (local name `AK-KB` or service `0x1910`) and return their id/name.
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
        state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .control_enabled = enabled;
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

    /// Voice input target after K6: `false` = direct Crew API send (default),
    /// `true` = type into the Crew composer. Persisted in binds.json.
    #[tauri::command]
    fn set_voice_inject(app: AppHandle, state: State<Shared>, enabled: bool) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let mut next = inner.kc_binds.clone();
        next.voice_inject = enabled;
        next.save()?;
        inner.kc_binds = next;
        drop(inner);
        log_line(
            &app,
            format!("[VOICE] voice input goes {}", if enabled { "into the Crew composer (inject)" } else { "straight to the chat (API)" }),
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
            voice_inject: inner.kc_binds.voice_inject,
            crew_api_connected: inner.crew_api_connected,
            crew_api_base_url: inner.crew_api_base_url.clone(),
            crew_api_last_error: inner.crew_api_last_error.clone(),
            crew_api_slots: inner.crew_api_slots.len(),
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
        let transport_kind = transport.as_deref().unwrap_or("usb");

        diagnostics::record(
            "INFO",
            "connect",
            &format!("begin transport={transport_kind}"),
        );
        let shared = state.inner();
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
                    "This PC is not paired with the keyboard for BLE. Connect over USB, open the pairing window on the keyboard, then pair.",
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
            inner.connection_id
        };

        // Start the reader BEFORE greeting so the board's hello_ack is not
        // missed. USB/UART is physically trusted and greets immediately; for
        // BLE the subscription is already confirmed live inside
        // `BleGattTransport::connect`, so it is likewise safe to greet now
        // (BLE, unlike USB, is not greet-immediately at the wire level — the
        // greeting deliberately waits until after notify-subscription).
        spawn_board_reader(app.clone(), shared.clone(), line_rx, connection_id);
        write_board(shared, &protocol::hello_payload())?;
        let (aliases, status_payloads) = {
            let inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            let aliases = inner.kc_binds.aliases.clone();
            let status_payloads = (0..3)
                .map(|index| {
                    let session_id = inner
                        .kc_binds
                        .sessions
                        .get(index)
                        .map(|value| value.trim())
                        .unwrap_or("");
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
            (aliases, status_payloads)
        };
        if let Err(err) = write_board(shared, &protocol::binding_aliases_payload(&aliases)) {
            log_line(&app, format!("[WARN] binding alias sync failed: {err}"));
        }
        // Replay all three slots on every board link. Explicit unbound frames
        // clear stale colors for slots that have no current cached status.
        for payload in status_payloads {
            if let Err(err) = write_board(shared, &payload) {
                log_line(&app, format!("[WARN] status replay failed: {err}"));
            }
        }
        log_line(
            &app,
            format!("[INFO] connected transport={kind:?} endpoint={endpoint}"),
        );
        diagnostics::record("INFO", "connect", &format!("ready transport={kind:?}"));
        Ok(())
    }

    #[tauri::command]
    fn disconnect_device(state: State<Shared>) -> Result<(), String> {
        diagnostics::record("INFO", "disconnect", "begin");
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
            inner.menus.reset_display();
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

    // ── Local firmware update over USB (docs/ota.md) ───────────────────────

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

    /// Forget the key on this PC. The keyboard keeps its copy until it is
    /// cleared on the device (Settings > Credentials).
    #[tauri::command]
    fn ble_forget(app: AppHandle) -> Result<(), String> {
        ble_pairing::forget()?;
        let _ = app.emit("ble-pairing", ble_pairing::status());
        Ok(())
    }

    /// FP-5: choose which ASR handles a board voice event. `cloud` = board mic +
    /// Tuya cloud ASR (FP-3); `system` = activate the PC's own dictation here.
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
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_binds
            .sessions
            .clone())
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
        let aliases = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_binds.set_alias(slot, &alias)?;
            inner.kc_binds.save()?;
            inner.kc_binds.aliases.clone()
        };
        let _ = write_board(state.inner(), &protocol::binding_aliases_payload(&aliases));
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
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.set(slot, number);
        inner.kc_binds.save()?;
        // Keep the live router context in sync with the persisted numbers.
        inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
        inner.kc_router.ctx.switch_instances = inner.kc_binds.instances.clone();
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
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.set_session(slot, &session_id)?;
        inner.kc_binds.set(slot, number);
        inner.kc_binds.save()?;
        inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
        inner.kc_router.ctx.switch_instances = inner.kc_binds.instances.clone();
        Ok(())
    }

    /// Remove only a slot's Watched Chat association, preserving its Chat # and
    /// AK alias.
    #[tauri::command]
    fn kc_bind_session_clear(state: State<Shared>, slot: u8) -> Result<(), String> {
        let alias = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_binds.clear_session(slot)?;
            inner.kc_binds.save()?;
            // Alias for the now-unbound slot (empty falls back to A/B/C on AK).
            inner
                .kc_binds
                .aliases
                .get((slot as usize).saturating_sub(1))
                .cloned()
                .unwrap_or_default()
        };
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
        inner.kc_router.ctx.switch_instances = inner.kc_binds.instances.clone();
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
        let bound = inner.kc_binds.sessions.clone();
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
            let bound = inner.kc_binds.sessions.clone();
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
    fn route_board_via_crew_api(app: &AppHandle, state: &Shared, frame: &Inbound) -> bool {
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
                    let key = inner.kc_binds.sessions.get(index)?.trim().to_string();
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
                    let _ = app.emit("crew-api-active-slot", slot.clone());
                    log_line(app, format!("[CREW-API] selected stable slot {slot}"));
                    // The scope changed (current chat); ESC'd requests stay
                    // parked until K4 recalls them.
                    refresh_menus(app, state);
                    let fetch = state.inner.lock().map(|inner| inner.menus.slots_needing_questions()).unwrap_or_default();
                    for s in fetch {
                        fetch_questions(app, state, s);
                    }
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

    fn handle_frame(app: &AppHandle, state: &Shared, frame: Inbound) {
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
                if let Ok(mut inner) = state.inner.lock() {
                    inner.firmware = ack.fw.clone();
                    inner.menu_capable = ack.capabilities.iter().any(|c| c == "menu");
                    inner.menus.reset_display();
                }
                refresh_menus(app, state);
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
        | Inbound::DeviceSettings { seq: Some(seq), .. }
        | Inbound::MenuResult { seq: Some(seq), .. } = &frame
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

        // LCD menu answers, dictated menu answers and the K6 model actions.
        // Gated on `control_enabled` like every other board-driven PC action.
        let control_enabled = state.inner.lock().map(|inner| inner.control_enabled).unwrap_or(false);
        let menu_frame = matches!(frame, Inbound::MenuResult { .. });
        if (control_enabled && handle_menu_frame(app, state, &frame)) || (!control_enabled && menu_frame) {
            log_line(app, format!("[RX] {}", describe_inbound(&frame)));
            let _ = app.emit("board-event", inbound_to_json(&frame));
            return;
        }

        // Voice input ③, direct-send mode (default): the keyboard keeps and
        // edits the text, K6 posts it through the Crew API; nothing is typed.
        // `voice_inject` keeps the old composer-injection path below.
        let direct = state
            .inner
            .lock()
            .ok()
            .filter(|inner| inner.control_enabled && !inner.kc_binds.voice_inject)
            .map(|inner| voice_send::classify(&frame, inner.crew_api_active_slot.as_deref()));
        if let Some(decision) = direct.filter(|d| *d != VoiceDirect::NotMine) {
            handle_voice_direct(app, state, decision);
            log_line(app, format!("[RX] {}", describe_inbound(&frame)));
            let _ = app.emit("board-event", inbound_to_json(&frame));
            return;
        }

        // FP-5: on a voice-start edge, if the active source is the PC's own ASR,
        // activate OS dictation (Win+H). Board-mic (cloud) source is a no-op here.
        // GATED on `control_enabled` (same gate as route B): while control is
        // disabled the K4 voice frame only reflects trigger state in the UI and
        // NEVER activates Win+H — no board frame drives the PC/Kiro Crew.
        if let Inbound::Voice { payload, .. } = &frame {
            let (enabled, source) = state
                .inner
                .lock()
                .map(|inner| (inner.control_enabled, inner.voice_source))
                .unwrap_or((false, VoiceSource::Cloud));
            if enabled && pc_asr::route_voice(source, payload.state) == VoiceAction::Activate {
                // Bring the Kiro Crew WINDOW to the foreground so OS dictation
                // types into it. Do NOT inject Alt+Enter here: that chord SUBMITS
                // the composer in Kiro Crew, so using it to "focus" auto-sent the
                // text left from the previous dictation (confirmed off-by-one on
                // hardware — each K4 press submitted the prior utterance). Window
                // focus alone is enough (the composer is the default focused
                // control) and can never submit. Win+H fires only when the
                // verified Crew window took focus; otherwise the user's speech
                // would be typed into whatever window is in front.
                let focused = state
                    .controller
                    .lock()
                    .map_err(|_| "controller lock poisoned".to_string())
                    .and_then(|mut controller| controller.focus());
                match focused.and_then(|()| pc_asr::activator().activate()) {
                    Ok(()) => log_line(app, "[PC-ASR] focused window + activated OS dictation (Win+H)"),
                    Err(e) => log_line(app, format!("[PC-ASR] dictation not started: {e}")),
                }
            }
        }

        // Primary route: stable Crew slot REST control. When API is connected
        // and Chat A/B/C has an API slot binding, consume the frame without
        // focusing a window. Otherwise preserve the legacy SendInput behavior.
        let api_consumed = route_board_via_crew_api(app, state, &frame);
        if !api_consumed {
            let (enabled, action) = state
                .inner
                .lock()
                .ok()
                .map(|inner| (inner.control_enabled, inner.kc_router.route(&frame)))
                .unwrap_or((false, ControlAction::None));
            if enabled && action != ControlAction::None {
                let desc = describe_control_action(&action);
                let result = exec_control_action(app, state, &action);
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

        log_line(app, format!("[RX] {}", describe_inbound(&frame)));
        let _ = app.emit("board-event", inbound_to_json(&frame));
    }

    /// Direct send of a confirmed voice input (see `voice_send`). Runs the
    /// HTTP call off the board-reader thread. On any failure the text goes
    /// back to the keyboard's input confirm with a short reason; it is never
    /// typed into a window as a fallback.
    fn handle_voice_direct(app: &AppHandle, state: &Shared, decision: VoiceDirect) {
        let fail = |app: &AppHandle, state: &Shared, reason: &str, text: Option<String>| {
            let mut frames = vec![notice_payload(reason, NoticeLevel::Error)];
            if let Some(text) = text {
                frames.push(voice_send::input_restore_payload(&text, reason));
            }
            write_menu_frames(app, state, frames);
        };
        match decision {
            VoiceDirect::NotMine | VoiceDirect::Consumed => {}
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
        menus.set_last_models(kc_binds.last_models.clone());
        let jump_chats = kc_binds.chats.clone();
        let switch_instances = kc_binds.instances.clone();
        let kc_pinned_session = (!kc_binds.pinned_session.is_empty())
            .then(|| kc_binds.pinned_session.clone());
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
        let state: Shared = Arc::new(AppState {
            transport: Mutex::new(None),
            controller: Mutex::new(KiroCrewController::new(None)),
            connecting: std::sync::atomic::AtomicBool::new(false),
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
                    switch_instances,
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
                menu_capable: false,
                ota_package: None,
                ota: None,
                ota_device_key: String::new(),
                ota_last_emit: String::new(),
            }),
        });
        spawn_state_watchdog(state.clone());
        diagnostics::record("INFO", "lifecycle", "Tauri state initialized");

        tauri::Builder::default()
            .manage(state)
            .setup(|app| {
                let app_handle = app.handle().clone();
                let shared = app.state::<Shared>().inner().clone();
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
                set_voice_inject,
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
                set_keymap,
                send_wifi,
                send_license,
                read_license,
                enter_pairing,
                ble_pair,
                ota_load,
                ota_start,
                ota_cancel,
                ble_pairing_status,
                ble_forget,
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
                bind: None,
                text: None,
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
    fn unknown_frame_describes_and_serializes() {
        let frame = Inbound::Unknown {
            msg_type: "weird".into(),
        };
        assert_eq!(describe_inbound(&frame), "unknown type=weird");
        assert_eq!(inbound_to_json(&frame)["type"], "weird");
    }
}
