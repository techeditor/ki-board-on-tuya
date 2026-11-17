//! Alibaba Cloud Model Studio (百炼) realtime speech recognition: Fun-ASR
//! realtime and Paraformer realtime share one WebSocket protocol.
//!
//! Official docs (checked 2026-11-17):
//! - <https://help.aliyun.com/zh/model-studio/fun-asr-realtime-websocket-api>
//! - <https://help.aliyun.com/zh/model-studio/fun-asr-client-events>
//! - <https://help.aliyun.com/zh/model-studio/fun-asr-server-events>
//! - <https://help.aliyun.com/zh/model-studio/websocket-for-paraformer-real-time-service>
//!
//! - `wss://dashscope.aliyuncs.com/api-ws/v1/inference`, header
//!   `Authorization: Bearer <API key>` (checked during the handshake).
//! - Client sends `run-task` (JSON), waits for `task-started`, streams binary
//!   PCM, then sends `finish-task`. The server sends `result-generated`
//!   (`payload.output.sentence`: `text`, `sentence_id`, `sentence_end`,
//!   `heartbeat`), then `task-finished`; or `task-failed` with
//!   `error_code` / `error_message`.
//! - Hotwords: Fun-ASR takes domain words as `input.context` (user,
//!   `input_text`); Paraformer only takes a pre-created `vocabulary_id`, which
//!   this client does not create, so Paraformer runs without hotwords.

use std::collections::BTreeMap;

use serde_json::{json, Value};
use tungstenite::handshake::client::Request;
use tungstenite::Message;

use super::ws::{request_with_headers, WireEvent, WireProtocol, WsEngine};
use super::{random_id, AsrError, AsrErrorKind, EngineKind, StartOptions, SAMPLE_RATE};

pub const URL: &str = "wss://dashscope.aliyuncs.com/api-ws/v1/inference";
/// 100 ms of 16 kHz s16le mono, as in the official sample.
const CHUNK_BYTES: usize = 3200;
/// Fun-ASR keeps at most 400 characters of context per turn.
const CONTEXT_MAX_CHARS: usize = 400;

#[derive(Clone)]
pub struct Config {
    pub url: String,
    pub key: String,
    pub model: String,
}

pub fn engine(config: Config) -> Box<dyn super::AsrEngine> {
    Box::new(WsEngine::new(EngineKind::Aliyun, Aliyun::new(config)))
}

struct Aliyun {
    config: Config,
    task_id: String,
    started: bool,
    /// sentence_id -> text; finished and in-progress sentences.
    sentences: BTreeMap<i64, String>,
}

impl Aliyun {
    fn new(config: Config) -> Self {
        Self { config, task_id: random_id(), started: false, sentences: BTreeMap::new() }
    }

    fn text(&self) -> String {
        self.sentences.values().map(String::as_str).collect::<Vec<_>>().join("")
    }
}

/// The `run-task` message.
pub fn run_task(task_id: &str, model: &str, opts: &StartOptions) -> Value {
    let mut input = json!({});
    if model.starts_with("fun-asr") && !opts.hotwords.is_empty() {
        let mut words = opts.hotwords.join("，");
        if words.chars().count() > CONTEXT_MAX_CHARS {
            words = words.chars().take(CONTEXT_MAX_CHARS).collect();
        }
        input = json!({ "context": [ { "role": "user", "content": [ { "type": "input_text", "text": words } ] } ] });
    }
    json!({
        "header": { "action": "run-task", "task_id": task_id, "streaming": "duplex" },
        "payload": {
            "task_group": "audio",
            "task": "asr",
            "function": "recognition",
            "model": model,
            "parameters": { "format": "pcm", "sample_rate": SAMPLE_RATE },
            "input": input,
        }
    })
}

pub fn finish_task(task_id: &str) -> Value {
    json!({
        "header": { "action": "finish-task", "task_id": task_id, "streaming": "duplex" },
        "payload": { "input": {} }
    })
}

fn failed(header: &Value) -> AsrError {
    let code = header.get("error_code").and_then(Value::as_str).unwrap_or("");
    let message = header.get("error_message").and_then(Value::as_str).unwrap_or("");
    let lower = format!("{code} {message}").to_lowercase();
    let kind = if lower.contains("apikey") || lower.contains("api key") || lower.contains("unauthorized") || lower.contains("access denied") {
        AsrErrorKind::Auth
    } else {
        AsrErrorKind::Server
    };
    AsrError::new(kind, format!("Model Studio task-failed {code}: {}", message.chars().take(300).collect::<String>()))
}

