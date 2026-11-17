//! Local Kiro Crew API client for the desktop Companion.
//!
//! Authentication follows the gateway's official local-app bootstrap:
//! 1. Read `<KIROCREW_HOME>/.local_secret` (default `~/.kiro/crew`).
//! 2. `GET /api/token/local` with `X-Local-Secret` to obtain a short-lived
//!    owner LINK token, and trade it once (`?token=` on `/api/version`) for a
//!    session token from the `mc_token_<port>` Set-Cookie (Crew 0.12+).
//! 3. Use the session token as the port-scoped dashboard cookie
//!    (`mc_token_<port>`) for REST and for the `/api/ws` WebSocket handshake.
//!    Only the single-use link token ever travels in a URL (CP-14), and it
//!    never appears in error text or logs.
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
use tungstenite::http::{
    header::{COOKIE, ORIGIN},
    HeaderValue,
};
use tungstenite::{client, Error as WsError, Message};

pub const DEFAULT_CREW_PORT: u16 = 5476;
/// Longest chat message the Companion sends through the API (CP-15).
pub const MAX_MESSAGE_BYTES: usize = 8 * 1024;
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
    /// Release builds always use `~/.kiro/crew` and port 5476. The
    /// `KIROCREW_HOME` / `KIROCREW_PORT` overrides exist in debug builds only
    /// (CP-09): in a release build an environment variable must not be able
    /// to point the Companion's secret at another directory or port.
    pub fn from_env() -> Self {
        let default_home = crate::app_dirs::home_dir().join(".kiro").join("crew");
        if !cfg!(debug_assertions) {
            return Self { host: "127.0.0.1".to_string(), port: DEFAULT_CREW_PORT, crew_home: default_home };
        }
        let crew_home = std::env::var("KIROCREW_HOME")
            .ok()
            .filter(|v| !v.trim().is_empty())
            .map(PathBuf::from)
            .unwrap_or(default_home);
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

    pub fn ws_url(&self) -> String {
        format!("ws://{}:{}/api/ws", self.host, self.port)
    }

    pub fn local_secret_path(&self) -> PathBuf {
        self.crew_home.join(".local_secret")
    }

    /// Where the gateway's executable must live: the parent of the Crew home
    /// (`~/.kiro`), see `crew_identity`.
    pub fn install_root(&self) -> PathBuf {
        self.crew_home.parent().map(PathBuf::from).unwrap_or_else(|| self.crew_home.clone())
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
    /// Session token shared by every clone of this client.
    tokens: TokenCache,
    /// Who owns the Crew port (CP-09); the OS query unless a test injects one.
    identity: Arc<dyn crate::crew_identity::ListenerQuery + Send + Sync>,
}

/// The exchanged Crew session token, kept in process memory only: never
/// written to disk, sent to the frontend, logged, or `Debug`-printed.
#[derive(Clone, Default)]
struct TokenCache(Arc<std::sync::Mutex<Option<String>>>);

impl TokenCache {
    fn get(&self) -> Option<String> {
        self.0.lock().ok().and_then(|t| t.clone())
    }
    fn set(&self, token: &str) {
        if let Ok(mut t) = self.0.lock() {
            *t = Some(token.to_string());
        }
    }
    /// Forget `token` unless another thread already replaced it.
    fn clear_if(&self, token: &str) {
        if let Ok(mut t) = self.0.lock() {
            if t.as_deref() == Some(token) {
                *t = None;
            }
        }
    }
}

/// Run `op` with the cached session token (minting one when none is cached).
/// `op` returns its value and whether the gateway refused the token
/// (HTTP 401/403). A refused token is dropped, a fresh one is minted, and
/// `op` runs exactly once more; the second answer is returned as is.
fn with_auth_retry<T>(
    cache: &TokenCache,
    mut mint: impl FnMut() -> Result<String, String>,
    mut op: impl FnMut(&str) -> Result<(T, bool), String>,
) -> Result<T, String> {
    let token = match cache.get() {
        Some(t) => t,
        None => {
            let t = mint()?;
            cache.set(&t);
            t
        }
    };
    let (value, refused) = op(&token)?;
    if !refused {
        return Ok(value);
    }
    cache.clear_if(&token);
    let fresh = mint()?;
    cache.set(&fresh);
    let (value, refused) = op(&fresh)?;
    if refused {
        cache.clear_if(&fresh);
    }
    Ok(value)
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
    /// The newest pending tool approval (`origin` native or coordinator).
    #[serde(default)]
    pub pending_approval_info: Option<PendingApprovalInfo>,
    /// `[OPTIONS: ...]` choices parsed from the last assistant reply.
    #[serde(default, deserialize_with = "null_default")]
    pub has_options: bool,
    #[serde(default, deserialize_with = "null_default")]
    pub options: Vec<String>,
    /// Timestamp of the reply the options came from (Crew sends a string;
    /// kept as an opaque key).
    #[serde(default, deserialize_with = "lenient_string")]
    pub options_ts: String,
    #[serde(default, deserialize_with = "null_default")]
    pub prompt_preview: String,
    /// Pinned model; empty or `auto` means the session follows Auto.
    #[serde(default, deserialize_with = "null_default")]
    pub model: String,
    #[serde(default, deserialize_with = "null_default")]
    pub served_model: String,
    /// Approval mode flags: `trust` = Trust, `trust_reads` = Reads, neither = Normal.
    #[serde(default, deserialize_with = "null_default")]
    pub trust: bool,
    #[serde(default, deserialize_with = "null_default")]
    pub trust_reads: bool,
    /// `member` on a Bot (crewmate) thread; empty on an ordinary session.
    #[serde(default, deserialize_with = "null_default")]
    pub mode: String,
    /// Set on a Crew Mode worker row: the root session's key.
    #[serde(default, deserialize_with = "null_default")]
    pub crew_root: String,
    /// Set on a session a Bot dispatched: the Bot thread's key.
    #[serde(default, deserialize_with = "null_default")]
    pub mate_root: String,
}

impl CrewSlot {
    /// A Bot (crewmate) thread. Remote rows carry no `mode`; their derived
    /// `member-` key prefix (`members_parts/member_sessions.py`) tells instead.
    pub fn is_bot(&self) -> bool {
        self.mode == "member" || self.key.starts_with("member-")
    }

    /// A worker session: dispatched by a Crew Mode root or by a Bot.
    pub fn is_worker(&self) -> bool {
        !self.crew_root.is_empty() || !self.mate_root.is_empty()
    }
}

/// One usable remote crew from `GET /api/instances`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CrewInstance {
    pub id: String,
    pub name: String,
}

