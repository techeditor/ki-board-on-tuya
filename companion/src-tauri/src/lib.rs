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

pub mod asr;
pub mod inject;
pub mod kirocrew_control;
pub mod bind_store;
pub mod hook_install;
pub mod pc_asr;
pub mod protocol;
pub mod status_sink;
pub mod transport;

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
        Inbound::Nav { seq, payload } => format!(
            "nav {:?} {:?} ({}ms){}",
            payload.dir,
            payload.action,
            payload.held_ms,
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
        Inbound::WifiStatus { seq, payload } => {
            format!("wifi_status {} {}{}", payload.state, payload.ssid, seq_suffix(*seq))
        }
        Inbound::LicenseStatus { seq, payload } => {
            format!("license_status {}{}", payload.state, seq_suffix(*seq))
        }
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

/// Convert a decoded frame into a flat JSON object for the UI event stream.
pub fn inbound_to_json(frame: &Inbound) -> serde_json::Value {
    use serde_json::json;
    match frame {
        Inbound::Key { seq, payload } => json!({
            "kind": "key", "seq": seq, "id": payload.id,
            "action": payload.action, "held_ms": payload.held_ms
        }),
        Inbound::Nav { seq, payload } => json!({
            "kind": "nav", "seq": seq, "dir": payload.dir,
            "action": payload.action, "held_ms": payload.held_ms
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
            "kind": "voice_source", "seq": seq, "source": payload.source
        }),
        Inbound::WifiStatus { seq, payload } => json!({
            "kind": "wifi_status", "seq": seq, "state": payload.state, "ssid": payload.ssid
        }),
        Inbound::LicenseStatus { seq, payload } => json!({
            "kind": "license_status", "seq": seq, "state": payload.state
        }),
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
    use crate::pc_asr::{self, VoiceAction, VoiceSource};
    use crate::protocol::{self, Inbound};
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
    use std::sync::{Arc, Mutex};
    use std::thread;
    use tauri::{AppHandle, Emitter, Manager, State};

    struct Inner {
        transport: Option<Box<dyn BoardTransport>>,
        kind: TransportKind,
        endpoint: String,
        firmware: String,
        connection_id: u64,
        logs: Vec<String>,
        last_error: String,
        voice_source: VoiceSource,
        /// FP-4b (route B): the window-focus + SendInput controller ("the
        /// hands") and the pure router that maps board events to control
        /// actions. This is the control path that supersedes route A.
        kc_controller: KiroCrewController,
        kc_router: ControlRouter,
        /// When false (the default), board key/nav frames are NEVER routed to
        /// Kiro Crew control — the INPUT view only reflects trigger state. The
        /// user must explicitly enable control (UI toggle -> set_control_enabled)
        /// before board events drive the local Kiro Crew window.
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
        /// FP-4b Route STATUS — "watched chat" filter. `None` = follow every
        /// session (only the UI live-list keys them apart). `Some(id)` = pin ONE
        /// Kiro `session_id`: only that session's status is forwarded as a
        /// `display` frame to the board, so a background chat's activity no
        /// longer clobbers the LCD. The `kc-status` UI event is ALWAYS pushed
        /// (pinned or not) so the picker can list every active session and the
        /// user can choose which one to pin.
        kc_pinned_session: Option<String>,
        /// FP-4b M4 (§3.2): per-key chat-number store for K1-K3, loaded from
        /// `config_dir/ak-companion/binds.json` at startup. Each key binds to a
        /// chat NUMBER (1..=9); pressing it fires `Alt+N` (jump to chat N). The
        /// numbers here are mirrored into `kc_router.ctx.jump_chats`.
        kc_binds: BindStore,
    }

    struct AppState {
        inner: Mutex<Inner>,
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
        control_enabled: bool,
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

    fn write_board(state: &Shared, payload: &Value) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        let transport = inner.transport.as_mut().ok_or("device is not connected")?;
        transport.write_json(payload)
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
    /// is OFF: while disabled the board only reports trigger state to the UI and
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

    #[tauri::command]
    fn get_status(state: State<Shared>) -> Result<Status, String> {
        let inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        Ok(Status {
            device_connected: inner.transport.is_some(),
            transport: inner.kind,
            endpoint: inner.endpoint.clone(),
            firmware_version: inner.firmware.clone(),
            last_error: inner.last_error.clone(),
            voice_source: inner.voice_source.as_str().to_string(),
            control_enabled: inner.control_enabled,
        })
    }

    #[tauri::command]
    fn connect_device(
        app: AppHandle,
        state: State<Shared>,
        transport: Option<String>,
        port_name: Option<String>,
    ) -> Result<(), String> {
        let transport_kind = transport.as_deref().unwrap_or("usb");
        let shared = state.inner();
        // Only one transport at a time. If something is already connected, do
        // NOT silently tear it down — tell the user to disconnect first. This
        // makes switching USB<->BLE an explicit two-step action and avoids
        // yanking a live link out from under the reader thread.
        {
            let inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            if inner.transport.is_some() {
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
                let transport =
                    BleGattTransport::connect(device_id, line_tx).map_err(|err| {
                        log_line(&app, format!("[ERROR] ble transport: {err}"));
                        err
                    })?;
                Box::new(transport)
            }
            _ => {
                let transport =
                    UsbSerialTransport::connect(port_name.clone(), line_tx).map_err(|err| {
                        log_line(&app, format!("[ERROR] transport: {err}"));
                        err
                    })?;
                Box::new(transport)
            }
        };
        let kind = boxed.kind();
        let endpoint = boxed.endpoint();

        let connection_id = {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kind = kind;
            inner.endpoint = endpoint.clone();
            inner.transport = Some(boxed);
            inner.last_error.clear();
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
        log_line(
            &app,
            format!("[INFO] connected transport={kind:?} endpoint={endpoint}"),
        );
        Ok(())
    }

    #[tauri::command]
    fn disconnect_device(state: State<Shared>) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        // Best-effort graceful goodbye BEFORE we drop the transport: plain UART
        // has no hardware disconnect signal, so without this the board's link
        // header would stay "USB Connected" forever after we close the port.
        if let Some(transport) = inner.transport.as_mut() {
            let _ = transport.write_json(&protocol::bye_payload());
        }
        if let Some(mut transport) = inner.transport.take() {
            // Sets the transport stop flag so the low-level serial reader thread
            // exits and releases the OS port; dropping `transport` here closes
            // the port handle we held. The app-level board reader then sees
            // `transport.is_none()` on its next liveness check and also exits.
            transport.disconnect();
        }
        inner.kind = TransportKind::None;
        inner.endpoint.clear();
        inner.connection_id = inner.connection_id.wrapping_add(1);
        Ok(())
    }

    #[tauri::command]
    fn send_display(
        state: State<Shared>,
        agent_name: String,
        display_state: String,
        text: String,
    ) -> Result<(), String> {
        write_board(
            state.inner(),
            &protocol::display_payload(&agent_name, &display_state, &text),
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

    /// Provision Wi-Fi on the board: send SSID + password over the JSONL
    /// channel. The board applies it via netmgr and reports back a wifi_status
    /// frame (surfaced to the UI as a 'wifi-status' event).
    #[tauri::command]
    fn send_wifi(state: State<Shared>, ssid: String, pass: String) -> Result<(), String> {
        if ssid.trim().is_empty() {
            return Err("SSID is required".to_string());
        }
        write_board(state.inner(), &protocol::wifi_payload(ssid.trim(), &pass))
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
        write_board(state.inner(), &protocol::license_payload(uuid.trim(), authkey.trim()))
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
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_controller.set_title_match(title);
        // Focus now to verify the window exists; bubbles up the "no match" Err.
        inner.kc_controller.focus()
    }

    /// route B: focus the bound window and type `text` (no command bar, no Enter).
    #[tauri::command]
    fn kc_send_text(state: State<Shared>, text: String) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_controller.send_text(&text)
    }

    /// route B: focus, open the command bar (Ctrl+K), type `cmd`, press Enter.
    #[tauri::command]
    fn kc_command_bar(state: State<Shared>, cmd: String) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_controller.command_bar(&cmd)
    }

    /// route B: focus and inject a single named chord
    /// (`enter`/`esc`/`up`/`down`/`left`/`right`/`ctrlk`).
    #[tauri::command]
    fn kc_chord(state: State<Shared>, name: String) -> Result<(), String> {
        let chord = Chord::parse(&name).ok_or_else(|| format!("unknown chord {name:?}"))?;
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_controller.chord(chord)
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

    /// M4: bind slot `slot` (1-based) to chat `number` (`1..=9`). Validates the
    /// number, persists the store, and updates the LIVE router `ControlCtx` so
    /// the next board press jumps to the new number without a restart.
    #[tauri::command]
    fn kc_bind_set(state: State<Shared>, slot: u8, number: u8) -> Result<(), String> {
        if !(1..=9).contains(&number) {
            return Err(format!("chat number must be 1..=9 (got {number})"));
        }
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.set(slot, number);
        inner.kc_binds.save()?;
        // Keep the live router context in sync with the persisted numbers.
        inner.kc_router.ctx.jump_chats = inner.kc_binds.chats.clone();
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

    /// Apply one mapped status update coming from the hook sink (runs on the
    /// sink's listener thread, so it takes the shared state + app handle by
    /// value/clone). Keeps last-status per `session_id`, emits a `display` frame
    /// to the board (best-effort — ignored when the board is offline), and
    /// emits a `kc-status` UI event.
    fn apply_status(app: &AppHandle, state: &Shared, status: StatusDisplay) {
        // Record last-status per session, and read the pinned filter under the
        // same lock so the forward decision is consistent with what the UI shows.
        let forward = if let Ok(mut inner) = state.inner.lock() {
            inner
                .kc_status
                .insert(status.session_id.clone(), status.clone());
            // Forward to the board only when no session is pinned (follow all)
            // or the pinned session matches this event's session.
            crate::status_sink::should_forward(
                inner.kc_pinned_session.as_deref(),
                &status.session_id,
            )
        } else {
            // Lock poisoned: fail open so a status still reaches the board.
            true
        };
        // Emit a `display` frame to the board only for the watched session
        // (reuse the existing encoder). Best-effort — ignored when offline.
        if forward {
            let _ = write_board(
                state,
                &protocol::display_payload(&status.agent_name, &status.state, &status.text),
            );
        }
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
                "forwarded": forward,
            }),
        );
        log_line(
            app,
            format!(
                "[KC-STATUS] {} · {} · {}{}",
                status.session_id,
                status.state,
                status.text,
                if forward { "" } else { " (muted — not pinned)" }
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
        // Set the pin and grab the pinned session's last status under one lock.
        let last = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_pinned_session = Some(sid.clone());
            inner.kc_status.get(&sid).cloned()
        };
        // Refresh the board now with the pinned session's last status.
        if let Some(s) = last {
            let _ = write_board(
                &state,
                &protocol::display_payload(&s.agent_name, &s.state, &s.text),
            );
        }
        log_line(&app, format!("[KC-STATUS] pinned session {sid}"));
        Ok(())
    }

    /// Clear the pin: go back to following every session (all statuses drive the
    /// board again).
    #[tauri::command]
    fn kc_unpin_session(app: AppHandle, state: State<Shared>) -> Result<(), String> {
        state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_pinned_session = None;
        log_line(&app, "[KC-STATUS] unpinned — following all sessions");
        Ok(())
    }

    // ─────── FP-4b Route STATUS — one-click global hook install/remove ───────

    /// Resolve the hook command: the UI may pass an explicit companion exe path
    /// (e.g. the user browsed to `companion.exe`); empty falls back to THIS
    /// running executable.
    fn resolve_hook_command(exe: Option<String>) -> String {
        let exe = exe
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(hook_install::current_exe_path);
        hook_install::hook_command(&exe)
    }

    /// Report per-agent hook install status (does not modify anything). `exe`
    /// optional — only affects the `command` string echoed back.
    #[tauri::command]
    fn kc_hook_status(exe: Option<String>) -> Result<hook_install::HookStatus, String> {
        hook_install::status(&resolve_hook_command(exe))
    }

    /// One-click: inject `companion --hook` into agentSpawn/postToolUse/stop of
    /// EVERY local Kiro agent (idempotent). Returns the post-install status.
    #[tauri::command]
    fn kc_hook_install(
        app: AppHandle,
        exe: Option<String>,
    ) -> Result<hook_install::HookStatus, String> {
        let command = resolve_hook_command(exe);
        let st = hook_install::install(&command)?;
        log_line(
            &app,
            format!(
                "[KC-HOOK] installed into {} agent(s) · {}",
                st.agents.len(),
                command
            ),
        );
        Ok(st)
    }

    /// One-click: remove the companion `--hook` entry from every local Kiro
    /// agent (leaves hand-authored hooks intact). Returns the post-remove status.
    #[tauri::command]
    fn kc_hook_remove(
        app: AppHandle,
        exe: Option<String>,
    ) -> Result<hook_install::HookStatus, String> {
        let st = hook_install::remove(&resolve_hook_command(exe))?;
        log_line(&app, format!("[KC-HOOK] removed from {} agent(s)", st.agents.len()));
        Ok(st)
    }

    /// Compact human-readable description of a routed control action for logs.
    fn describe_control_action(action: &ControlAction) -> String {
        match action {
            ControlAction::CommandBar(cmd) => format!("command-bar {cmd:?}"),
            ControlAction::Chord(c) => format!("chord {c:?}"),
            ControlAction::SendText(t) => format!("send-text {t:?}"),
            ControlAction::Compose(t) => format!("compose {t:?}"),
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
            .inner
            .lock()
            .map_err(|_| "state lock poisoned".to_string())
            .and_then(|mut inner| inner.kc_controller.apply(action))
    }

    fn spawn_board_reader(
        app: AppHandle,
        state: Shared,
        line_rx: Receiver<String>,
        connection_id: u64,
    ) {
        thread::spawn(move || {
            while let Ok(line) = line_rx.recv() {
                // Stop if this connection has been superseded.
                let live = state
                    .inner
                    .lock()
                    .map(|inner| inner.transport.is_some() && inner.connection_id == connection_id)
                    .unwrap_or(false);
                if !live {
                    break;
                }
                if let Some(frame) = protocol::decode_line(&line) {
                    handle_frame(&app, &state, frame);
                }
            }
        });
    }

    fn handle_frame(app: &AppHandle, state: &Shared, frame: Inbound) {
        // Housekeeping frames the reader answers itself.
        match &frame {
            Inbound::Ping => {
                let _ = write_board(state, &protocol::pong_payload());
            }
            Inbound::HelloAck(ack) => {
                if let Ok(mut inner) = state.inner.lock() {
                    inner.firmware = ack.fw.clone();
                }
            }
            _ => {}
        }
        // Acknowledge sequenced board events.
        if let Inbound::Key { seq: Some(seq), .. }
        | Inbound::Nav { seq: Some(seq), .. }
        | Inbound::Voice { seq: Some(seq), .. }
        | Inbound::Macro { seq: Some(seq), .. }
        | Inbound::Asr { seq: Some(seq), .. } = &frame
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
        }

        // The board reports Wi-Fi provisioning progress -> push to the UI so the
        // WiFi config panel shows connecting/connected/failed.
        if let Inbound::WifiStatus { payload, .. } = &frame {
            let _ = app.emit("wifi-status", serde_json::json!({
                "state": payload.state, "ssid": payload.ssid
            }));
            log_line(app, format!("[WIFI] {} {}", payload.state, payload.ssid));
        }

        // Device-license provisioning result -> UI (no secret is carried).
        if let Inbound::LicenseStatus { payload, .. } = &frame {
            let _ = app.emit("license-status", payload.state.clone());
            log_line(app, format!("[LICENSE] {}", payload.state));
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
                // control) and can never submit. Best-effort; Win+H still fires if
                // focus fails (the user may already be on the window).
                if let Ok(mut inner) = state.inner.lock() {
                    let _ = inner.kc_controller.focus();
                }
                match pc_asr::activator().activate() {
                    Ok(()) => log_line(app, "[PC-ASR] focused window + activated OS dictation (Win+H)"),
                    Err(e) => log_line(app, format!("[PC-ASR] activate failed: {e}")),
                }
            }
        }

        // FP-4b (route B — "the hands"): map the board event to a control action
        // via the pure router, then drive the local Kiro Crew window (focus +
        // SendInput). GATED on `control_enabled` (default OFF): while disabled,
        // board key/nav frames only reflect trigger state in the UI and NEVER
        // drive Kiro Crew. Errors (e.g. no window matches) are logged, never
        // fatal to the reader thread.
        {
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
                    Ok(()) => log_line(app, format!("[KC-CTL] {desc}")),
                    Err(e) => log_line(app, format!("[KC-CTL] {desc} failed: {e}")),
                }
            }
        }

        log_line(app, format!("[RX] {}", describe_inbound(&frame)));
        let _ = app.emit("board-event", inbound_to_json(&frame));
    }

    pub fn run() {
        // Load the persisted per-key chat numbers once, and seed both the store
        // and the router's ControlCtx from them so the first board press already
        // jumps to the configured numbers.
        let kc_binds = BindStore::load();
        let jump_chats = kc_binds.chats.clone();
        let state: Shared = Arc::new(AppState {
            inner: Mutex::new(Inner {
                transport: None,
                kind: TransportKind::None,
                endpoint: String::new(),
                firmware: String::new(),
                connection_id: 0,
                logs: Vec::new(),
                last_error: String::new(),
                voice_source: VoiceSource::System,
                kc_controller: KiroCrewController::new(None),
                kc_router: ControlRouter::new(crate::kirocrew_control::ControlCtx {
                    jump_chats,
                }),
                control_enabled: false,
                status_sink: None,
                status_token: String::new(),
                kc_status: HashMap::new(),
                kc_pinned_session: None,
                kc_binds,
            }),
        });

        tauri::Builder::default()
            .manage(state)
            .invoke_handler(tauri::generate_handler![
                list_serial_ports,
                scan_ble,
                get_logs,
                clear_logs,
                set_control_enabled,
                get_status,
                connect_device,
                disconnect_device,
                send_display,
                send_led,
                send_config,
                send_wifi,
                send_license,
                set_voice_source,
                kc_bind_window,
                kc_send_text,
                kc_command_bar,
                kc_chord,
                kc_bind_get,
                kc_bind_set,
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
            ])
            .run(tauri::generate_context!())
            .expect("error while running tauri application");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use protocol::{KeyAction, KeyPayload, NavDir, NavPayload, VoicePayload, VoiceState};

    #[test]
    fn describe_key_and_nav() {
        let key = Inbound::Key {
            seq: Some(7),
            payload: KeyPayload {
                id: "k2".into(),
                action: KeyAction::Long,
                held_ms: 820,
            },
        };
        assert_eq!(describe_inbound(&key), "key k2 Long (820ms) seq=7");

        let nav = Inbound::Nav {
            seq: None,
            payload: NavPayload {
                dir: NavDir::Center,
                action: KeyAction::Down,
                held_ms: 0,
            },
        };
        assert_eq!(describe_inbound(&nav), "nav Center Down (0ms)");
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
    fn unknown_frame_describes_and_serializes() {
        let frame = Inbound::Unknown {
            msg_type: "weird".into(),
        };
        assert_eq!(describe_inbound(&frame), "unknown type=weird");
        assert_eq!(inbound_to_json(&frame)["type"], "weird");
    }
}
