//! FP-4 kirocrew-control — drive the local Kiro Crew gateway (route A).
//!
//! The companion turns a board voice/command event into a real Kiro Crew agent
//! turn by calling the gateway's **session-control** HTTP API on the loopback
//! interface (`http://127.0.0.1:5476`). Per design D2, the four control ops map
//! onto four endpoints:
//!
//! | op        | endpoint                          | body                        |
//! |-----------|-----------------------------------|-----------------------------|
//! | bind      | `POST /api/session-control/create`| `{title, agent}` -> session |
//! | send/exec | `POST /api/session-control/send`  | `{target, message}`         |
//! | read      | `POST /api/session-control/read`  | `{target}`                  |
//! | switch    | `POST /api/session-control/stop`  | `{target}`                  |
//!
//! Auth (design D1/E4): strict-internal routes require the header
//! `X-Internal-Secret` whose value is the gateway's `.local_secret`, read **at
//! runtime** (never baked into the binary) from `$KIROCREW_HOME/.local_secret`
//! else `~/.kiro/crew/.local_secret`. A CSRF `Origin: http://127.0.0.1:5476`
//! header is also sent so the strict-internal Origin check passes.
//!
//! This module is **platform-neutral**: it has no Tauri / GTK / Win32 deps, so
//! `cargo test` / `clippy` build it on the Linux CI host. The Tauri command
//! wrappers that call it live in the `#[cfg(windows)]` app module in `lib.rs`.
//!
//! The path-resolution and request-shaping helpers are pure (no network, no
//! process env writes beyond reads) and unit-tested below.

use std::path::PathBuf;
use std::time::Duration;

use serde_json::{json, Value};

/// Gateway base URL (loopback only — the gateway is never exposed externally).
pub const GATEWAY_BASE: &str = "http://127.0.0.1:5476";

/// Header name carrying the gateway internal secret on strict-internal routes.
pub const INTERNAL_SECRET_HEADER: &str = "X-Internal-Secret";

/// Request timeout for a single session-control call.
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

// ───────────────────────── pure helpers (unit-tested) ─────────────────────────

/// Resolve the on-disk path of the gateway's `.local_secret`, honouring the
/// same precedence the gateway itself uses:
///
/// 1. `$KIROCREW_HOME/.local_secret` when `KIROCREW_HOME` is set & non-empty;
/// 2. otherwise `<home>/.kiro/crew/.local_secret`.
///
/// `kirocrew_home` / `home_dir` are passed in (rather than read from the
/// process env) so the resolution is a pure function and fully unit-testable.
/// The runtime wrapper [`secret_path`] supplies the real env values.
pub fn resolve_secret_path(kirocrew_home: Option<&str>, home_dir: Option<&str>) -> Option<PathBuf> {
    if let Some(kc_home) = kirocrew_home {
        let kc_home = kc_home.trim();
        if !kc_home.is_empty() {
            return Some(PathBuf::from(kc_home).join(".local_secret"));
        }
    }
    let home = home_dir?;
    let home = home.trim();
    if home.is_empty() {
        return None;
    }
    Some(
        PathBuf::from(home)
            .join(".kiro")
            .join("crew")
            .join(".local_secret"),
    )
}

/// Runtime `.local_secret` path from the current process environment.
pub fn secret_path() -> Option<PathBuf> {
    let kc_home = std::env::var("KIROCREW_HOME").ok();
    let home = std::env::var("HOME")
        .ok()
        .or_else(|| std::env::var("USERPROFILE").ok());
    resolve_secret_path(kc_home.as_deref(), home.as_deref())
}

/// A session-control op and the request it maps to. Keeps URL + body derivation
/// in one pure place so both the client and the unit tests build identical
/// requests.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Op {
    /// bind: create/bind a session for `agent` with `title`.
    Create,
    /// send + execute: run `message` in `target` as its next turn.
    Send,
    /// read: fetch the target's latest reply.
    Read,
    /// switch: stop/close `target`.
    Stop,
}

impl Op {
    /// Path segment under `/api/session-control/`.
    pub fn path_suffix(self) -> &'static str {
        match self {
            Op::Create => "create",
            Op::Send => "send",
            Op::Read => "read",
            Op::Stop => "stop",
        }
    }

    /// Full request URL for this op against `base` (no trailing slash on base).
    pub fn url(self, base: &str) -> String {
        format!(
            "{}/api/session-control/{}",
            base.trim_end_matches('/'),
            self.path_suffix()
        )
    }
}

/// Build the JSON body for `create` (bind): `{title, agent}`.
pub fn create_body(title: &str, agent: &str) -> Value {
    json!({ "title": title, "agent": agent })
}

/// Build the JSON body for `send` (input + execute): `{target, message}`.
pub fn send_body(target: &str, message: &str) -> Value {
    json!({ "target": target, "message": message })
}

