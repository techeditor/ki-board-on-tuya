//! Volcengine Doubao streaming ASR (大模型流式语音识别), bidirectional
//! streaming, optimised endpoint.
//!
//! Protocol (official doc, checked 2026-11-17):
//! <https://www.volcengine.com/docs/6561/1354869>
//!
//! - `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`
//! - Auth in handshake headers: new console `X-Api-Key`; old console
//!   `X-Api-App-Key` + `X-Api-Access-Key`. Plus `X-Api-Resource-Id` and a
//!   random `X-Api-Connect-Id`.
//! - Binary frames: 4-byte header (version 1 / header size 1, message type +
//!   flags, serialization + compression, reserved), optional big-endian
//!   sequence, big-endian payload size, payload. The first frame is a "full
//!   client request" (JSON), then "audio only" frames; the last audio frame
//!   carries a negative sequence. The server answers "full server response"
//!   frames (JSON `result.text`) and the response to the last packet has the
//!   last-packet flag. Error frames carry a code and a message.
//! - Audio: PCM s16le, 16 kHz, mono; 200 ms per packet is recommended.

use std::io::Read;

use serde_json::{json, Value};
use tungstenite::handshake::client::Request;
use tungstenite::Message;

use super::ws::{request_with_headers, WireEvent, WireProtocol, WsEngine};
use super::{random_id, AsrError, AsrErrorKind, EngineKind, StartOptions, SAMPLE_RATE};

pub const URL: &str = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";

const HEADER_V1: u8 = 0x11; // version 1, header size 1 x 4 bytes
const FULL_CLIENT_REQUEST: u8 = 0b0001;
const AUDIO_ONLY: u8 = 0b0010;
const FULL_SERVER_RESPONSE: u8 = 0b1001;
const SERVER_ERROR: u8 = 0b1111;
const FLAG_POS_SEQ: u8 = 0b0001;
const FLAG_LAST: u8 = 0b0010;
const FLAG_NEG_SEQ: u8 = 0b0011;
const SER_NONE: u8 = 0;
const SER_JSON: u8 = 1;
const COMP_NONE: u8 = 0;
const COMP_GZIP: u8 = 1;
/// 200 ms of 16 kHz s16le mono.
const CHUNK_BYTES: usize = 6400;

/// Credentials and settings for one connection.
#[derive(Clone)]
pub struct Config {
    pub url: String,
    /// New console: the API key. Old console: the access token.
    pub key: String,
    /// Old console only; empty for the new console.
    pub app_id: String,
    pub resource_id: String,
}

pub fn engine(config: Config) -> Box<dyn super::AsrEngine> {
    Box::new(WsEngine::new(EngineKind::Doubao, Doubao { config, seq: 1 }))
}

struct Doubao {
    config: Config,
    seq: i32,
}

/// One client frame (no compression).
pub fn encode(msg_type: u8, flags: u8, serialization: u8, seq: Option<i32>, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(12 + payload.len());
    out.push(HEADER_V1);
    out.push((msg_type << 4) | (flags & 0x0f));
    out.push((serialization << 4) | COMP_NONE);
    out.push(0);
    if let Some(seq) = seq {
        out.extend_from_slice(&seq.to_be_bytes());
    }
    out.extend_from_slice(&(payload.len() as u32).to_be_bytes());
    out.extend_from_slice(payload);
    out
}

/// A server frame.
#[derive(Debug, PartialEq)]
pub enum ServerFrame {
    Response { last: bool, body: Value },
    Error { code: u32, message: String },
}