/// The remote crews worth listing: not disabled, and connected now or before,
/// the same filter the dashboard's instance switcher applies (Ctrl+2.. follow
/// this order). Anything malformed is skipped.
pub fn parse_instances(v: &Value) -> Vec<CrewInstance> {
    v.get("instances")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|i| !i.get("disabled").and_then(Value::as_bool).unwrap_or(false))
        .filter(|i| {
            i.get("was_connected").and_then(Value::as_bool).unwrap_or(false)
                || i.pointer("/status/state").and_then(Value::as_str) == Some("connected")
        })
        .filter_map(|i| {
            let id = i.get("id").and_then(Value::as_str)?.trim();
            validate_instance_id(id).ok()?;
            let name = i.get("name").and_then(Value::as_str).map(str::trim).filter(|n| !n.is_empty()).unwrap_or(id);
            Some(CrewInstance { id: id.to_string(), name: name.to_string() })
        })
        .collect()
}

/// A Crew instance id: the slug of its name (`instances/registry.py`).
pub fn validate_instance_id(id: &str) -> Result<&str, String> {
    let ok = !id.is_empty()
        && id.len() <= 64
        && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'-' | b'_'));
    if ok { Ok(id) } else { Err(format!("invalid Kiro Crew instance id {id:?}")) }
}

/// `pending_approval_info` of a slot projection (fields are redacted by Crew).
#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
pub struct PendingApprovalInfo {
    #[serde(default, deserialize_with = "null_default")]
    pub origin: String,
    #[serde(default, deserialize_with = "null_default")]
    pub tool: String,
    #[serde(default, deserialize_with = "null_default")]
    pub tool_input: String,
    #[serde(default, deserialize_with = "null_default")]
    pub tool_kind: String,
    #[serde(default, deserialize_with = "null_default")]
    pub request_id: String,
    #[serde(default, deserialize_with = "null_default")]
    pub request_mid: String,
    #[serde(default, deserialize_with = "null_default")]
    pub tool_purpose: String,
}

/// Treat an explicit JSON `null` like a missing field, so one null in a slot
/// projection does not fail the whole `slots` frame.
fn null_default<'de, D, T>(d: D) -> Result<T, D::Error>
where
    D: serde::Deserializer<'de>,
    T: Default + Deserialize<'de>,
{
    Ok(Option::<T>::deserialize(d)?.unwrap_or_default())
}

/// A string, number or null as an opaque string key.
fn lenient_string<'de, D>(d: D) -> Result<String, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Ok(match Option::<Value>::deserialize(d)? {
        Some(Value::String(s)) => s,
        Some(Value::Number(n)) => n.to_string(),
        _ => String::new(),
    })
}

/// One entry of `GET /api/models`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq, Eq)]
pub struct CrewModel {
    pub model_name: String,
    #[serde(default, deserialize_with = "null_default")]
    pub display_name: String,
}

/// One pending question card from `GET /api/chat/slots/{slot}/question`.
/// A blocking MCP `ask_question` card has `ask_id`; a stateless or native
/// (kiro-cli `AskUserQuestion`) card has `card_id`.
#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct QuestionCard {
    #[serde(default, deserialize_with = "null_default")]
    pub ask_id: String,
    #[serde(default, deserialize_with = "null_default")]
    pub card_id: String,
    #[serde(default, deserialize_with = "null_default")]
    pub slot: String,
    #[serde(default, deserialize_with = "null_default")]
    pub questions: Vec<CardQuestion>,
    #[serde(default, deserialize_with = "null_default")]
    pub native: bool,
    #[serde(default, deserialize_with = "lenient_string")]
    pub ts: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct CardQuestion {
    #[serde(default, deserialize_with = "null_default")]
    pub question: String,
    #[serde(default, deserialize_with = "null_default")]
    pub header: String,
    #[serde(default, deserialize_with = "null_default")]
    pub options: Vec<CardOption>,
    #[serde(default, rename = "multiSelect", deserialize_with = "null_default")]
    pub multi_select: bool,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct CardOption {
    #[serde(default, deserialize_with = "null_default")]
    pub label: String,
}

/// One suggestion of a `followup_card` broadcast.
#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct FollowupItem {
    #[serde(default, deserialize_with = "null_default")]
    pub title: String,
    #[serde(default, deserialize_with = "null_default")]
    pub prompt: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
pub struct FollowupCard {
    #[serde(default, deserialize_with = "null_default")]
    pub slot: String,
    #[serde(default, deserialize_with = "null_default")]
    pub items: Vec<FollowupItem>,
    #[serde(default, deserialize_with = "lenient_string")]
    pub ts: String,
}

/// Approval decisions the Companion may send. The native, mid-bound form of
/// `/approve` accepts only these three (Crew `slot_approval.py`), so the
/// keyboard never offers a trust grant.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ApprovalAction {
    Approved,
    Rejected,
    RejectedOnce,
}

impl ApprovalAction {
    pub fn wire(self) -> &'static str {
        match self {
            ApprovalAction::Approved => "approved",
            ApprovalAction::Rejected => "rejected",
            ApprovalAction::RejectedOnce => "rejected_once",
        }
    }
}

/// Approval modes the keyboard may set. YOLO is deliberately absent: it can
/// only be turned on in the dashboard (FR-10).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ApprovalMode {
    Normal,
    Reads,
    Trust,
}

