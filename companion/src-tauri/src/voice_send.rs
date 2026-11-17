//! Voice input after K6 on the keyboard (input confirm ③).
//!
//! Default: the keyboard keeps the recognised text on its LCD, K4 edits it
//! there, and K6 sends a `confirm` key frame that carries the text; the
//! Companion posts it to the selected chat through the Crew API. Nothing is
//! typed and the Crew window need not be in front. On failure the text goes
//! back to the keyboard (`input_restore`) instead of being typed.
//!
//! Optional (`binds.json` `voice_inject`): the previous behaviour, where the
//! `asr` text and the K4/K6 edits are injected into the Crew composer.
//!
//! Pure: decides what a board frame means here; the app layer does the IO.

use serde_json::{Map, Value};

use crate::kirocrew_control::sanitize_device_text;
use crate::menu_wire::{clip_text, MAX_FRAME_BYTES, NOTICE_MAX};
use crate::protocol::{Inbound, KeyAction, PROTOCOL_VERSION};

/// What the Companion does with a board frame in direct-send mode.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VoiceDirect {
    /// Not a voice-input frame: route it as before.
    NotMine,
    /// A voice-input frame with nothing to do on the PC (the `asr` text, the
    /// K4 / K1 edits): the keyboard handles it; never inject it.
    Consumed,
    /// K6 confirmed: post `text` to `slot`.
    Send { slot: String, text: String },
    /// K6 confirmed but it cannot be sent. `text` (when there is one) goes
    /// back to the keyboard; `reason` is shown there.
    Fail { reason: &'static str, text: Option<String> },
}

/// Classify a board frame for direct-send mode. `active_slot` is the chat
/// K1-K3 selected (the Crew API stable slot key), if any.
pub fn classify(frame: &Inbound, active_slot: Option<&str>) -> VoiceDirect {
    match frame {
        Inbound::Asr { .. } => VoiceDirect::Consumed,
        Inbound::Key { payload, .. } => match payload.bind {
            Some(KeyAction::CancelChar) | Some(KeyAction::CancelAll) => VoiceDirect::Consumed,
            Some(KeyAction::Confirm) => {
                let Some(raw) = payload.text.as_deref() else {
                    // Firmware without the text field: nothing to send.
                    return VoiceDirect::Fail { reason: "Update keyboard firmware", text: None };
                };
                // Same rules as injected text: no control characters, a hard
                // length cap, empty means nothing to send.
                let Some(text) = sanitize_device_text(raw) else {
                    return VoiceDirect::Fail { reason: "Nothing to send", text: None };
                };
                match active_slot.map(str::trim).filter(|s| !s.is_empty()) {
                    Some(slot) => VoiceDirect::Send { slot: slot.to_string(), text },
                    None => VoiceDirect::Fail { reason: "No chat selected (K1-K3)", text: Some(text) },
                }
            }
            _ => VoiceDirect::NotMine,
        },
        _ => VoiceDirect::NotMine,
    }
}

/// Short keyboard text for a failed Crew API send. The full error goes to
/// the Companion log only.
pub fn failure_reason(error: &str) -> &'static str {
    if error.contains("HTTP 401") || error.contains("HTTP 403") || error.contains("token") {
        "Crew refused: sign-in"
    } else if error.contains("local secret") || error.contains("connect") || error.contains("unavailable") {
        "Crew not running"
    } else {
        "Send failed"
    }
}

/// Where an `input_restore` text comes from (`src`): the keyboard shows a
/// failed send in red and a PC ASR result in its normal colour.
pub const SRC_RETRY: &str = "retry";
pub const SRC_PC: &str = "pc";