/// Build the JSON body for `read`: `{target}`.
pub fn read_body(target: &str) -> Value {
    json!({ "target": target })
}

/// Build the JSON body for `stop` (switch): `{target}`.
pub fn stop_body(target: &str) -> Value {
    json!({ "target": target })
}

/// Extract a created session id from a `create` response body, tolerating the
/// common key spellings the gateway may use.
pub fn parse_created_id(body: &Value) -> Option<String> {
    for key in [
        "id",
        "session_id",
        "sessionId",
        "sessionKey",
        "session_key",
        "target",
    ] {
        if let Some(s) = body.get(key).and_then(Value::as_str) {
            if !s.is_empty() {
                return Some(s.to_string());
            }
        }
    }
    None
}

/// Extract the latest reply text from a `read` response body, tolerating the
/// common shapes: a plain string field, or `{message|text|reply|content}`.
pub fn parse_reply(body: &Value) -> String {
    for key in ["reply", "message", "text", "content", "latest"] {
        if let Some(s) = body.get(key).and_then(Value::as_str) {
            return s.to_string();
        }
    }
    // Fall back to the whole body so the caller still sees *something* useful
    // (e.g. a transcript array) rather than an empty string.
    body.to_string()
}

// ───────────────────────── network client ─────────────────────────

/// Thin blocking client over the gateway session-control API.
///
/// The `.local_secret` value is held only in memory (never persisted), matching
/// the R6.3 requirement. Construct with [`KiroCrewClient::from_env`] to read the
/// secret from disk at runtime, or [`KiroCrewClient::new`] to inject it (tests /
/// alternate secret sources).
pub struct KiroCrewClient {
    base: String,
    secret: String,
    http: reqwest::blocking::Client,
}

impl KiroCrewClient {
    /// Build a client with an explicit base URL and secret (no disk access).
    pub fn new(base: impl Into<String>, secret: impl Into<String>) -> Result<Self, String> {
        let http = reqwest::blocking::Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()
            .map_err(|e| format!("failed to build HTTP client: {e}"))?;
        Ok(Self {
            base: base.into(),
            secret: secret.into(),
            http,
        })
    }

    /// Build a client for the local gateway, reading `.local_secret` at runtime.
    pub fn from_env() -> Result<Self, String> {
        let path = secret_path().ok_or_else(|| {
            "cannot resolve .local_secret path (set KIROCREW_HOME or HOME)".to_string()
        })?;
        let secret = std::fs::read_to_string(&path)
            .map_err(|e| format!("cannot read {}: {e}", path.display()))?
            .trim()
            .to_string();
        if secret.is_empty() {
            return Err(format!("{} is empty", path.display()));
        }
        Self::new(GATEWAY_BASE, secret)
    }

    /// Issue one session-control POST and return the parsed JSON body, mapping
    /// transport and HTTP-status errors to a human-readable `String` (design
    /// D2 error semantics: 400/403/404/409/429 → reason).
    fn post(&self, op: Op, body: &Value) -> Result<Value, String> {
        let url = op.url(&self.base);
        let resp = self
            .http
            .post(&url)
            .header(INTERNAL_SECRET_HEADER, &self.secret)
            .header(reqwest::header::ORIGIN, GATEWAY_BASE)
            .json(body)
            .send()
            .map_err(|e| format!("{} request failed: {e}", op.path_suffix()))?;

        let status = resp.status();
        let text = resp.text().unwrap_or_default();
        if !status.is_success() {
            return Err(format!(
                "{} returned HTTP {}: {}",
                op.path_suffix(),
                status.as_u16(),
                text.trim()
            ));
        }
        if text.trim().is_empty() {
            return Ok(Value::Null);
        }
        serde_json::from_str(&text)
            .map_err(|e| format!("{} response was not JSON: {e}", op.path_suffix()))
    }

    /// bind (op ①): create/bind a session for `agent`; return its id/handle.
    pub fn create_session(&self, title: &str, agent: &str) -> Result<String, String> {
        let body = self.post(Op::Create, &create_body(title, agent))?;
        parse_created_id(&body).ok_or_else(|| format!("create returned no session id: {body}"))
    }

    /// input + send/execute (ops ②③): run `message` in `target` as its next turn.
    pub fn send(&self, target: &str, message: &str) -> Result<(), String> {
        self.post(Op::Send, &send_body(target, message))?;
        Ok(())
    }

    /// read: fetch the target's latest reply (for LCD feedback / acks).
    pub fn read(&self, target: &str) -> Result<String, String> {
        let body = self.post(Op::Read, &read_body(target))?;
        Ok(parse_reply(&body))
    }