impl ApprovalMode {
    pub fn wire(self) -> &'static str {
        match self {
            ApprovalMode::Normal => "normal",
            ApprovalMode::Reads => "trust_reads",
            ApprovalMode::Trust => "trust",
        }
    }
    pub fn of_slot(slot: &CrewSlot) -> ApprovalMode {
        if slot.trust {
            ApprovalMode::Trust
        } else if slot.trust_reads {
            ApprovalMode::Reads
        } else {
            ApprovalMode::Normal
        }
    }
}

/// `POST /api/chat/slots/{slot}/model` result.
#[derive(Clone, Debug, Default, Deserialize, PartialEq, Eq)]
pub struct ModelSetReceipt {
    #[serde(default)]
    pub ok: bool,
    #[serde(default, deserialize_with = "null_default")]
    pub model: String,
    #[serde(default)]
    pub deferred: bool,
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
            // Never follow a redirect: the token exchange URL carries a link
            // token that must reach only the loopback gateway.
            .redirect(reqwest::redirect::Policy::none())
            .build()
            .map_err(|e| format!("build Crew HTTP client: {e}"))?;
        Ok(Self { config, http, tokens: TokenCache::default(), identity: Arc::from(crate::crew_identity::system_query()) })
    }

    /// Replace the port-owner query (tests).
    pub fn with_identity_query(mut self, q: Arc<dyn crate::crew_identity::ListenerQuery + Send + Sync>) -> Self {
        self.identity = q;
        self
    }

    /// CP-09: the process on 127.0.0.1:<port> must be this user's Kiro Crew
    /// gateway before the secret or a token is sent to it. Fails closed.
    pub fn verify_gateway(&self) -> Result<u32, String> {
        crate::crew_identity::verify_gateway(self.identity.as_ref(), self.config.port, &self.config.install_root())
            .map_err(|e| e.to_string())
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
        check_secret_permissions(&path)?;
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
    /// the gateway's official local-app endpoint, then exchange that one-time
    /// link token for a session token.
    ///
    /// Since Crew 0.12 `/api/token/local` returns a LINK token: the cookie path
    /// refuses it ("link token is not a session"). The gateway's middleware
    /// trades a link presented once as `?token=` for a `kind=session` token in
    /// the `mc_token_<port>` Set-Cookie. That one request carries the link in
    /// its URL; the link is single-use, expires within minutes, goes only to
    /// the loopback gateway, and never appears in our errors or logs.
    pub fn bootstrap_token(&self) -> Result<String, String> {
        let link = self.local_link_token()?;
        let mut url = self.api_url(&["api", "version"])?;
        url.query_pairs_mut().append_pair("token", &link);
        let resp = self
            .http
            .get(url)
            .send()
            .map_err(|e| format!("exchange Kiro Crew token: {}", e.without_url()))?;
        if !resp.status().is_success() {
            return Err(format!("exchange Kiro Crew token: HTTP {}", resp.status().as_u16()));
        }
        let cookie = format!("mc_token_{}", self.config.port);
        let values = resp
            .headers()
            .get_all(reqwest::header::SET_COOKIE)
            .iter()
            .filter_map(|v| v.to_str().ok().map(str::to_string))
            .collect::<Vec<_>>();
        // A gateway that predates the link/session split sets no cookie and
        // accepts the token as is.
        Ok(session_cookie_value(values.iter().map(String::as_str), &cookie).unwrap_or(link))
    }

    fn local_link_token(&self) -> Result<String, String> {
        // Identity first: nothing is read or sent before the listener passes.
        self.verify_gateway()?;
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
        // `crew=all`: a person's list leaves Crew Mode workers out by default
        // (`crew_mode.list_view`); the Companion filters them itself
        // (CrewSlot::is_worker), like the dashboard does.
        let mut url = self.api_url(&["api", "chat", "slots"])?;
        url.set_query(Some("crew=all"));
        let resp = self.send_authed("list chats", |cookie| self.http.get(url.clone()).header(reqwest::header::COOKIE, cookie))?;
        serde_json::from_value(read_json_response(resp, "list chats")?)
            .map_err(|e| format!("decode Kiro Crew chats: {e}"))
    }

    /// Remote crews (`GET /api/instances`, owner-only). The feature being off
    /// (`instances_disabled`) or absent means none.
    /// shortcut: that 403 also re-mints the session token once (send_authed);
    /// cache "disabled" if it shows up in the gateway's rate limits.
    pub fn list_instances(&self) -> Result<Vec<CrewInstance>, String> {
        match self.get_json(&["api", "instances"], "list instances") {
            Ok(v) => Ok(parse_instances(&v)),
            Err(e) if e.contains("instances_disabled") || e.contains("HTTP 404") => Ok(Vec::new()),
            Err(e) => Err(e),
        }
    }

    /// A remote crew's live sessions through the Hub
    /// (`GET /api/instances/{id}/chat-slots`). Rows carry only key, title,
    /// agent, running / pending_approval / needs_input and `crew_root`.
    pub fn list_remote_slots(&self, instance: &str) -> Result<Vec<CrewSlot>, String> {
        let instance = validate_instance_id(instance)?;
        let v = self.get_json(&["api", "instances", instance, "chat-slots"], "list remote chats")?;
        serde_json::from_value(v).map_err(|e| format!("decode remote Kiro Crew chats: {e}"))
    }

    /// Send a user message to one stable slot key. `ws=1` asks the gateway for
    /// an immediate JSON receipt; response/status updates arrive on `/api/ws`.
    pub fn send_message(&self, slot: &str, message: &str) -> Result<SendReceipt, String> {
        let slot = validate_slot_key(slot)?;
        let message = message.trim();
        if message.is_empty() {
            return Err("message is required".to_string());
        }
        if message.len() > MAX_MESSAGE_BYTES {
            return Err(format!(
                "message is too long ({} bytes, limit {MAX_MESSAGE_BYTES})",
                message.len()
            ));
        }
        let mut url = self.api_url(&["api", "chat"])?;
        url.set_query(Some("ws=1"));
        let body = json!({ "slot": slot, "message": message });
        let resp = self.send_authed("send message", |cookie| {
            self.http.post(url.clone()).header(reqwest::header::COOKIE, cookie).json(&body)
        })?;
        serde_json::from_value(read_json_response(resp, "send message")?)
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
        let slot = validate_slot_key(slot)?;
        self.post_json(&["api", "chat", "slots", slot, action], None, action)?;
        Ok(())
    }

    /// Build `<base>/<segments...>`; each segment is percent-encoded, so an
    /// id from the gateway cannot add a path or query.
    fn api_url(&self, segments: &[&str]) -> Result<reqwest::Url, String> {
        let mut url = reqwest::Url::parse(&self.config.base_url())
            .map_err(|e| format!("build Kiro Crew URL: {e}"))?;
        url.path_segments_mut()
            .map_err(|_| "Kiro Crew base URL cannot contain path segments".to_string())?
            .extend(segments);
        Ok(url)
    }

    /// The cached session token, minting one when none is cached. For the
    /// WebSocket handshake; REST goes through [`Self::send_authed`].
    fn session_token(&self) -> Result<String, String> {
        if let Some(t) = self.tokens.get() {
            return Ok(t);
        }
        let t = self.bootstrap_token()?;
        self.tokens.set(&t);
        Ok(t)
    }

    /// Send a request built with the session cookie; on 401/403 re-mint the
    /// token and send it once more (a refused request changed nothing).
    fn send_authed(
        &self,
        what: &str,
        build: impl Fn(&str) -> reqwest::blocking::RequestBuilder,
    ) -> Result<reqwest::blocking::Response, String> {
        // A cached token is an owner credential too: check the listener
        // before every request, not only before minting.
        self.verify_gateway()?;
        with_auth_retry(&self.tokens, || self.bootstrap_token(), |token| {
            let resp = build(&self.auth_cookie(token))
                .send()
                .map_err(|e| format!("Kiro Crew {what}: {e}"))?;
            let refused = matches!(resp.status().as_u16(), 401 | 403);
            Ok((resp, refused))
        })
    }

    fn post_json(&self, segments: &[&str], body: Option<&Value>, what: &str) -> Result<Value, String> {
        let url = self.api_url(segments)?;
        let resp = self.send_authed(what, |cookie| {
            let req = self.http.post(url.clone()).header(reqwest::header::COOKIE, cookie);
            match body {
                Some(body) => req.json(body),
                None => req,
            }
        })?;
        read_json_response(resp, what)
    }

    fn get_json(&self, segments: &[&str], what: &str) -> Result<Value, String> {
        let url = self.api_url(segments)?;
        let resp = self.send_authed(what, |cookie| {
            self.http.get(url.clone()).header(reqwest::header::COOKIE, cookie)
        })?;
        read_json_response(resp, what)
    }

    /// Decide one native tool approval (FR-04). `request_mid` binds the
    /// decision to the exact request the keyboard showed.
    pub fn approve(
        &self,
        slot: &str,
        request_id: &str,
        request_mid: &str,
        action: ApprovalAction,
    ) -> Result<(), String> {
        let slot = validate_slot_key(slot)?;
        if request_id.trim().is_empty() || request_mid.trim().is_empty() {
            return Err("approval request_id and request_mid are required".to_string());
        }
        let body = approve_body(request_id, request_mid, action);
        self.post_json(&["api", "chat", "slots", slot, "approve"], Some(&body), "approve")?;
        Ok(())
    }

    /// `GET /api/models` (includes `auto`).
    pub fn list_models(&self) -> Result<Vec<CrewModel>, String> {
        let v = self.get_json(&["api", "models"], "list models")?;
        serde_json::from_value(v).map_err(|e| format!("decode Kiro Crew models: {e}"))
    }

    /// Set one slot's model (FR-06). `deferred` = applies after the running turn.
    pub fn set_model(&self, slot: &str, model: &str) -> Result<ModelSetReceipt, String> {
        let slot = validate_slot_key(slot)?;
        let model = model.trim();
        if model.is_empty() {
            return Err("model is required".to_string());
        }
        let v = self.post_json(&["api", "chat", "slots", slot, "model"], Some(&json!({ "model": model })), "set model")?;
        serde_json::from_value(v).map_err(|e| format!("decode Kiro Crew model receipt: {e}"))
    }

    /// Set one slot's approval mode (FR-10). The slot is mandatory: Crew reads
    /// an empty slot as "every session".
    pub fn set_mode(&self, slot: &str, mode: ApprovalMode) -> Result<(), String> {
        let slot = validate_slot_key(slot)?;
        let body = json!({ "mode": mode.wire(), "slot": slot });
        self.post_json(&["api", "chat", "mode"], Some(&body), "set mode")?;
        Ok(())
    }

    /// Pending question cards of one slot (FR-07).
    pub fn slot_questions(&self, slot: &str) -> Result<Vec<QuestionCard>, String> {
        let slot = validate_slot_key(slot)?;
        let v = self.get_json(&["api", "chat", "slots", slot, "question"], "list questions")?;
        serde_json::from_value(v.get("pending").cloned().unwrap_or(Value::Array(Vec::new())))
            .map_err(|e| format!("decode Kiro Crew question cards: {e}"))
    }

    /// Answer a blocking `ask_question` card: `answers` maps question text -> answer.
    pub fn answer_ask(&self, ask_id: &str, answers: &[(String, String)]) -> Result<(), String> {
        if ask_id.trim().is_empty() || answers.is_empty() {
            return Err("ask_id and answers are required".to_string());
        }
        let map: serde_json::Map<String, Value> =
            answers.iter().map(|(q, a)| (q.clone(), Value::String(a.clone()))).collect();
        self.post_json(&["api", "ask-question", ask_id, "answer"], Some(&json!({ "answers": map })), "answer question")?;
        Ok(())
    }

    /// Steer a native card's answer into the running turn. Returns false when
    /// no turn was running (`unavailable`), so the caller can send it normally.
    pub fn steer_question(&self, slot: &str, card_id: &str, message: &str) -> Result<bool, String> {
        let slot = validate_slot_key(slot)?;
        if card_id.trim().is_empty() || message.trim().is_empty() {
            return Err("card_id and message are required".to_string());
        }
        let v = self.post_json(
            &["api", "chat", "slots", slot, "question", "steer"],
            Some(&json!({ "card_id": card_id, "message": message })),
            "steer answer",
        )?;
        Ok(!v.get("unavailable").and_then(Value::as_bool).unwrap_or(false))
    }
}