fn be_u32(b: &[u8], at: usize) -> Result<u32, AsrError> {
    b.get(at..at + 4)
        .map(|s| u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
        .ok_or_else(|| AsrError::new(AsrErrorKind::Protocol, "short frame"))
}

pub fn decode(frame: &[u8]) -> Result<ServerFrame, AsrError> {
    let bad = |m: &str| AsrError::new(AsrErrorKind::Protocol, m.to_string());
    if frame.len() < 4 {
        return Err(bad("short frame"));
    }
    let header_len = (frame[0] & 0x0f) as usize * 4;
    let msg_type = frame[1] >> 4;
    let flags = frame[1] & 0x0f;
    let compression = frame[2] & 0x0f;
    let mut at = header_len;
    match msg_type {
        FULL_SERVER_RESPONSE => {
            if flags & FLAG_POS_SEQ != 0 {
                at += 4;
            }
            let size = be_u32(frame, at)? as usize;
            at += 4;
            let raw = frame.get(at..at + size).ok_or_else(|| bad("payload size past end"))?;
            let body = if compression == COMP_GZIP {
                let mut s = Vec::new();
                flate2::read::GzDecoder::new(raw).read_to_end(&mut s).map_err(|e| bad(&format!("gunzip: {e}")))?;
                s
            } else {
                raw.to_vec()
            };
            let body = if body.is_empty() { Value::Null } else { serde_json::from_slice(&body).map_err(|e| bad(&format!("json: {e}")))? };
            Ok(ServerFrame::Response { last: flags & FLAG_LAST != 0, body })
        }
        SERVER_ERROR => {
            let code = be_u32(frame, at)?;
            let size = be_u32(frame, at + 4)? as usize;
            let raw = frame.get(at + 8..at + 8 + size).unwrap_or(&[]);
            let raw = if compression == COMP_GZIP {
                let mut s = Vec::new();
                let _ = flate2::read::GzDecoder::new(raw).read_to_end(&mut s);
                s
            } else {
                raw.to_vec()
            };
            Ok(ServerFrame::Error { code, message: String::from_utf8_lossy(&raw).chars().take(300).collect() })
        }
        _ => Err(bad("unexpected message type")),
    }
}

/// `result.text` (the result may be an object or a one-element list).
fn result_text(body: &Value) -> Option<String> {
    let result = body.get("result")?;
    let result = result.as_array().and_then(|a| a.first()).unwrap_or(result);
    result.get("text").and_then(Value::as_str).map(str::to_string)
}

/// The full client request JSON.
pub fn request_json(opts: &StartOptions) -> Value {
    let mut request = json!({
        "model_name": "bigmodel",
        "enable_itn": true,
        "enable_punc": true,
        "result_type": "full",
    });
    if !opts.hotwords.is_empty() {
        let words: Vec<Value> = opts.hotwords.iter().map(|w| json!({ "word": w })).collect();
        // `context` is a JSON string, per the doc.
        request["corpus"] = json!({ "context": json!({ "hotwords": words }).to_string() });
    }
    json!({
        "user": { "uid": "ki-board-companion" },
        "audio": { "format": "pcm", "codec": "raw", "rate": SAMPLE_RATE, "bits": 16, "channel": 1 },
        "request": request,
    })
}

fn server_error(code: u32, message: String) -> AsrError {
    // 45xxxxxx = request problems; the doc lists auth failures at the handshake.
    let kind = if message.to_lowercase().contains("auth") || message.contains("401") || message.contains("403") {
        AsrErrorKind::Auth
    } else {
        AsrErrorKind::Server
    };
    AsrError::new(kind, format!("Doubao error {code}: {message}"))
}

impl WireProtocol for Doubao {
    fn request(&self) -> Result<Request, AsrError> {
        if self.config.key.is_empty() {
            return Err(AsrError::new(AsrErrorKind::NoKey, "no Doubao key"));
        }
        let mut headers = vec![
            ("X-Api-Resource-Id", self.config.resource_id.clone()),
            ("X-Api-Connect-Id", random_id()),
        ];
        if self.config.app_id.is_empty() {
            headers.push(("X-Api-Key", self.config.key.clone()));
        } else {
            headers.push(("X-Api-App-Key", self.config.app_id.clone()));
            headers.push(("X-Api-Access-Key", self.config.key.clone()));
        }
        request_with_headers(&self.config.url, &headers)
    }

    fn open(&mut self, opts: &StartOptions) -> Vec<Message> {
        let body = request_json(opts).to_string();
        let frame = encode(FULL_CLIENT_REQUEST, FLAG_POS_SEQ, SER_JSON, Some(self.seq), body.as_bytes());
        vec![Message::Binary(frame.into())]
    }

    fn can_stream(&self) -> bool {
        true
    }

    fn chunk_bytes(&self) -> usize {
        CHUNK_BYTES
    }

    fn audio(&mut self, pcm: Vec<u8>) -> Vec<Message> {
        self.seq += 1;
        vec![Message::Binary(encode(AUDIO_ONLY, FLAG_POS_SEQ, SER_NONE, Some(self.seq), &pcm).into())]
    }

    fn finish(&mut self, rest: Vec<u8>) -> Vec<Message> {
        self.seq += 1;
        vec![Message::Binary(encode(AUDIO_ONLY, FLAG_NEG_SEQ, SER_NONE, Some(-self.seq), &rest).into())]
    }

    fn handle(&mut self, msg: Message) -> Result<Option<WireEvent>, AsrError> {
        let Message::Binary(frame) = msg else { return Ok(None) };
        match decode(&frame)? {
            ServerFrame::Error { code, message } => Err(server_error(code, message)),
            ServerFrame::Response { last, body } => {
                if let Some(code) = body.get("code").and_then(Value::as_u64) {
                    if code != 20_000_000 && code != 0 {
                        let m = body.get("message").and_then(Value::as_str).unwrap_or("").to_string();
                        return Err(server_error(code as u32, m));
                    }
                }
                let text = result_text(&body).unwrap_or_default();
                Ok(Some(if last { WireEvent::Final(text) } else { WireEvent::Partial(text) }))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::ws::mock;
    use super::super::AsrEvent;
    use super::*;
    use std::sync::mpsc;

    fn response(seq: i32, last: bool, body: &Value, gzip: bool) -> Vec<u8> {
        let mut payload = body.to_string().into_bytes();
        if gzip {
            use std::io::Write;
            let mut enc = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
            enc.write_all(&payload).unwrap();
            payload = enc.finish().unwrap();
        }
        let flags = if last { FLAG_NEG_SEQ } else { FLAG_POS_SEQ };
        let mut f = vec![HEADER_V1, (FULL_SERVER_RESPONSE << 4) | flags, (SER_JSON << 4) | gzip as u8, 0];
        f.extend_from_slice(&seq.to_be_bytes());
        f.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        f.extend_from_slice(&payload);
        f
    }

    #[test]
    fn client_frames_follow_the_documented_layout() {
        let f = encode(FULL_CLIENT_REQUEST, FLAG_POS_SEQ, SER_JSON, Some(1), b"{}");
        assert_eq!(f, vec![0x11, 0x11, 0x10, 0x00, 0, 0, 0, 1, 0, 0, 0, 2, b'{', b'}']);
        let last = encode(AUDIO_ONLY, FLAG_NEG_SEQ, SER_NONE, Some(-3), &[9, 9]);
        assert_eq!(&last[..4], &[0x11, 0x23, 0x00, 0x00]);
        assert_eq!(&last[4..8], &(-3i32).to_be_bytes());
        assert_eq!(&last[8..12], &[0, 0, 0, 2]);
    }

    #[test]
    fn server_frames_decode_plain_gzip_and_errors() {
        let body = json!({"result": {"text": "你好"}});
        assert_eq!(decode(&response(2, false, &body, false)).unwrap(), ServerFrame::Response { last: false, body: body.clone() });
        assert_eq!(decode(&response(-5, true, &body, true)).unwrap(), ServerFrame::Response { last: true, body });
        let mut err = vec![0x11, 0xF0, 0x10, 0x00];
        err.extend_from_slice(&45_000_001u32.to_be_bytes());
        err.extend_from_slice(&5u32.to_be_bytes());
        err.extend_from_slice(b"oops!");
        assert_eq!(decode(&err).unwrap(), ServerFrame::Error { code: 45_000_001, message: "oops!".into() });
        assert!(decode(&[0x11, 0x91]).is_err());
        assert!(decode(&[0x11, 0x91, 0x10, 0, 0, 0, 0, 1, 0, 0, 0, 99]).is_err());
    }

    #[test]
    fn hotwords_go_into_corpus_context_as_a_json_string() {
        let opts = StartOptions { hotwords: vec!["Kiro".into(), "涂鸦".into()] };
        let j = request_json(&opts);
        let ctx: Value = serde_json::from_str(j["request"]["corpus"]["context"].as_str().unwrap()).unwrap();
        assert_eq!(ctx, json!({"hotwords": [{"word": "Kiro"}, {"word": "涂鸦"}]}));
        assert_eq!(j["audio"]["rate"], 16000);
        assert!(request_json(&StartOptions::default())["request"].get("corpus").is_none());
    }

    fn run_against_mock(app_id: &str) -> (Vec<AsrEvent>, mock::Seen) {
        let mut audio_frames = 0;
        let (url, server) = mock::serve(move |msg| {
            let Message::Binary(f) = msg else { return Some(vec![]) };
            let msg_type = f[1] >> 4;
            let flags = f[1] & 0x0f;
            if msg_type == FULL_CLIENT_REQUEST {
                return Some(vec![Message::Binary(response(1, false, &json!({"result": {"text": ""}}), false).into())]);
            }
            audio_frames += 1;
            let last = flags == FLAG_NEG_SEQ;
            let text = if last { "打开设置。" } else { "打开" };
            Some(vec![Message::Binary(response(audio_frames + 1, last, &json!({"result": {"text": text}}), true).into())])
        });
        let mut eng = engine(Config { url, key: "test-key".into(), app_id: app_id.into(), resource_id: "volc.seedasr.sauc.duration".into() });
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        eng.push_audio(&vec![0i16; 8000]).unwrap(); // 0.5 s -> 2 full packets + rest
        eng.finish().unwrap();
        let events = mock::collect(&rx);
        (events, server.join().unwrap())
    }

    #[test]
    fn streams_to_a_mock_server_and_reports_partial_then_final() {
        let (events, seen) = run_against_mock("");
        assert_eq!(events.last(), Some(&AsrEvent::Final("打开设置。".into())));
        assert!(events.contains(&AsrEvent::Partial("打开".into())));
        let header = |n: &str| seen.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(n)).map(|(_, v)| v.clone());
        assert_eq!(header("x-api-key").as_deref(), Some("test-key"));
        assert_eq!(header("x-api-resource-id").as_deref(), Some("volc.seedasr.sauc.duration"));
        assert_eq!(header("x-api-connect-id").map(|v| v.len()), Some(32));
        assert!(header("x-api-app-key").is_none());
        // full request + 2 x 6400 bytes + last packet with the remaining 3200.
        let sizes: Vec<usize> = seen.messages.iter().map(|m| m.len()).collect();
        assert_eq!(sizes.len(), 4, "{sizes:?}");
        assert_eq!(sizes[1], 12 + 6400);
        assert_eq!(sizes[3], 12 + 3200);
        let Message::Binary(last) = &seen.messages[3] else { panic!() };
        assert_eq!(last[1], 0x23);
        assert_eq!(&last[4..8], &(-4i32).to_be_bytes());
    }

    #[test]
    fn old_console_sends_app_id_and_access_token() {
        let (_, seen) = run_against_mock("123456");
        let header = |n: &str| seen.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(n)).map(|(_, v)| v.clone());
        assert_eq!(header("x-api-app-key").as_deref(), Some("123456"));
        assert_eq!(header("x-api-access-key").as_deref(), Some("test-key"));
        assert!(header("x-api-key").is_none());
    }

    #[test]
    fn server_error_frame_becomes_an_error_event() {
        let (url, server) = mock::serve(|_| {
            let mut err = vec![0x11, 0xF0, 0x10, 0x00];
            err.extend_from_slice(&45_000_151u32.to_be_bytes());
            err.extend_from_slice(&12u32.to_be_bytes());
            err.extend_from_slice(b"bad audio fm");
            Some(vec![Message::Binary(err.into())])
        });
        let mut eng = engine(Config { url, key: "k".into(), app_id: String::new(), resource_id: "r".into() });
        let (tx, rx) = mpsc::channel();
        eng.start(&StartOptions::default(), Box::new(move |e| { let _ = tx.send(e); })).unwrap();
        let events = mock::collect(&rx);
        assert!(matches!(events.last(), Some(AsrEvent::Error(e)) if e.kind == AsrErrorKind::Server && e.detail.contains("45000151")));
        eng.cancel();
        drop(eng);
        let _ = server.join();
    }

    #[test]
    fn no_key_fails_before_connecting() {
        let mut eng = engine(Config { url: URL.into(), key: String::new(), app_id: String::new(), resource_id: "r".into() });
        let err = eng.start(&StartOptions::default(), Box::new(|_| {})).unwrap_err();
        assert_eq!(err.kind, AsrErrorKind::NoKey);
    }
}