    /// switch (op ④): stop/close `target`.
    pub fn stop(&self, target: &str) -> Result<(), String> {
        self.post(Op::Stop, &stop_body(target))?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── secret-path resolution ─────────────────────────────────

    #[test]
    fn secret_path_prefers_kirocrew_home() {
        let p =
            resolve_secret_path(Some("/data/kiro/.kiro/crew"), Some("/home/alice")).expect("path");
        assert_eq!(p, PathBuf::from("/data/kiro/.kiro/crew/.local_secret"));
    }

    #[test]
    fn secret_path_falls_back_to_home_kiro_crew() {
        let p = resolve_secret_path(None, Some("/home/alice")).expect("path");
        assert_eq!(p, PathBuf::from("/home/alice/.kiro/crew/.local_secret"));
    }

    #[test]
    fn secret_path_ignores_blank_kirocrew_home() {
        // A blank/whitespace KIROCREW_HOME must not win over HOME.
        let p = resolve_secret_path(Some("   "), Some("/home/bob")).expect("path");
        assert_eq!(p, PathBuf::from("/home/bob/.kiro/crew/.local_secret"));
    }

    #[test]
    fn secret_path_trims_kirocrew_home() {
        let p = resolve_secret_path(Some(" /opt/kc \n"), None).expect("path");
        assert_eq!(p, PathBuf::from("/opt/kc/.local_secret"));
    }

    #[test]
    fn secret_path_none_when_no_source() {
        assert!(resolve_secret_path(None, None).is_none());
        assert!(resolve_secret_path(Some(""), Some("")).is_none());
    }

    // ── URL construction for all four ops ──────────────────────

    #[test]
    fn op_urls_are_correct() {
        let base = "http://127.0.0.1:5476";
        assert_eq!(
            Op::Create.url(base),
            "http://127.0.0.1:5476/api/session-control/create"
        );
        assert_eq!(
            Op::Send.url(base),
            "http://127.0.0.1:5476/api/session-control/send"
        );
        assert_eq!(
            Op::Read.url(base),
            "http://127.0.0.1:5476/api/session-control/read"
        );
        assert_eq!(
            Op::Stop.url(base),
            "http://127.0.0.1:5476/api/session-control/stop"
        );
    }

    #[test]
    fn op_url_trims_trailing_slash_on_base() {
        assert_eq!(
            Op::Send.url("http://127.0.0.1:5476/"),
            "http://127.0.0.1:5476/api/session-control/send"
        );
    }

    // ── request-body construction for all four ops ─────────────

    #[test]
    fn create_body_has_title_and_agent() {
        let b = create_body("Board control", "kirocrew");
        assert_eq!(b["title"], "Board control");
        assert_eq!(b["agent"], "kirocrew");
        // Exactly the two expected keys.
        assert_eq!(b.as_object().unwrap().len(), 2);
    }

    #[test]
    fn send_body_has_target_and_message() {
        let b = send_body("sess-123", "hello agent");
        assert_eq!(b["target"], "sess-123");
        assert_eq!(b["message"], "hello agent");
        assert_eq!(b.as_object().unwrap().len(), 2);
    }

    #[test]
    fn read_and_stop_bodies_have_only_target() {
        let r = read_body("sess-9");
        assert_eq!(r["target"], "sess-9");
        assert_eq!(r.as_object().unwrap().len(), 1);

        let s = stop_body("sess-9");
        assert_eq!(s["target"], "sess-9");
        assert_eq!(s.as_object().unwrap().len(), 1);
    }

    // ── response parsing tolerance ─────────────────────────────

    #[test]
    fn parse_created_id_accepts_common_key_spellings() {
        assert_eq!(parse_created_id(&json!({"id": "a1"})), Some("a1".into()));
        assert_eq!(
            parse_created_id(&json!({"session_id": "b2"})),
            Some("b2".into())
        );
        assert_eq!(
            parse_created_id(&json!({"sessionKey": "c3"})),
            Some("c3".into())
        );
        assert_eq!(parse_created_id(&json!({"unrelated": 1})), None);
        assert_eq!(parse_created_id(&json!({"id": ""})), None);
    }

    #[test]
    fn parse_reply_reads_common_shapes() {
        assert_eq!(parse_reply(&json!({"reply": "hi"})), "hi");
        assert_eq!(parse_reply(&json!({"message": "yo"})), "yo");
        assert_eq!(parse_reply(&json!({"text": "sup"})), "sup");
        // Unknown shape falls back to the serialized body (non-empty).
        let fallback = parse_reply(&json!({"transcript": [1, 2, 3]}));
        assert!(fallback.contains("transcript"));
    }

    #[test]
    fn client_new_builds_without_network() {
        // Constructing the client must not touch the network or disk.
        let c = KiroCrewClient::new("http://127.0.0.1:5476", "dummy-secret");
        assert!(c.is_ok());
    }
}