/// Value of cookie `name` among `Set-Cookie` header values, if non-empty.
fn session_cookie_value<'a>(set_cookies: impl Iterator<Item = &'a str>, name: &str) -> Option<String> {
    set_cookies.into_iter().find_map(|header| {
        let (pair, _) = header.split_once(';').unwrap_or((header, ""));
        let (k, v) = pair.split_once('=')?;
        (k.trim() == name && !v.trim().is_empty()).then(|| v.trim().to_string())
    })
}

fn approve_body(request_id: &str, request_mid: &str, action: ApprovalAction) -> Value {
    json!({
        "origin": "native",
        "action": action.wire(),
        "request_id": request_id,
        "request_mid": request_mid,
    })
}

/// Map an HTTP response to JSON, surfacing the gateway's `error`/`code` on a
/// non-2xx status (never the request, which may carry the cookie).
fn read_json_response(resp: reqwest::blocking::Response, what: &str) -> Result<Value, String> {
    let status = resp.status();
    let body: Value = resp.json().unwrap_or(Value::Null);
    if status.is_success() {
        return Ok(body);
    }
    let code = body.get("code").and_then(Value::as_str).unwrap_or("");
    let error = body.get("error").and_then(Value::as_str).unwrap_or("");
    Err(format!("Kiro Crew {what}: HTTP {} {code} {error}", status.as_u16()).trim_end().to_string())
}