impl WireProtocol for Aliyun {
    fn request(&self) -> Result<Request, AsrError> {
        if self.config.key.is_empty() {
            return Err(AsrError::new(AsrErrorKind::NoKey, "no Model Studio key"));
        }
        request_with_headers(&self.config.url, &[("Authorization", format!("Bearer {}", self.config.key))])
    }

    fn open(&mut self, opts: &StartOptions) -> Vec<Message> {
        vec![Message::Text(run_task(&self.task_id, &self.config.model, opts).to_string().into())]
    }

    fn can_stream(&self) -> bool {
        self.started
    }

    fn chunk_bytes(&self) -> usize {
        CHUNK_BYTES
    }

    fn audio(&mut self, pcm: Vec<u8>) -> Vec<Message> {
        vec![Message::Binary(pcm.into())]
    }

    fn finish(&mut self, rest: Vec<u8>) -> Vec<Message> {
        let mut out = Vec::new();
        if !rest.is_empty() {
            out.push(Message::Binary(rest.into()));
        }
        out.push(Message::Text(finish_task(&self.task_id).to_string().into()));
        out
    }

    fn handle(&mut self, msg: Message) -> Result<Option<WireEvent>, AsrError> {
        let Message::Text(text) = msg else { return Ok(None) };
        let v: Value = serde_json::from_str(text.as_str())
            .map_err(|e| AsrError::new(AsrErrorKind::Protocol, format!("json: {e}")))?;
        let header = &v["header"];
        match header.get("event").and_then(Value::as_str).unwrap_or("") {
            "task-started" => {
                self.started = true;
                Ok(Some(WireEvent::Started))
            }
            "result-generated" => {
                let s = &v["payload"]["output"]["sentence"];
                if s.get("heartbeat").and_then(Value::as_bool) == Some(true) {
                    return Ok(None);
                }
                let id = s.get("sentence_id").and_then(Value::as_i64).unwrap_or(0);
                let t = s.get("text").and_then(Value::as_str).unwrap_or("");
                self.sentences.insert(id, t.to_string());
                Ok(Some(WireEvent::Partial(self.text())))
            }
            "task-finished" => Ok(Some(WireEvent::Final(self.text()))),
            "task-failed" => Err(failed(header)),
            _ => Ok(None),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::ws::mock;
    use super::super::AsrEvent;
    use super::*;
    use std::sync::mpsc;

    fn event(name: &str, payload: Value) -> Message {
        Message::Text(json!({"header": {"task_id": "t", "event": name, "attributes": {}}, "payload": payload}).to_string().into())
    }

    fn sentence(id: i64, text: &str, end: bool) -> Message {
        event("result-generated", json!({"output": {"sentence": {"sentence_id": id, "text": text, "sentence_end": end, "heartbeat": false}}}))
    }

    #[test]
    fn run_task_matches_the_documented_shape() {
        let v = run_task("abc", "fun-asr-realtime", &StartOptions::default());
        assert_eq!(v["header"], json!({"action": "run-task", "task_id": "abc", "streaming": "duplex"}));
        assert_eq!(v["payload"]["parameters"], json!({"format": "pcm", "sample_rate": 16000}));
        assert_eq!(v["payload"]["input"], json!({}));
        assert_eq!(finish_task("abc")["header"]["action"], "finish-task");
    }

    #[test]
    fn hotwords_are_context_for_fun_asr_only() {
        let opts = StartOptions { hotwords: vec!["Kiro".into(), "涂鸦".into()] };
        let v = run_task("t", "fun-asr-realtime", &opts);
        assert_eq!(v["payload"]["input"]["context"][0]["content"][0], json!({"type": "input_text", "text": "Kiro，涂鸦"}));
        assert_eq!(run_task("t", "paraformer-realtime-v2", &opts)["payload"]["input"], json!({}));
    }

    #[test]
    fn sentences_accumulate_and_heartbeats_are_skipped() {
        let mut p = Aliyun::new(Config { url: URL.into(), key: "k".into(), model: "fun-asr-realtime".into() });
        assert!(!p.can_stream());
        assert_eq!(p.handle(event("task-started", json!({}))).unwrap(), Some(WireEvent::Started));
        assert!(p.can_stream());
        assert_eq!(p.handle(sentence(1, "好，", false)).unwrap(), Some(WireEvent::Partial("好，".into())));
        assert_eq!(p.handle(sentence(1, "好，我知道了。", true)).unwrap(), Some(WireEvent::Partial("好，我知道了。".into())));
        assert_eq!(p.handle(sentence(2, "明天", false)).unwrap(), Some(WireEvent::Partial("好，我知道了。明天".into())));
        let hb = event("result-generated", json!({"output": {"sentence": {"sentence_id": 0, "text": "", "heartbeat": true}}}));
        assert_eq!(p.handle(hb).unwrap(), None);
        assert_eq!(p.handle(event("task-finished", json!({}))).unwrap(), Some(WireEvent::Final("好，我知道了。明天".into())));
    }

    #[test]
    fn task_failed_maps_auth_and_server_errors() {
        let mut p = Aliyun::new(Config { url: URL.into(), key: "k".into(), model: "m".into() });
        let fail = |code: &str, msg: &str| {
            Message::Text(json!({"header": {"event": "task-failed", "error_code": code, "error_message": msg}, "payload": {}}).to_string().into())
        };
        assert_eq!(p.handle(fail("InvalidApiKey", "Invalid API-key provided.")).unwrap_err().kind, AsrErrorKind::Auth);
        assert_eq!(p.handle(fail("CLIENT_ERROR", "request timeout after 23 seconds.")).unwrap_err().kind, AsrErrorKind::Server);
    }

    #[test]
    fn streams_to_a_mock_server_after_task_started() {
        let (url, server) = mock::serve(|msg| match msg {
            Message::Text(t) => {
                let v: Value = serde_json::from_str(t.as_str()).unwrap();
                match v["header"]["action"].as_str() {
                    Some("run-task") => Some(vec![event("task-started", json!({}))]),
                    Some("finish-task") => Some(vec![sentence(1, "打开设置。", true), event("task-finished", json!({}))]),
                    _ => Some(vec![]),
                }
            }
            Message::Binary(_) => Some(vec![sentence(1, "打开", false)]),
            _ => Some(vec![]),
        });
        let mut eng = engine(Config { url, key: "sk-test".into(), model: "fun-asr-realtime".into() });
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        // Audio pushed before task-started is held, then sent in 100 ms chunks.
        eng.push_audio(&vec![0i16; 4000]).unwrap();
        eng.finish().unwrap();
        let events = mock::collect(&rx);
        assert_eq!(events.last(), Some(&AsrEvent::Final("打开设置。".into())));
        let seen = server.join().unwrap();
        let auth = seen.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case("authorization")).map(|(_, v)| v.as_str());
        assert_eq!(auth, Some("Bearer sk-test"));
        let kinds: Vec<String> = seen
            .messages
            .iter()
            .map(|m| match m {
                Message::Text(t) => serde_json::from_str::<Value>(t.as_str()).unwrap()["header"]["action"].as_str().unwrap().to_string(),
                Message::Binary(b) => format!("bin{}", b.len()),
                _ => "other".into(),
            })
            .collect();
        assert_eq!(kinds, vec!["run-task", "bin3200", "bin3200", "bin1600", "finish-task"]);
    }

    #[test]
    fn refused_handshake_is_an_auth_error() {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let url = format!("ws://{}", listener.local_addr().unwrap());
        let server = std::thread::spawn(move || {
            let (mut s, _) = listener.accept().unwrap();
            let mut buf = [0u8; 2048];
            let _ = s.read(&mut buf);
            let _ = s.write_all(b"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
        });
        let mut eng = engine(Config { url, key: "bad".into(), model: "fun-asr-realtime".into() });
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        let events = mock::collect(&rx);
        assert!(matches!(events.last(), Some(AsrEvent::Error(e)) if e.kind == AsrErrorKind::Auth), "{events:?}");
        assert!(!format!("{events:?}").contains("bad\""), "the key must not appear in errors");
        server.join().unwrap();
    }
}
