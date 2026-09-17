//! Local Kiro Crew API client for the desktop Companion.
//!
//! Authentication follows the gateway's official local-app bootstrap:
//! 1. Read `<KIROCREW_HOME>/.local_secret` (default `~/.kiro/crew`).
//! 2. `GET /api/token/local` with `X-Local-Secret` to obtain a short-lived
//!    owner token.
//! 3. Use that token as the port-scoped dashboard cookie for REST and as the
//!    `/api/ws?token=...` query credential for WebSocket.
//!
//! The secret and token are never logged. Every REST operation bootstraps a
//! fresh token; every WebSocket reconnect re-reads the secret and re-issues the
//! token, so a gateway restart (which rotates `.local_secret`) self-heals.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::fs;
use std::io::ErrorKind;
use std::net::{TcpStream, ToSocketAddrs};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;
use tungstenite::client::IntoClientRequest;
use tungstenite::http::{header::ORIGIN, HeaderValue};
use tungstenite::{client, Error as WsError, Message};

pub const DEFAULT_CREW_PORT: u16 = 5476;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(3);
const READ_TIMEOUT: Duration = Duration::from_secs(1);
const RECONNECT_MIN: Duration = Duration::from_secs(1);
const RECONNECT_MAX: Duration = Duration::from_secs(15);

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CrewApiConfig {
    pub host: String,
    pub port: u16,
    pub crew_home: PathBuf,
}

impl Default for CrewApiConfig {
    fn default() -> Self {
        Self::from_env()
    }
}

impl CrewApiConfig {
    pub fn from_env() -> Self {
        let crew_home = std::env::var("KIROCREW_HOME")
            .ok()
            .filter(|v| !v.trim().is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| crate::app_dirs::home_dir().join(".kiro").join("crew"));
        let port = std::env::var("KIROCREW_PORT")
            .ok()
            .and_then(|v| v.parse::<u16>().ok())
            .filter(|p| *p != 0)
            .unwrap_or(DEFAULT_CREW_PORT);
        Self {
            host: "127.0.0.1".to_string(),
            port,
            crew_home,
        }
    }

    pub fn base_url(&self) -> String {
        format!("http://{}:{}", self.host, self.port)
    }

    pub fn ws_url(&self, token: &str) -> String {
        format!("ws://{}:{}/api/ws?token={token}", self.host, self.port)
    }

    pub fn local_secret_path(&self) -> PathBuf {
        self.crew_home.join(".local_secret")
    }

    fn socket_addr(&self) -> Result<std::net::SocketAddr, String> {
        (self.host.as_str(), self.port)
            .to_socket_addrs()
            .map_err(|e| format!("resolve Crew gateway {}:{}: {e}", self.host, self.port))?
            .next()
            .ok_or_else(|| format!("no address for Crew gateway {}:{}", self.host, self.port))
    }
}

#[derive(Clone)]
pub struct CrewApiClient {
    config: CrewApiConfig,
    http: reqwest::blocking::Client,
}