/// Validate a Crew slot key against the gateway's own format
/// (`^[A-Za-z0-9][A-Za-z0-9._:-]{0,199}$`, kiro_crew `cli_open._SLOT_KEY_RE`),
/// so a key can never smuggle a path segment or query into a request (CP-15).
pub fn validate_slot_key(slot: &str) -> Result<&str, String> {
    let slot = slot.trim();
    if slot.is_empty() {
        return Err("Kiro Crew slot key is required".to_string());
    }
    let mut chars = slot.chars();
    let first_ok = chars.next().is_some_and(|c| c.is_ascii_alphanumeric());
    let rest_ok = chars.all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '_' | ':' | '-'));
    if !first_ok || !rest_ok || slot.len() > 200 {
        return Err(format!("invalid Kiro Crew slot key {slot:?}"));
    }
    Ok(slot)
}

/// Refuse a `.local_secret` that is not this user's private file (Unix): it
/// must be owned by the current user and have mode exactly 0600, as Crew
/// writes it. Anything else was not written by Crew for this user, or has
/// leaked (CP-09). Windows relies on the per-user profile ACL.
fn check_secret_permissions(path: &std::path::Path) -> Result<(), String> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        let meta = fs::symlink_metadata(path)
            .map_err(|e| format!("read Kiro Crew local secret {}: {e}", path.display()))?;
        secret_file_ok(meta.file_type().is_file(), meta.uid(), meta.mode(), crate::crew_identity::current_uid())
            .map_err(|why| format!("Kiro Crew local secret {} {why}; refusing to use it", path.display()))?;
    }
    #[cfg(not(unix))]
    let _ = path;
    Ok(())
}

/// The Unix rule above, pure for tests.
#[cfg(unix)]
fn secret_file_ok(regular: bool, owner: u32, mode: u32, me: u32) -> Result<(), String> {
    if !regular {
        return Err("is not a regular file".into());
    }
    if owner != me {
        return Err(format!("is owned by uid {owner}, not the current user ({me})"));
    }
    if mode & 0o777 != 0o600 {
        return Err(format!("has mode {:o}, expected 600", mode & 0o777));
    }
    Ok(())
}

#[derive(Clone, Debug, PartialEq)]
pub enum CrewEvent {
    Connection(CrewConnection),
    Slots(Vec<CrewSlot>),
    ChatMessage(Value),
    Dashboard(Value),
    SlotTitle(Value),
    /// One-shot follow-up suggestions (owner WS only; not stored by Crew).
    Followup(FollowupCard),
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
            "followup_card" => serde_json::from_value(data)
                .map(CrewEvent::Followup)
                .map_err(|e| format!("decode Kiro Crew followup_card frame: {e}")),
            _ => Ok(CrewEvent::Other { event_type, data }),
        }
    }
}

