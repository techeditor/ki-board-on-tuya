//! Companion app library entry.
//!
//! Layering:
//! * [`protocol`] — pure v1 wire codec (no IO, unit-tested on any host).
//! * [`transport`] — `serialport`-based UART backend + a stable transport trait.
//! * [`inject`] / [`asr`] — Windows-only key injection and system dictation,
//!   each a trait with a real `#[cfg(windows)]` impl and a non-Windows stub.
//! * [`kirocrew`] — platform-neutral session-control HTTP client (route A):
//!   drives the local Kiro Crew gateway (bind/input/send/switch). Pure helpers
//!   (secret-path + request shaping) are unit-tested on the Linux host.
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
pub mod kirocrew;
pub mod kirocrew_control;
pub mod bind_store;
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
    use crate::kirocrew::KiroCrewClient;
    use crate::kirocrew_control::{Chord, ControlAction, ControlRouter, KiroCrewController};
    use crate::bind_store::{self, BindStore};
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
        /// FP-4: id/handle of the currently bound Kiro Crew session (route A).
        /// A board `asr` text frame is forwarded here via `send()`. `None` until
        /// a session is bound with `kc_create`.
        kc_target: Option<String>,
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
        /// FP-4b M4 (§3.2): per-conversation bind store for K1-K3 slots, loaded
        /// from `config_dir/ak-companion/binds.json` at startup. Short-press a
        /// slot -> focus its bound conversation; long-press -> bind current.
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

    // ───────────────────────── FP-4: Kiro Crew control (route A) ─────────────────────────

    /// Build a session-control client, reading `.local_secret` from disk at
    /// runtime. The secret is never persisted — the client is dropped at the end
    /// of each command (R6.3).
    fn kc_client() -> Result<KiroCrewClient, String> {
        KiroCrewClient::from_env()
    }

    /// bind (op ①): create/bind a Kiro Crew session for `agent`, cache it as the
    /// current target, and return its id.
    #[tauri::command]
    fn kc_create(state: State<Shared>, title: String, agent: String) -> Result<String, String> {
        let id = kc_client()?.create_session(&title, &agent)?;
        state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_target = Some(id.clone());
        Ok(id)
    }

    /// send + execute (ops ②③): run `message` in `target` (or the bound target
    /// when `target` is omitted) as its next agent turn.
    #[tauri::command]
    fn kc_send(state: State<Shared>, message: String, target: Option<String>) -> Result<(), String> {
        let target = resolve_target(&state, target)?;
        kc_client()?.send(&target, &message)
    }

    /// read: fetch the target's latest reply (for LCD feedback / acks).
    #[tauri::command]
    fn kc_read(state: State<Shared>, target: Option<String>) -> Result<String, String> {
        let target = resolve_target(&state, target)?;
        kc_client()?.read(&target)
    }

    /// switch (op ④): stop/close `target`; clears it as the bound target if it
    /// was the current one.
    #[tauri::command]
    fn kc_stop(state: State<Shared>, target: Option<String>) -> Result<(), String> {
        let target = resolve_target(&state, target)?;
        kc_client()?.stop(&target)?;
        if let Ok(mut inner) = state.inner.lock() {
            if inner.kc_target.as_deref() == Some(target.as_str()) {
                inner.kc_target = None;
            }
        }
        Ok(())
    }

    /// Resolve an explicit `target` argument, else the currently bound target.
    fn resolve_target(state: &State<Shared>, target: Option<String>) -> Result<String, String> {
        if let Some(t) = target {
            if !t.trim().is_empty() {
                return Ok(t);
            }
        }
        state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_target
            .clone()
            .ok_or_else(|| "no Kiro Crew session bound (call kc_create first)".to_string())
    }

    /// Forward a board-originated text (ASR / macro command) to the bound Kiro
    /// Crew session via `send()`. Best-effort: no bound target or a gateway error
    /// is logged, not fatal to the reader thread.
    fn forward_asr_to_session(app: &AppHandle, state: &Shared, text: &str) {
        let target = state
            .inner
            .lock()
            .ok()
            .and_then(|inner| inner.kc_target.clone());
        let Some(target) = target else {
            log_line(app, "[KC] asr text dropped: no session bound");
            return;
        };
        match kc_client().and_then(|c| c.send(&target, text)) {
            Ok(()) => log_line(app, format!("[KC] forwarded asr → {target}")),
            Err(e) => log_line(app, format!("[KC] forward failed: {e}")),
        }
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

    // ─────────────── FP-4b M4: per-conversation bind slots (§3.2) ───────────────

    /// M4: return the three bind slots (index 0 = K1 … index 2 = K3) for the UI.
    #[tauri::command]
    fn kc_bind_get(state: State<Shared>) -> Result<Vec<crate::bind_store::Slot>, String> {
        Ok(state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned")?
            .kc_binds
            .slots
            .clone())
    }

    /// M4: bind slot `slot` (1-based) to `url` (+ optional `label`, defaulting to
    /// the parsed slug) and persist. Mirrors a long-press bind from the UI.
    #[tauri::command]
    fn kc_bind_set(
        state: State<Shared>,
        slot: u8,
        url: String,
        label: Option<String>,
    ) -> Result<(), String> {
        let url = url.trim().to_string();
        if url.is_empty() {
            return Err("url is empty".to_string());
        }
        // Prefer an explicit label; else derive the slug from the URL.
        let label = label
            .map(|l| l.trim().to_string())
            .filter(|l| !l.is_empty())
            .or_else(|| crate::bind_store::parse_chat_slug(&url).map(|(_, slug)| slug));
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.set(slot, Some(url), label);
        inner.kc_binds.save()
    }

    /// M4: clear slot `slot` (1-based) and persist.
    #[tauri::command]
    fn kc_bind_clear(state: State<Shared>, slot: u8) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        inner.kc_binds.clear(slot);
        inner.kc_binds.save()
    }

    /// M4: bind the CURRENT window's conversation to slot `slot` (1-based),
    /// mirroring a physical long-press from the UI — captures the address-bar
    /// URL, parses the slug, persists, and LCD-echoes the bind.
    #[tauri::command]
    fn kc_bind_current(app: AppHandle, state: State<Shared>, slot: u8) -> Result<(), String> {
        bind_slot(&app, state.inner(), slot)
    }

    // ─────────────── FP-4b: STATUS sink ("the eyes") — T7 wiring ───────────────

    /// Apply one mapped status update coming from the hook sink (runs on the
    /// sink's listener thread, so it takes the shared state + app handle by
    /// value/clone). Keeps last-status per `session_id`, emits a `display` frame
    /// to the board (best-effort — ignored when the board is offline), and
    /// emits a `kc-status` UI event.
    fn apply_status(app: &AppHandle, state: &Shared, status: StatusDisplay) {
        // Record last-status per session.
        if let Ok(mut inner) = state.inner.lock() {
            inner
                .kc_status
                .insert(status.session_id.clone(), status.clone());
        }
        // Emit a `display` frame to the board (reuse the existing encoder).
        let _ = write_board(
            state,
            &protocol::display_payload(&status.agent_name, &status.state, &status.text),
        );
        // Push to the UI live status list.
        let _ = app.emit(
            "kc-status",
            serde_json::json!({
                "session_id": status.session_id,
                "agent_name": status.agent_name,
                "state": status.state,
                "text": status.text,
            }),
        );
        log_line(
            app,
            format!(
                "[KC-STATUS] {} · {} · {}",
                status.session_id, status.state, status.text
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

    /// Compact human-readable description of a routed control action for logs.
    fn describe_control_action(action: &ControlAction) -> String {
        match action {
            ControlAction::CommandBar(cmd) => format!("command-bar {cmd:?}"),
            ControlAction::Chord(c) => format!("chord {c:?}"),
            ControlAction::SendText(t) => format!("send-text {t:?}"),
            ControlAction::Compose(t) => format!("compose {t:?}"),
            ControlAction::FocusSlot(n) => format!("focus-slot {n}"),
            ControlAction::BindSlot(n) => format!("bind-slot {n}"),
            ControlAction::None => "none".to_string(),
        }
    }

    /// Execute a routed control action. Most actions go straight to the
    /// controller facade via `apply()`; the two M4 slot actions are stateful IO
    /// (bind store + capture/navigate + LCD echo) and are handled here instead.
    fn exec_control_action(
        app: &AppHandle,
        state: &Shared,
        action: &ControlAction,
    ) -> Result<(), String> {
        match action {
            ControlAction::FocusSlot(n) => focus_slot(app, state, *n),
            ControlAction::BindSlot(n) => bind_slot(app, state, *n),
            other => state
                .inner
                .lock()
                .map_err(|_| "state lock poisoned".to_string())
                .and_then(|mut inner| inner.kc_controller.apply(other)),
        }
    }

    /// M4 short-press: focus the conversation bound to slot `n`. Bound slot ->
    /// navigate the window to its `/chat/<slug>` URL; empty slot -> create a new
    /// chat (Alt+Shift+N) per §3.2 "empty slot -> new session".
    fn focus_slot(_app: &AppHandle, state: &Shared, n: u8) -> Result<(), String> {
        let url = state
            .inner
            .lock()
            .map_err(|_| "state lock poisoned".to_string())?
            .kc_binds
            .get(n)
            .and_then(|s| s.url.clone())
            .filter(|u| !u.trim().is_empty());

        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        match url {
            Some(u) => inner.kc_controller.navigate_url(&u),
            // Empty slot: create a new chat instead.
            None => inner.kc_controller.chord(Chord::NewChat),
        }
    }

    /// M4 long-press: bind the CURRENT conversation to slot `n`. Capture the
    /// window's address-bar URL, parse a `/chat/<slug>` out of it, persist it to
    /// the slot (label = slug), and emit a `display` frame confirming the bind
    /// so the board LCD shows `K<n> -> <label>` (M3 renders it).
    fn bind_slot(app: &AppHandle, state: &Shared, n: u8) -> Result<(), String> {
        let captured = {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kc_controller.capture_current_url()?
        };
        let text = captured.ok_or_else(|| {
            "no URL captured (is a browser window with an address bar focused?)".to_string()
        })?;
        let (url, label) = bind_store::parse_chat_slug(&text)
            .ok_or_else(|| format!("no /chat/<slug> in captured text {text:?}"))?;

        // Persist the bind.
        {
            let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
            inner
                .kc_binds
                .set(n, Some(url.clone()), Some(label.clone()));
            inner.kc_binds.save()?;
        }

        // LCD echo: reuse the existing `display` frame encoder (best-effort —
        // ignored when the board is offline). state = "bound".
        let _ = write_board(
            state,
            &protocol::display_payload(&label, "bound", &format!("K{n} -> {label}")),
        );
        log_line(app, format!("[KC-CTL] bound K{n} -> {label} ({url})"));
        Ok(())
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
        | Inbound::Macro { seq: Some(seq), .. } = &frame
        {
            let _ = write_board(state, &protocol::ack_payload(*seq));
        }

        // FP-5: on a voice-start edge, if the active source is the PC's own ASR,
        // activate OS dictation (Win+H). Board-mic (cloud) source is a no-op here.
        if let Inbound::Voice { payload, .. } = &frame {
            let source = state
                .inner
                .lock()
                .map(|inner| inner.voice_source)
                .unwrap_or(VoiceSource::Cloud);
            if pc_asr::route_voice(source, payload.state) == VoiceAction::Activate {
                match pc_asr::activator().activate() {
                    Ok(()) => log_line(app, "[PC-ASR] activated OS dictation (Win+H)"),
                    Err(e) => log_line(app, format!("[PC-ASR] activate failed: {e}")),
                }
            }
        }

        // FP-4: a board-originated text command (an `asr`/dictation result is
        // carried on the `macro` frame's `name` per the wire protocol) is
        // forwarded to the bound Kiro Crew session as a new agent turn (route A,
        // design D3: text → send). No-op when nothing is bound.
        if let Inbound::Macro { payload, .. } = &frame {
            let text = payload.name.trim();
            if !text.is_empty() {
                forward_asr_to_session(app, state, text);
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
                kc_target: None,
                kc_controller: KiroCrewController::new(None),
                kc_router: ControlRouter::new(
                    crate::kirocrew_control::ControlCtx::with_default_jumps(),
                ),
                control_enabled: false,
                status_sink: None,
                status_token: String::new(),
                kc_status: HashMap::new(),
                kc_binds: BindStore::load(),
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
                set_voice_source,
                kc_create,
                kc_send,
                kc_read,
                kc_stop,
                kc_bind_window,
                kc_send_text,
                kc_command_bar,
                kc_chord,
                kc_bind_get,
                kc_bind_set,
                kc_bind_clear,
                kc_bind_current,
                status_sink_start,
                status_sink_stop,
                status_token,
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
