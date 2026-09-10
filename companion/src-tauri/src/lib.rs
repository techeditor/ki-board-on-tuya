//! Companion app library entry.
//!
//! Layering:
//! * [`protocol`] — pure v1 wire codec (no IO, unit-tested on any host).
//! * [`transport`] — `serialport`-based UART backend + a stable transport trait.
//! * [`inject`] / [`asr`] — Windows-only key injection and system dictation,
//!   each a trait with a real `#[cfg(windows)]` impl and a non-Windows stub.
//! * [`describe_inbound`] / [`inbound_to_json`] — pure frame-routing helpers,
//!   shared by the Tauri reader thread and testable on Linux.
//! * The Tauri application itself (`app` module + [`run`]) is `#[cfg(windows)]`
//!   because Tauri needs WebKitGTK, which is absent on the Linux CI host.

pub mod asr;
pub mod inject;
pub mod pc_asr;
pub mod protocol;
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
    use crate::transport::usb_serial::{self, SerialPortInfo, UsbSerialTransport};
    use crate::transport::{BoardTransport, TransportKind};
    use crate::{describe_inbound, inbound_to_json};
    use serde::Serialize;
    use serde_json::Value;
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

    #[tauri::command]
    fn get_logs(state: State<Shared>) -> Result<Vec<String>, String> {
        Ok(state.inner.lock().map_err(|_| "state lock poisoned")?.logs.clone())
    }

    #[tauri::command]
    fn clear_logs(state: State<Shared>) -> Result<(), String> {
        state.inner.lock().map_err(|_| "state lock poisoned")?.logs.clear();
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
        })
    }

    #[tauri::command]
    fn connect_device(
        app: AppHandle,
        state: State<Shared>,
        port_name: Option<String>,
    ) -> Result<(), String> {
        let shared = state.inner();
        // Tear down any existing connection first.
        {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            if let Some(mut existing) = inner.transport.take() {
                existing.disconnect();
            }
            inner.kind = TransportKind::None;
            inner.endpoint.clear();
            inner.connection_id = inner.connection_id.wrapping_add(1);
        }

        let (line_tx, line_rx) = mpsc::channel::<String>();
        let transport = UsbSerialTransport::connect(port_name, line_tx).map_err(|err| {
            log_line(&app, format!("[ERROR] transport: {err}"));
            err
        })?;
        let kind = transport.kind();
        let endpoint = transport.endpoint();

        let connection_id = {
            let mut inner = shared.inner.lock().map_err(|_| "state lock poisoned")?;
            inner.kind = kind;
            inner.endpoint = endpoint.clone();
            inner.transport = Some(Box::new(transport));
            inner.last_error.clear();
            inner.connection_id = inner.connection_id.wrapping_add(1);
            inner.connection_id
        };

        // USB/UART is physically trusted: greet immediately.
        write_board(shared, &protocol::hello_payload())?;
        spawn_board_reader(app.clone(), shared.clone(), line_rx, connection_id);
        log_line(&app, format!("[INFO] connected endpoint={endpoint}"));
        Ok(())
    }

    #[tauri::command]
    fn disconnect_device(state: State<Shared>) -> Result<(), String> {
        let mut inner = state.inner.lock().map_err(|_| "state lock poisoned")?;
        if let Some(mut transport) = inner.transport.take() {
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
            }),
        });

        tauri::Builder::default()
            .manage(state)
            .invoke_handler(tauri::generate_handler![
                list_serial_ports,
                get_logs,
                clear_logs,
                get_status,
                connect_device,
                disconnect_device,
                send_display,
                send_led,
                send_config,
                set_voice_source,
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