/// Map one API slot projection into the existing Hook/LCD status vocabulary.
pub fn slot_display_status(slot: &CrewSlot) -> crate::status_sink::DisplayState {
    // Board color vocabulary: waiting=orange, running=red, info=blue, idle=green.
    let (state, text) = if slot.pending_approval || slot.needs_input || slot.waiting_for_input {
        // The agent is waiting on the user. Surface WHAT it is asking (its last
        // message) so the keyboard screen shows the actual prompt, not a generic
        // line. Fall back to the generic text only when there is no message yet.
        if slot.last_message.trim().is_empty() {
            ("waiting", "needs your input".to_string())
        } else {
            ("waiting", compact_text(&slot.last_message, 80))
        }
    } else if slot.stopping {
        ("running", "stopping".to_string())
    } else if slot.running || slot.subagents_running {
        ("running", "agent running".to_string())
    } else if !slot.last_message.trim().is_empty() {
        // Finished with a reply to read but nothing to act on -> info (blue).
        ("info", compact_text(&slot.last_message, 40))
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
    let token = api.session_token()?;
    // The cached token is sent in the handshake: check the listener first.
    api.verify_gateway()?;
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
        .ws_url()
        .into_client_request()
        .map_err(|e| format!("build Kiro Crew WebSocket request: {e}"))?;
    request.headers_mut().insert(
        COOKIE,
        HeaderValue::from_str(&api.auth_cookie(&token))
            .map_err(|_| "Kiro Crew returned a token that is not a valid cookie value".to_string())?,
    );
    request.headers_mut().insert(
        ORIGIN,
        HeaderValue::from_str(&api.config.base_url())
            .map_err(|e| format!("build Kiro Crew WebSocket Origin: {e}"))?,
    );
    let (mut socket, _) =
        client(request, stream).map_err(|e| {
            // A refused handshake means the cached session token is stale:
            // drop it so the reconnect mints a fresh one.
            if let tungstenite::HandshakeError::Failure(WsError::Http(resp)) = &e {
                if matches!(resp.status().as_u16(), 401 | 403) {
                    api.tokens.clear_if(&token);
                }
            }
            format!("open Kiro Crew WebSocket: {e}")
        })?;
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
    fn instances_list_keeps_the_switcher_order_and_usable_rows() {
        let v = json!({"active": true, "instances": [
            {"id": "remote1", "name": "Remote 1", "was_connected": true, "status": {"state": "disconnected"}},
            {"id": "off", "name": "Off", "disabled": true, "was_connected": true},
            {"id": "never", "name": "Never", "was_connected": false, "status": {"state": "disconnected"}},
            {"id": "lab-2", "name": "", "status": {"state": "connected"}},
            {"id": "Bad Id", "was_connected": true}
        ]});
        assert_eq!(
            parse_instances(&v),
            vec![
                CrewInstance { id: "remote1".into(), name: "Remote 1".into() },
                CrewInstance { id: "lab-2".into(), name: "lab-2".into() },
            ]
        );
        assert!(parse_instances(&json!({})).is_empty());
        assert!(validate_instance_id("../x").is_err());
    }

    #[test]
    fn bots_and_workers_are_recognised() {
        let bot: CrewSlot = serde_json::from_value(json!({"key": "member-mate.memory-member-mate-ab", "mode": "member"})).unwrap();
        let remote_bot: CrewSlot = serde_json::from_value(json!({"key": "member-ops"})).unwrap();
        let worker: CrewSlot = serde_json::from_value(json!({"key": "chat-9-1", "mate_root": "member-mate"})).unwrap();
        let crew_worker: CrewSlot = serde_json::from_value(json!({"key": "chat-8-1", "crew_root": "chat-1-1", "mode": null})).unwrap();
        let plain: CrewSlot = serde_json::from_value(json!({"key": "chat-7-1"})).unwrap();
        assert!(bot.is_bot() && remote_bot.is_bot() && !plain.is_bot());
        assert!(worker.is_worker() && crew_worker.is_worker() && !bot.is_worker() && !plain.is_worker());
    }

    #[test]
    fn config_builds_expected_urls() {
        let cfg = CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 5476,
            crew_home: PathBuf::from("C:/Users/test/.kiro/crew"),
        };
        assert_eq!(cfg.base_url(), "http://127.0.0.1:5476");
        assert_eq!(cfg.ws_url(), "ws://127.0.0.1:5476/api/ws");
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
        // Waiting with no message yet -> generic fallback text.
        let waiting = CrewSlot {
            key: "chat-a".into(),
            title: "A".into(),
            running: true,
            pending_approval: true,
            ..Default::default()
        };
        let mapped = slot_display_status(&waiting);
        assert_eq!(mapped.state, "waiting");
        assert_eq!(mapped.text, "needs your input");
        assert_eq!(mapped.session_id, "chat-a");

        // Waiting WITH a message -> surface the actual prompt on the screen,
        // not the generic line, so the user knows what is being asked.
        let waiting_prompt = CrewSlot {
            key: "chat-a2".into(),
            needs_input: true,
            last_message: "Approve deleting 3 files? (y/n)".into(),
            ..Default::default()
        };
        let mapped_prompt = slot_display_status(&waiting_prompt);
        assert_eq!(mapped_prompt.state, "waiting");
        assert_eq!(mapped_prompt.text, "Approve deleting 3 files? (y/n)");

        let running = CrewSlot {
            key: "chat-b".into(),
            running: true,
            ..Default::default()
        };
        assert_eq!(slot_display_status(&running).state, "running");

        // Finished with a reply but nothing to act on -> info (blue), text kept.
        let info = CrewSlot {
            key: "chat-c".into(),
            last_message: "all done".into(),
            ..Default::default()
        };
        assert_eq!(slot_display_status(&info).state, "info");
        assert_eq!(slot_display_status(&info).text, "all done");

        // Truly idle (no last message) -> idle (green).
        let idle = CrewSlot {
            key: "chat-d".into(),
            ..Default::default()
        };
        assert_eq!(slot_display_status(&idle).state, "idle");
        assert_eq!(slot_display_status(&idle).text, "ready");
    }

    /// Recorded shape of a 0.12.2 `slots` frame (field names from
    /// `slot_projection.py`), including the nulls Crew emits.
    const SLOTS_FRAME: &str = r#"{"type":"slots","data":[
      {"key":"dashboard:chat-1","title":"Plan","agent":"kirocrew","running":true,
       "pending_approval":true,
       "pending_approval_info":{"origin":"native","tool":"Run shell","tool_input":"git push",
         "tool_kind":"execute","request_id":"tc-9","request_mid":"m-41","trust_grantable":"1"},
       "needs_input":false,"has_options":false,"options_ts":null,"options":[],
       "prompt_preview":null,"model":null,"served_model":"claude-sonnet-4.5",
       "trust":false,"trust_reads":true,"trust_scope":null,"last_message":"x"},
      {"key":"chat-2","pending_approval_info":null,"has_options":true,
       "options":["Yes","No"],"options_ts":"2026-11-16T18:20:01.250Z","model":"claude-opus-4.1"}
    ]}"#;

    #[test]
    fn slots_frame_with_menu_fields_and_nulls_decodes() {
        let CrewEvent::Slots(slots) = CrewEvent::parse_text(SLOTS_FRAME).unwrap() else {
            panic!("slots");
        };
        let info = slots[0].pending_approval_info.as_ref().unwrap();
        assert_eq!((info.origin.as_str(), info.request_id.as_str(), info.request_mid.as_str()), ("native", "tc-9", "m-41"));
        assert_eq!(slots[0].model, "");
        assert_eq!(slots[0].prompt_preview, "");
        assert_eq!(ApprovalMode::of_slot(&slots[0]), ApprovalMode::Reads);
        assert!(slots[1].pending_approval_info.is_none());
        assert_eq!(slots[1].options, vec!["Yes", "No"]);
        assert_eq!(slots[1].options_ts, "2026-11-16T18:20:01.250Z");
        assert_eq!(ApprovalMode::of_slot(&slots[1]), ApprovalMode::Normal);
    }

    #[test]
    fn followup_and_question_shapes_decode() {
        let ev = CrewEvent::parse_text(
            r#"{"type":"followup_card","data":{"slot":"chat-2","items":[{"title":"Ship","description":"d","prompt":"Ship it"}],"ts":12.5}}"#,
        )
        .unwrap();
        match ev {
            CrewEvent::Followup(card) => {
                assert_eq!(card.slot, "chat-2");
                assert_eq!(card.items[0].prompt, "Ship it");
            }
            other => panic!("{other:?}"),
        }
        let cards: Vec<QuestionCard> = serde_json::from_str(
            r#"[{"ask_id":"a1","slot":"chat-2","ts":1,"questions":[{"question":"Which?","header":"DB","multiSelect":true,"options":[{"label":"pg","description":"x"}]}]},
                {"card_id":"c1","slot":"chat-2","native":true,"ts":2,"questions":[]}]"#,
        )
        .unwrap();
        assert!(cards[0].questions[0].multi_select);
        assert_eq!(cards[0].questions[0].options[0].label, "pg");
        assert!(cards[1].native && cards[1].ask_id.is_empty());
    }

    #[test]
    fn auth_cache_reuses_token_and_remints_once_when_refused() {
        use std::cell::{Cell, RefCell};
        let cache = TokenCache::default();
        let minted = Cell::new(0);
        let mint = || {
            minted.set(minted.get() + 1);
            Ok(format!("t{}", minted.get()))
        };
        // First call mints; later calls reuse the cached token.
        let seen = RefCell::new(Vec::new());
        for _ in 0..2 {
            let v = with_auth_retry(&cache, mint, |t| {
                seen.borrow_mut().push(t.to_string());
                Ok((1, false))
            });
            assert_eq!(v, Ok(1));
        }
        assert_eq!(minted.get(), 1);
        assert_eq!(*seen.borrow(), vec!["t1", "t1"]);

        // Expired: the cached token is refused once -> re-mint and retry.
        seen.borrow_mut().clear();
        let v = with_auth_retry(&cache, mint, |t| {
            seen.borrow_mut().push(t.to_string());
            Ok((t.to_string(), t == "t1"))
        });
        assert_eq!(v, Ok("t2".to_string()));
        assert_eq!(*seen.borrow(), vec!["t1", "t2"]);
        assert_eq!(cache.get().as_deref(), Some("t2"));

        // Refused again after the re-mint: exactly one retry, the second
        // answer is returned, and the bad token is not kept.
        seen.borrow_mut().clear();
        let v = with_auth_retry(&cache, mint, |t| {
            seen.borrow_mut().push(t.to_string());
            Ok((403, true))
        });
        assert_eq!(v, Ok(403));
        assert_eq!(*seen.borrow(), vec!["t2", "t3"]);
        assert_eq!(minted.get(), 3);
        assert_eq!(cache.get(), None);

        // A mint failure surfaces as the error; transport errors do not retry.
        let v: Result<i32, String> = with_auth_retry(&cache, || Err("no secret".into()), |_| Ok((0, false)));
        assert_eq!(v, Err("no secret".to_string()));
        cache.set("t9");
        let calls = Cell::new(0);
        let v: Result<i32, String> = with_auth_retry(&cache, mint, |_| {
            calls.set(calls.get() + 1);
            Err("connection refused".into())
        });
        assert!(v.is_err());
        assert_eq!(calls.get(), 1);
    }

    #[test]
    fn session_cookie_is_picked_from_set_cookie_headers() {
        let headers = [
            "mc_token=; Max-Age=0; Path=/",
            "mc_token_5476=sess.abc; HttpOnly; Path=/; SameSite=Lax",
            "mc_refresh_5476=r; Path=/api/auth",
        ];
        assert_eq!(session_cookie_value(headers.iter().copied(), "mc_token_5476"), Some("sess.abc".into()));
        assert_eq!(session_cookie_value(headers.iter().copied(), "mc_token_1"), None);
        assert_eq!(session_cookie_value(["mc_token_5476=; Max-Age=0"].iter().copied(), "mc_token_5476"), None);
    }

    #[test]
    fn approve_body_is_native_and_mid_bound_and_modes_exclude_yolo() {
        let body = approve_body("tc-9", "m-41", ApprovalAction::RejectedOnce);
        assert_eq!(
            body,
            json!({"origin":"native","action":"rejected_once","request_id":"tc-9","request_mid":"m-41"})
        );
        let wires: Vec<&str> = [ApprovalMode::Normal, ApprovalMode::Reads, ApprovalMode::Trust].iter().map(|m| m.wire()).collect();
        assert_eq!(wires, vec!["normal", "trust_reads", "trust"]);
    }

    #[test]
    fn menu_api_inputs_are_validated_before_network() {
        let client = CrewApiClient::new(CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 1,
            crew_home: PathBuf::from("missing"),
        })
        .unwrap();
        assert!(client.approve("chat/x", "r", "m", ApprovalAction::Approved).unwrap_err().contains("invalid"));
        assert!(client.approve("chat", "r", "", ApprovalAction::Approved).unwrap_err().contains("request_mid"));
        // An empty slot would mean "every session" to Crew: refused here.
        assert!(client.set_mode("", ApprovalMode::Trust).unwrap_err().contains("slot"));
        assert!(client.set_model("chat", " ").unwrap_err().contains("model"));
        assert!(client.answer_ask("", &[]).is_err());
        assert!(client.steer_question("chat", "", "x").is_err());
        let url = client.api_url(&["api", "ask-question", "../x?y", "answer"]).unwrap();
        assert_eq!(url.path(), "/api/ask-question/..%2Fx%3Fy/answer");
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

    #[test]
    fn slot_keys_follow_the_gateway_format() {
        for ok in ["chat-7", "dashboard:chat-1", "member-mate.memory-x_1", "a"] {
            assert_eq!(validate_slot_key(ok), Ok(ok), "{ok}");
        }
        assert_eq!(validate_slot_key("  chat-1 "), Ok("chat-1"));
        let long = "a".repeat(201);
        for bad in ["", "-chat", "chat/../../api", "chat?x=1", "chat 1", "chat#1", "ch\nat", long.as_str()] {
            assert!(validate_slot_key(bad).is_err(), "{bad:?}");
        }
    }

    #[test]
    fn oversized_or_bad_slot_send_is_refused_before_network() {
        let client = CrewApiClient::new(CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 1,
            crew_home: PathBuf::from("missing"),
        })
        .unwrap();
        let big = "x".repeat(MAX_MESSAGE_BYTES + 1);
        assert!(client.send_message("chat-1", &big).unwrap_err().contains("too long"));
        assert!(client.send_message("chat/x", "hi").unwrap_err().contains("invalid"));
        assert!(client.stop_slot("chat/stop").unwrap_err().contains("invalid"));
    }

    #[cfg(unix)]
    #[test]
    fn local_secret_readable_by_others_is_refused() {
        use std::os::unix::fs::PermissionsExt;
        let scratch = std::env::var("KIROCREW_SCRATCH")
            .or_else(|_| std::env::var("TMPDIR"))
            .unwrap_or_else(|_| "/tmp".into());
        let home = PathBuf::from(format!("{scratch}/ak-crew-secret-{}", std::process::id()));
        fs::create_dir_all(&home).unwrap();
        let secret = home.join(".local_secret");
        fs::write(&secret, "s3cr3t\n").unwrap();
        let client = CrewApiClient::new(CrewApiConfig {
            host: "127.0.0.1".into(),
            port: 1,
            crew_home: home.clone(),
        })
        .unwrap();
        fs::set_permissions(&secret, fs::Permissions::from_mode(0o644)).unwrap();
        assert!(client.read_local_secret().unwrap_err().contains("mode 644"));
        fs::set_permissions(&secret, fs::Permissions::from_mode(0o400)).unwrap();
        assert!(client.read_local_secret().unwrap_err().contains("expected 600"));
        fs::set_permissions(&secret, fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(client.read_local_secret().unwrap(), "s3cr3t");
        let _ = fs::remove_dir_all(&home);
    }

    #[cfg(unix)]
    #[test]
    fn secret_file_rule() {
        assert_eq!(secret_file_ok(true, 1000, 0o100600, 1000), Ok(()));
        assert!(secret_file_ok(true, 0, 0o100600, 1000).unwrap_err().contains("owned by uid 0"));
        assert!(secret_file_ok(true, 1000, 0o100640, 1000).is_err());
        assert!(secret_file_ok(true, 1000, 0o100700, 1000).is_err());
        assert!(secret_file_ok(false, 1000, 0o120600, 1000).unwrap_err().contains("regular"));
    }

    struct FakeListeners(Result<Vec<crate::crew_identity::Listener>, String>, &'static str);
    impl crate::crew_identity::ListenerQuery for FakeListeners {
        fn listeners(&self, _port: u16) -> Result<Vec<crate::crew_identity::Listener>, String> {
            self.0.clone()
        }
        fn current_user(&self) -> Result<crate::crew_identity::OwnerId, String> {
            Ok(crate::crew_identity::OwnerId(self.1.into()))
        }
    }

    #[test]
    fn identity_failure_blocks_secret_and_requests() {
        use crate::crew_identity::{Listener, OwnerId, IDENTITY_FAILED};
        let home = PathBuf::from("/home/u/.kiro/crew");
        let mk = |q: FakeListeners| {
            CrewApiClient::new(CrewApiConfig { host: "127.0.0.1".into(), port: 1, crew_home: home.clone() })
                .unwrap()
                .with_identity_query(Arc::new(q))
        };
        let gw = PathBuf::from("/home/u/.kiro/crew-python/bin/python3.12");
        // Wrong owner / wrong executable / query failure: refused before the
        // secret file is even read (it does not exist here) or a socket opens.
        for q in [
            FakeListeners(Ok(vec![Listener { pid: 5, owner: OwnerId("0".into()), exe: gw.clone() }]), "1000"),
            FakeListeners(
                Ok(vec![Listener { pid: 5, owner: OwnerId("1000".into()), exe: "/usr/bin/nc".into() }]),
                "1000",
            ),
            FakeListeners(Err("denied".into()), "1000"),
        ] {
            let c = mk(q);
            assert!(c.bootstrap_token().unwrap_err().starts_with(IDENTITY_FAILED));
            assert!(c.list_slots().unwrap_err().starts_with(IDENTITY_FAILED));
        }
        // Genuine gateway: the check passes and the next step (reading the
        // missing secret) is what fails.
        let ok = mk(FakeListeners(Ok(vec![Listener { pid: 5, owner: OwnerId("1000".into()), exe: gw }]), "1000"));
        assert_eq!(ok.verify_gateway(), Ok(5));
        let err = ok.bootstrap_token().unwrap_err();
        assert!(!err.starts_with(IDENTITY_FAILED) && err.contains("local secret"), "{err}");
    }

    #[test]
    fn install_root_is_the_parent_of_the_crew_home() {
        let cfg = CrewApiConfig { host: "127.0.0.1".into(), port: 1, crew_home: PathBuf::from("/home/u/.kiro/crew") };
        assert_eq!(cfg.install_root(), PathBuf::from("/home/u/.kiro"));
    }
}
