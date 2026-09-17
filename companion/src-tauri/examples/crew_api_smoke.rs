//! Read-only live smoke test for the local Kiro Crew gateway.
//!
//! Run from `companion/src-tauri`:
//!   cargo run --example crew_api_smoke
//!
//! It never prints credentials and never sends a chat message. It verifies the
//! official local-token bootstrap, REST slot listing, and the first WebSocket
//! `slots` snapshot.

use agentic_keyboard_companion_lib::crew_api::{CrewApiClient, CrewEvent, CrewRealtime};
use std::sync::mpsc;
use std::time::Duration;

fn main() -> Result<(), String> {
    let client = CrewApiClient::from_env()?;
    let slots = client.list_slots()?;
    println!(
        "REST OK: endpoint={} chats={}",
        client.config().base_url(),
        slots.len()
    );

    let (tx, rx) = mpsc::channel();
    let mut worker = CrewRealtime::start(client, move |event| {
        let _ = tx.send(event);
    })?;

    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    let mut connected = false;
    let mut snapshot = None;
    while std::time::Instant::now() < deadline {
        match rx.recv_timeout(Duration::from_secs(1)) {
            Ok(CrewEvent::Connection(state)) if state.connected => connected = true,
            Ok(CrewEvent::Slots(slots)) => {
                snapshot = Some(slots.len());
                break;
            }
            Ok(CrewEvent::Connection(state)) => {
                eprintln!("WS retry: {}", state.error);
            }
            Ok(_) => {}
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(error) => return Err(format!("Crew realtime channel closed: {error}")),
        }
    }
    worker.stop();

    match (connected, snapshot) {
        (true, Some(count)) => {
            println!("WS OK: connected=true initial_slots={count}");
            Ok(())
        }
        _ => Err("WebSocket did not deliver an initial slots snapshot within 10s".to_string()),
    }
}