/// Companion -> keyboard: show `text` in ③ with `msg` on the status bar
/// (a failed send, or a PC ASR result; an empty PC text ends the keyboard's
/// wait in ②). The text is clipped (UTF-8 safe) until the frame fits the
/// keyboard's 511-byte line limit.
pub fn input_restore_payload(text: &str, msg: &str, src: &str) -> Value {
    let msg = clip_text(msg, NOTICE_MAX);
    let mut budget = text.len();
    loop {
        let mut p = Map::new();
        p.insert("text".into(), Value::String(clip_text(text, budget)));
        if !msg.is_empty() {
            p.insert("msg".into(), Value::String(msg.clone()));
        }
        p.insert("src".into(), Value::String(src.to_string()));
        let frame = serde_json::json!({ "v": PROTOCOL_VERSION, "type": "input_restore", "payload": Value::Object(p) });
        let len = serde_json::to_vec(&frame).map(|b| b.len()).unwrap_or(usize::MAX);
        if len <= MAX_FRAME_BYTES || budget <= 3 {
            return frame;
        }
        budget = budget.saturating_sub((len - MAX_FRAME_BYTES).max(1));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{decode_line, AsrPayload};

    fn confirm(text: Option<&str>) -> Inbound {
        let t = match text {
            Some(t) => format!(r#","text":{}"#, serde_json::to_string(t).unwrap()),
            None => String::new(),
        };
        decode_line(&format!(r#"{{"v":1,"seq":3,"type":"key","payload":{{"id":"k6","action":"single","bind":"confirm"{t}}}}}"#))
            .expect("confirm frame")
    }

    #[test]
    fn confirm_with_text_sends_to_the_selected_chat() {
        assert_eq!(
            classify(&confirm(Some("ship it\nnow")), Some("dashboard:chat-1")),
            VoiceDirect::Send { slot: "dashboard:chat-1".into(), text: "ship it now".into() }
        );
    }

    #[test]
    fn failures_keep_the_text_when_there_is_one() {
        assert_eq!(
            classify(&confirm(Some("hello")), None),
            VoiceDirect::Fail { reason: "No chat selected (K1-K3)", text: Some("hello".into()) }
        );
        assert_eq!(classify(&confirm(Some("hello")), Some("  ")), classify(&confirm(Some("hello")), None));
        assert_eq!(
            classify(&confirm(None), Some("c")),
            VoiceDirect::Fail { reason: "Update keyboard firmware", text: None }
        );
        assert_eq!(classify(&confirm(Some(" \u{0007} ")), Some("c")), VoiceDirect::Fail { reason: "Nothing to send", text: None });
        let long = "x".repeat(crate::kirocrew_control::MAX_DEVICE_TEXT_CHARS + 1);
        assert!(matches!(classify(&confirm(Some(&long)), Some("c")), VoiceDirect::Fail { .. }));
    }

    #[test]
    fn asr_and_edits_are_consumed_and_others_pass() {
        let asr = Inbound::Asr { seq: None, payload: AsrPayload { text: "hi".into() } };
        assert_eq!(classify(&asr, Some("c")), VoiceDirect::Consumed);
        for bind in ["cancel_char", "cancel_all"] {
            let f = decode_line(&format!(r#"{{"v":1,"type":"key","payload":{{"id":"k4","action":"single","bind":"{bind}"}}}}"#)).unwrap();
            assert_eq!(classify(&f, Some("c")), VoiceDirect::Consumed, "{bind}");
        }
        let chat = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k1","action":"single","bind":"chat1"}}"#).unwrap();
        assert_eq!(classify(&chat, Some("c")), VoiceDirect::NotMine);
        let enc = decode_line(r#"{"v":1,"type":"key","payload":{"id":"enc","action":"single"}}"#).unwrap();
        assert_eq!(classify(&enc, Some("c")), VoiceDirect::NotMine);
    }

    #[test]
    fn failure_reasons_are_short() {
        assert_eq!(failure_reason("Kiro Crew send message: HTTP 403 forbidden"), "Crew refused: sign-in");
        assert_eq!(failure_reason("read Kiro Crew local secret /x: No such file"), "Crew not running");
        assert_eq!(failure_reason("Kiro Crew send message: error sending request: connect refused"), "Crew not running");
        assert_eq!(failure_reason("Kiro Crew send message: HTTP 409 busy"), "Send failed");
    }

    #[test]
    fn restore_frame_fits_and_keeps_utf8() {
        let f = input_restore_payload("你好 world", "Crew not running", SRC_RETRY);
        assert_eq!(f["type"], "input_restore");
        assert_eq!(f["payload"], serde_json::json!({"text":"你好 world","msg":"Crew not running","src":"retry"}));
        let f = input_restore_payload("", "没听清", SRC_PC);
        assert_eq!(f["payload"], serde_json::json!({"text":"","msg":"没听清","src":"pc"}));
        let quotes = "\"".repeat(300);
        let f = input_restore_payload(&quotes, "m", SRC_PC);
        assert!(serde_json::to_vec(&f).unwrap().len() <= MAX_FRAME_BYTES);
        assert!(f["payload"]["text"].as_str().unwrap().ends_with("..."));
        let cjk = "你".repeat(250);
        let f = input_restore_payload(&cjk, "", SRC_PC);
        assert!(serde_json::to_vec(&f).unwrap().len() <= MAX_FRAME_BYTES);
        assert!(f["payload"].get("msg").is_none());
    }
}