#[derive(Clone, Debug, Deserialize)]
struct LocalTokenResponse {
    token: String,
    #[allow(dead_code)]
    expires_in: Option<u64>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct CrewSlot {
    pub key: String,
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub agent: String,
    #[serde(default)]
    pub running: bool,
    #[serde(default)]
    pub stopping: bool,
    #[serde(default)]
    pub pending_approval: bool,
    #[serde(default)]
    pub waiting_for_input: bool,
    #[serde(default)]
    pub needs_input: bool,
    #[serde(default)]
    pub subagents_running: bool,
    #[serde(default)]
    pub last_message: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
pub struct SendReceipt {
    #[serde(default)]
    pub ok: bool,
    #[serde(default)]
    pub slot: String,
    #[serde(default)]
    pub mid: String,
    #[serde(default)]
    pub queued: bool,
    #[serde(default)]
    pub queue_id: String,
}

impl CrewApiClient {
    pub fn new(config: CrewApiConfig) -> Result<Self, String> {
        let http = reqwest::blocking::Client::builder()
            .connect_timeout(CONNECT_TIMEOUT)
            .timeout(Duration::from_secs(10))
            .build()
            .map_err(|e| format!("build Crew HTTP client: {e}"))?;
        Ok(Self { config, http })
    }

    pub fn from_env() -> Result<Self, String> {
        Self::new(CrewApiConfig::from_env())
    }

    pub fn config(&self) -> &CrewApiConfig {
        &self.config
    }

    /// Read the current gateway-local secret. It rotates whenever the gateway
    /// restarts, so callers must not cache it across operations/reconnects.
    pub fn read_local_secret(&self) -> Result<String, String> {
        let path = self.config.local_secret_path();
        let secret = fs::read_to_string(&path)
            .map_err(|e| format!("read Kiro Crew local secret {}: {e}", path.display()))?;
        let secret = secret.trim().to_string();
        if secret.is_empty() {
            return Err(format!(
                "Kiro Crew local secret is empty: {}",
                path.display()
            ));
        }
        Ok(secret)
    }

    /// Exchange the current `.local_secret` for a short-lived owner token using
    /// the gateway's official local-app endpoint.
    pub fn bootstrap_token(&self) -> Result<String, String> {
        let secret = self.read_local_secret()?;
        let url = format!("{}/api/token/local", self.config.base_url());
        let response = self
            .http
            .get(url)
            .header("X-Local-Secret", secret)
            .send()
            .map_err(|e| format!("bootstrap Kiro Crew token: {e}"))?
            .error_for_status()
            .map_err(|e| format!("bootstrap Kiro Crew token: {e}"))?;
        let payload: LocalTokenResponse = response
            .json()
            .map_err(|e| format!("decode Kiro Crew token response: {e}"))?;
        if payload.token.trim().is_empty() {
            return Err("Kiro Crew returned an empty local token".to_string());
        }
        Ok(payload.token)
    }

    fn auth_cookie(&self, token: &str) -> String {
        format!("mc_token_{}={token}", self.config.port)
    }

    /// List live chat slots. The owner token gives this local desktop app the
    /// same slot visibility as the dashboard itself.
    pub fn list_slots(&self) -> Result<Vec<CrewSlot>, String> {
        let token = self.bootstrap_token()?;
        self.http
            .get(format!("{}/api/chat/slots", self.config.base_url()))
            .header(reqwest::header::COOKIE, self.auth_cookie(&token))
            .send()
            .map_err(|e| format!("list Kiro Crew chats: {e}"))?
            .error_for_status()
            .map_err(|e| format!("list Kiro Crew chats: {e}"))?
            .json::<Vec<CrewSlot>>()
            .map_err(|e| format!("decode Kiro Crew chats: {e}"))
    }

    /// Send a user message to one stable slot key. `ws=1` asks the gateway for
    /// an immediate JSON receipt; response/status updates arrive on `/api/ws`.
    pub fn send_message(&self, slot: &str, message: &str) -> Result<SendReceipt, String> {
        let slot = slot.trim();
        let message = message.trim();
        if slot.is_empty() {
            return Err("Kiro Crew slot key is required".to_string());
        }
        if message.is_empty() {
            return Err("message is required".to_string());
        }
        let token = self.bootstrap_token()?;
        self.http
            .post(format!("{}/api/chat?ws=1", self.config.base_url()))
            .header(reqwest::header::COOKIE, self.auth_cookie(&token))
            .json(&json!({ "slot": slot, "message": message }))
            .send()
            .map_err(|e| format!("send Kiro Crew message: {e}"))?
            .error_for_status()
            .map_err(|e| format!("send Kiro Crew message: {e}"))?
            .json::<SendReceipt>()
            .map_err(|e| format!("decode Kiro Crew send receipt: {e}"))
    }

    pub fn continue_slot(&self, slot: &str) -> Result<(), String> {
        self.slot_action(slot, "continue")
    }

    pub fn interrupt_slot(&self, slot: &str) -> Result<(), String> {
        self.slot_action(slot, "interrupt")
    }

    pub fn stop_slot(&self, slot: &str) -> Result<(), String> {
        self.slot_action(slot, "stop")
    }

    fn slot_action(&self, slot: &str, action: &str) -> Result<(), String> {
        let slot = slot.trim();
        if slot.is_empty() {
            return Err("Kiro Crew slot key is required".to_string());
        }
        let token = self.bootstrap_token()?;
        let mut url = reqwest::Url::parse(&self.config.base_url())
            .map_err(|e| format!("build Kiro Crew URL: {e}"))?;
        url.path_segments_mut()
            .map_err(|_| "Kiro Crew base URL cannot contain path segments".to_string())?
            .extend(["api", "chat", "slots", slot, action]);
        self.http
            .post(url)
            .header(reqwest::header::COOKIE, self.auth_cookie(&token))
            .send()
            .map_err(|e| format!("Kiro Crew {action}: {e}"))?
            .error_for_status()
            .map_err(|e| format!("Kiro Crew {action}: {e}"))?;
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum CrewEvent {
    Connection(CrewConnection),
    Slots(Vec<CrewSlot>),
    ChatMessage(Value),
    Dashboard(Value),
    SlotTitle(Value),
    Other { event_type: String, data: Value },
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct CrewConnection {
    pub connected: bool,
    pub base_url: String,
    pub error: String,
    pub retry_secs: u64,
}

impl CrewEvent {
    fn parse_text(text: &str) -> Result<Self, String> {
        let frame: Value = serde_json::from_str(text)
            .map_err(|e| format!("decode Kiro Crew WebSocket frame: {e}"))?;
        let event_type = frame
            .get("type")
            .and_then(Value::as_str)
            .unwrap_or("unknown")
            .to_string();
        let data = frame.get("data").cloned().unwrap_or(Value::Null);
        match event_type.as_str() {
            "slots" => serde_json::from_value(data)
                .map(CrewEvent::Slots)
                .map_err(|e| format!("decode Kiro Crew slots frame: {e}")),
            "chat_message" => Ok(CrewEvent::ChatMessage(data)),
            "dashboard" => Ok(CrewEvent::Dashboard(data)),
            "slot_title" => Ok(CrewEvent::SlotTitle(data)),
            _ => Ok(CrewEvent::Other { event_type, data }),
        }
    }
}

/// Map one API slot projection into the existing Hook/LCD status vocabulary.
pub fn slot_display_status(slot: &CrewSlot) -> crate::status_sink::DisplayState {
    let (state, text) = if slot.pending_approval || slot.needs_input || slot.waiting_for_input {
        ("waiting", "needs your input".to_string())
    } else if slot.stopping {
        ("running", "stopping".to_string())
    } else if slot.running || slot.subagents_running {
        ("running", "agent running".to_string())
    } else if !slot.last_message.trim().is_empty() {
        ("idle", compact_text(&slot.last_message, 40))
    } else {
        ("idle", "ready".to_string())
    };
    crate::status_sink::DisplayState {
        agent_name: if slot.agent.trim().is_empty() {
            "kirocrew".to_string()
        } else {
            slot.agent.clone()
        },
        state: state.to_string(),
        text,
        event: "api_ws".to_string(),
        detail_text: slot.last_message.clone(),
        session_id: slot.key.clone(),
    }
}

fn compact_text(text: &str, max_chars: usize) -> String {
    let flat = text.split_whitespace().collect::<Vec<_>>().join(" ");
    let mut out: String = flat.chars().take(max_chars).collect();
    if flat.chars().count() > max_chars {
        out.push('…');
    }
    out
}

pub struct CrewRealtime {
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl CrewRealtime {
    pub fn start<F>(client: CrewApiClient, callback: F) -> Result<Self, String>
    where
        F: Fn(CrewEvent) + Send + 'static,
    {
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let worker = thread::Builder::new()
            .name("ak-crew-api-ws".to_string())
            .spawn(move || realtime_loop(client, worker_stop, callback))
            .map_err(|e| format!("start Kiro Crew realtime worker: {e}"))?;
        Ok(Self {
            stop,
            worker: Some(worker),
        })
    }

    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for CrewRealtime {
    fn drop(&mut self) {
        self.stop();
    }
}

fn realtime_loop<F>(client: CrewApiClient, stop: Arc<AtomicBool>, callback: F)
where
    F: Fn(CrewEvent),
{
    let mut retry = RECONNECT_MIN;
    while !stop.load(Ordering::Acquire) {
        match run_realtime_connection(&client, &stop, &callback) {
            Ok(()) if stop.load(Ordering::Acquire) => break,
            Ok(()) => {}
            Err(error) => {
                callback(CrewEvent::Connection(CrewConnection {
                    connected: false,
                    base_url: client.config.base_url(),
                    error,
                    retry_secs: retry.as_secs(),
                }));
            }
        }
        if sleep_interruptible(&stop, retry) {
            break;
        }
        retry = (retry * 2).min(RECONNECT_MAX);
    }
}

fn run_realtime_connection<F>(
    api: &CrewApiClient,
    stop: &AtomicBool,
    callback: &F,
) -> Result<(), String>
where
    F: Fn(CrewEvent),
{
    let token = api.bootstrap_token()?;
    let addr = api.config.socket_addr()?;
    let stream = TcpStream::connect_timeout(&addr, CONNECT_TIMEOUT)
        .map_err(|e| format!("connect Kiro Crew WebSocket: {e}"))?;
    stream
        .set_read_timeout(Some(READ_TIMEOUT))
        .map_err(|e| format!("set Kiro Crew WebSocket read timeout: {e}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(3)))
        .map_err(|e| format!("set Kiro Crew WebSocket write timeout: {e}"))?;

    let mut request = api
        .config
        .ws_url(&token)
        .into_client_request()
        .map_err(|e| format!("build Kiro Crew WebSocket request: {e}"))?;
    request.headers_mut().insert(
        ORIGIN,
        HeaderValue::from_str(&api.config.base_url())
            .map_err(|e| format!("build Kiro Crew WebSocket Origin: {e}"))?,
    );
    let (mut socket, _) =
        client(request, stream).map_err(|e| format!("open Kiro Crew WebSocket: {e}"))?;
    callback(CrewEvent::Connection(CrewConnection {
        connected: true,
        base_url: api.config.base_url(),
        error: String::new(),
        retry_secs: 0,
    }));

    while !stop.load(Ordering::Acquire) {
        match socket.read() {
            Ok(Message::Text(text)) => match CrewEvent::parse_text(text.as_str()) {
                Ok(event) => callback(event),
                Err(error) => callback(CrewEvent::Other {
                    event_type: "decode_error".to_string(),
                    data: Value::String(error),
                }),
            },
            Ok(Message::Ping(payload)) => {
                socket
                    .send(Message::Pong(payload))
                    .map_err(|e| format!("reply to Kiro Crew WebSocket ping: {e}"))?;
            }
            Ok(Message::Close(_)) => return Err("Kiro Crew WebSocket closed".to_string()),
            Ok(_) => {}
            Err(WsError::Io(error))
                if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(WsError::ConnectionClosed | WsError::AlreadyClosed) => {
                return Err("Kiro Crew WebSocket disconnected".to_string())
            }
            Err(error) => return Err(format!("read Kiro Crew WebSocket: {error}")),
        }
    }
    let _ = socket.close(None);
    Ok(())
}

/// Returns true when stop was requested while waiting.
fn sleep_interruptible(stop: &AtomicBool, duration: Duration) -> bool {
    let step = Duration::from_millis(100);
    let mut elapsed = Duration::ZERO;
    while elapsed < duration {
        if stop.load(Ordering::Acquire) {
            return true;
        }
        let nap = (duration - elapsed).min(step);
        thread::sleep(nap);
        elapsed += nap;
    }
    stop.load(Ordering::Acquire)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_builds_expected_urls() {
        let cfg = CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 5476,
            crew_home: PathBuf::from("C:/Users/test/.kiro/crew"),
        };
        assert_eq!(cfg.base_url(), "http://127.0.0.1:5476");
        assert_eq!(
            cfg.ws_url("abc.def"),
            "ws://127.0.0.1:5476/api/ws?token=abc.def"
        );
        assert!(cfg.local_secret_path().ends_with(".local_secret"));
    }

    #[test]
    fn parses_slots_websocket_frame() {
        let event = CrewEvent::parse_text(
            r#"{"type":"slots","data":[{"key":"chat-7","title":"Test","agent":"kirocrew","running":true,"last_message":"working"}]}"#,
        )
        .unwrap();
        match event {
            CrewEvent::Slots(slots) => {
                assert_eq!(slots.len(), 1);
                assert_eq!(slots[0].key, "chat-7");
                assert!(slots[0].running);
            }
            other => panic!("unexpected event: {other:?}"),
        }
    }

    #[test]
    fn maps_slot_state_priority() {
        let waiting = CrewSlot {
            key: "chat-a".into(),
            title: "A".into(),
            running: true,
            pending_approval: true,
            ..Default::default()
        };
        let mapped = slot_display_status(&waiting);
        assert_eq!(mapped.state, "waiting");
        assert_eq!(mapped.session_id, "chat-a");

        let running = CrewSlot {
            key: "chat-b".into(),
            running: true,
            ..Default::default()
        };
        assert_eq!(slot_display_status(&running).state, "running");

        let idle = CrewSlot {
            key: "chat-c".into(),
            last_message: "all done".into(),
            ..Default::default()
        };
        assert_eq!(slot_display_status(&idle).text, "all done");
    }

    #[test]
    fn compact_text_is_char_safe() {
        assert_eq!(compact_text("a  b\nc", 10), "a b c");
        assert_eq!(compact_text("你好世界", 3), "你好世…");
    }

    #[test]
    fn rejects_empty_send_inputs_before_network() {
        let client = CrewApiClient::new(CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 1,
            crew_home: PathBuf::from("missing"),
        })
        .unwrap();
        assert!(client
            .send_message("", "hello")
            .unwrap_err()
            .contains("slot"));
        assert!(client
            .send_message("chat", " ")
            .unwrap_err()
            .contains("message"));
        assert!(client.continue_slot(" ").unwrap_err().contains("slot"));
        assert!(client.interrupt_slot("").unwrap_err().contains("slot"));
        assert!(client.stop_slot("").unwrap_err().contains("slot"));
    }
}
