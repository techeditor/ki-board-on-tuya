//! Voice input on the keyboard: two modes per chat slot.
//!
//! Keyboard mode (default). The recognised text stays on the keyboard's input
//! strip (home page). K4 edits it there, and K6 sends a `confirm` key frame
//! that carries the text and the slot; the Companion posts it to that slot's
//! chat through the Crew API. Nothing is typed, the Crew window need not be in
//! front, and the chat open on the PC does not matter. On failure the text
//! goes back to the strip (`input_restore`).
//!
//! PC mode: a slot with an Alt+N number (and "keyboard controls the PC" on).
//! The Companion presses Alt+N, types the text into the Crew input box
//! without sending, and tells the keyboard (`input_restore` with `filled`).
//! K6 then presses Enter, K4 Backspace (long press: clears the box) for a
//! short window ([`PC_FILL_WINDOW`]). If the window check or the typing fails,
//! the text goes to the strip instead.
//!
//! Pure: decides what a board frame means here; the app layer does the IO.

use std::time::Duration;

use serde_json::{Map, Value};

use crate::kirocrew_control::sanitize_device_text;
use crate::menu_wire::{clip_text, MAX_FRAME_BYTES, NOTICE_MAX};
use crate::protocol::{Inbound, KeyAction, PROTOCOL_VERSION};

/// The keyboard's input strip holds this many bytes (firmware
/// `AK_INPUT_TEXT_MAX`); `input_restore` text is cut to it.
pub const BOARD_TEXT_MAX: usize = 380;

/// How long K6 / K4 act on a PC fill. The keyboard's own window is
/// 20 s; this one is longer so the keyboard always ends first.
pub const PC_FILL_WINDOW: Duration = Duration::from_secs(25);

/// What a K6 / K4 press does to a filled PC input box.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PcKey {
    Enter,
    Backspace,
    Clear,
}

/// What the Companion does with a board frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VoiceDirect {
    /// Not a voice-input frame: route it as before.
    NotMine,
    /// A voice-input frame with nothing to do on the PC (the `asr` text of a
    /// keyboard-mode slot, the K4 edits of the strip): the keyboard
    /// handles it; never inject it.
    Consumed,
    /// K6 confirmed: post `text` to `slot` (a Crew API slot key).
    Send { slot: String, text: String },
    /// K6 confirmed but it cannot be sent. `text` (when there is one) goes
    /// back to the keyboard; `reason` is shown there.
    Fail { reason: &'static str, text: Option<String> },
    /// PC mode: fill the input box of chat slot `slot` (1..=3) with `text`.
    PcFill { slot: u8, text: String },
    /// PC mode: K6 / K4 on the filled input box.
    PcKey(PcKey),
}

/// What [`classify`] needs to know about the Companion's state.
pub struct VoiceCtx<'a> {
    /// The Crew API slot key bound to K1..K3 (A..C); `None` = not bound.
    pub slot_keys: [Option<String>; 3],
    /// The chat K1-K3 selected last. Only used for a `confirm` without `slot`.
    pub active_slot: Option<&'a str>,
    /// Slots whose voice text goes to the PC input box.
    pub pc: [bool; 3],
    /// A PC fill is waiting (see [`PC_FILL_WINDOW`]).
    pub pc_fill: bool,
}

/// Classify a board frame. The caller has checked that the keyboard may
/// control the PC.
pub fn classify(frame: &Inbound, ctx: &VoiceCtx) -> VoiceDirect {
    match frame {
        Inbound::Asr { payload, .. } => match payload.slot.filter(|s| (1..=3).contains(s)) {
            Some(slot) if ctx.pc[slot as usize - 1] => match sanitize_device_text(&payload.text) {
                Some(text) => VoiceDirect::PcFill { slot, text },
                None => VoiceDirect::Fail { reason: "Didn't catch that", text: None },
            },
            _ => VoiceDirect::Consumed,
        },
        Inbound::Key { payload, .. } => match payload.bind {
            Some(KeyAction::CancelChar) => pc_key_or_consumed(ctx, PcKey::Backspace),
            Some(KeyAction::CancelAll) => pc_key_or_consumed(ctx, PcKey::Clear),
            Some(KeyAction::Confirm) => {
                let Some(raw) = payload.text.as_deref() else {
                    // No text: the keyboard's K6 on a filled PC input box.
                    return if ctx.pc_fill {
                        VoiceDirect::PcKey(PcKey::Enter)
                    } else {
                        VoiceDirect::Fail { reason: "Nothing to send", text: None }
                    };
                };
                // Same rules as injected text: no control characters, a hard
                // length cap, empty means nothing to send.
                let Some(text) = sanitize_device_text(raw) else {
                    return VoiceDirect::Fail { reason: "Nothing to send", text: None };
                };
                let slot = match payload.slot.filter(|s| (1..=3).contains(s)) {
                    Some(n) => ctx.slot_keys[n as usize - 1].as_deref(),
                    None => ctx.active_slot,
                };
                match slot.map(str::trim).filter(|s| !s.is_empty()) {
                    Some(slot) => VoiceDirect::Send { slot: slot.to_string(), text },
                    None if payload.slot.is_some() => {
                        VoiceDirect::Fail { reason: "Chat not bound (Settings)", text: Some(text) }
                    }
                    None => VoiceDirect::Fail { reason: "No chat selected (K1-K3)", text: Some(text) },
                }
            }
            _ => VoiceDirect::NotMine,
        },
        _ => VoiceDirect::NotMine,
    }
}

/// A K1-K3 press picks (or jumps to) a chat. That ends a PC fill: Enter or
/// Backspace would otherwise hit the input box of the chat shown now.
pub fn ends_pc_fill(frame: &Inbound) -> bool {
    matches!(frame, Inbound::Key { payload, .. }
        if matches!(payload.bind, Some(KeyAction::Chat(_)))
            || (payload.bind.is_none()
                && payload.action == KeyAction::Single
                && matches!(payload.id.as_str(), "k1" | "k2" | "k3")))
}

fn pc_key_or_consumed(ctx: &VoiceCtx, key: PcKey) -> VoiceDirect {
    if ctx.pc_fill {
        VoiceDirect::PcKey(key)
    } else {
        VoiceDirect::Consumed
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

/// Where an `input_restore` text comes from (`src`): a failed send is shown
/// in red and leaves the chat history; a PC ASR result or a failed PC fill
/// is just more text for the strip.
pub const SRC_RETRY: &str = "retry";
pub const SRC_PC: &str = "pc";

/// Companion -> keyboard: put `text` on the input strip (appended to what is
/// there) with `msg` on the status bar. An empty `text` with `src` "pc" ends
/// the keyboard's wait in ② with `msg`. The text is cut to [`BOARD_TEXT_MAX`]
/// and clipped (UTF-8 safe) until the frame fits the keyboard's 511-byte line
/// limit.
pub fn input_restore_payload(text: &str, msg: &str, src: &str) -> Value {
    let msg = clip_text(msg, NOTICE_MAX);
    let mut budget = text.len().min(BOARD_TEXT_MAX);
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

/// Companion -> keyboard: the PC input box was filled; K6 / K4 now act
/// on it for a while. `msg` replaces the keyboard's default status text.
pub fn pc_filled_payload(msg: &str) -> Value {
    let mut p = Map::new();
    p.insert("text".into(), Value::String(String::new()));
    p.insert("filled".into(), Value::Bool(true));
    let msg = clip_text(msg, NOTICE_MAX);
    if !msg.is_empty() {
        p.insert("msg".into(), Value::String(msg));
    }
    serde_json::json!({ "v": PROTOCOL_VERSION, "type": "input_restore", "payload": Value::Object(p) })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{decode_line, AsrPayload};

        fn ctx<'a>(active: Option<&'a str>, pc: [bool; 3], pc_fill: bool) -> VoiceCtx<'a> {
        VoiceCtx {
            slot_keys: [Some("dashboard:chat-1".into()), Some("dashboard:chat-2".into()), None],
            active_slot: active,
            pc,
            pc_fill,
        }
    }

    fn confirm(text: Option<&str>, slot: Option<u8>) -> Inbound {
        let t = text.map(|t| format!(r#","text":{}"#, serde_json::to_string(t).unwrap())).unwrap_or_default();
        let s = slot.map(|s| format!(r#","slot":{s}"#)).unwrap_or_default();
        decode_line(&format!(r#"{{"v":1,"seq":3,"type":"key","payload":{{"id":"k6","action":"single","bind":"confirm"{t}{s}}}}}"#))
            .expect("confirm frame")
    }

    fn asr(text: &str, slot: Option<u8>) -> Inbound {
        Inbound::Asr { seq: None, payload: AsrPayload { text: text.into(), slot } }
    }

    #[test]
    fn confirm_sends_to_the_slot_it_names() {
        let c = ctx(Some("dashboard:chat-9"), [false; 3], false);
        assert_eq!(
            classify(&confirm(Some("ship it\nnow"), Some(2)), &c),
            VoiceDirect::Send { slot: "dashboard:chat-2".into(), text: "ship it now".into() },
            "the slot field wins over the chat K1-K3 selected last"
        );
        // Older firmware: no slot field, the last selected chat.
        assert_eq!(
            classify(&confirm(Some("hi"), None), &c),
            VoiceDirect::Send { slot: "dashboard:chat-9".into(), text: "hi".into() }
        );
    }

    #[test]
    fn failures_keep_the_text_when_there_is_one() {
        let c = ctx(None, [false; 3], false);
        assert_eq!(
            classify(&confirm(Some("hello"), None), &c),
            VoiceDirect::Fail { reason: "No chat selected (K1-K3)", text: Some("hello".into()) }
        );
        assert_eq!(
            classify(&confirm(Some("hello"), Some(3)), &ctx(Some("x"), [false; 3], false)),
            VoiceDirect::Fail { reason: "Chat not bound (Settings)", text: Some("hello".into()) },
            "an unbound slot never falls back to another chat"
        );
        assert_eq!(classify(&confirm(None, Some(1)), &c), VoiceDirect::Fail { reason: "Nothing to send", text: None });
        assert_eq!(classify(&confirm(Some(" \u{0007} "), Some(1)), &c), VoiceDirect::Fail { reason: "Nothing to send", text: None });
        let long = "x".repeat(crate::kirocrew_control::MAX_DEVICE_TEXT_CHARS + 1);
        assert!(matches!(classify(&confirm(Some(&long), Some(1)), &c), VoiceDirect::Fail { .. }));
    }

    #[test]
    fn keyboard_mode_asr_and_edits_are_consumed() {
        let c = ctx(Some("c"), [false, true, false], false);
        assert_eq!(classify(&asr("hi", Some(1)), &c), VoiceDirect::Consumed);
        assert_eq!(classify(&asr("hi", None), &c), VoiceDirect::Consumed, "no slot: the strip already has text");
        for bind in ["cancel_char", "cancel_all"] {
            let f = decode_line(&format!(r#"{{"v":1,"type":"key","payload":{{"id":"k4","action":"single","bind":"{bind}"}}}}"#)).unwrap();
            assert_eq!(classify(&f, &c), VoiceDirect::Consumed, "{bind}");
        }
        let chat = decode_line(r#"{"v":1,"type":"key","payload":{"id":"k1","action":"single","bind":"chat1"}}"#).unwrap();
        assert_eq!(classify(&chat, &c), VoiceDirect::NotMine);
        let enc = decode_line(r#"{"v":1,"type":"key","payload":{"id":"enc","action":"single"}}"#).unwrap();
        assert_eq!(classify(&enc, &c), VoiceDirect::NotMine);
    }

    #[test]
    fn pc_mode_fills_and_then_k6_k4_k1_act_on_the_box() {
        let c = ctx(Some("c"), [false, true, false], false);
        assert_eq!(classify(&asr("你好\n世界", Some(2)), &c), VoiceDirect::PcFill { slot: 2, text: "你好 世界".into() });
        assert_eq!(
            classify(&asr("\u{0007}", Some(2)), &c),
            VoiceDirect::Fail { reason: "Didn't catch that", text: None }
        );
        // Without a fill pending the edits stay on the keyboard.
        assert_eq!(classify(&confirm(None, None), &c), VoiceDirect::Fail { reason: "Nothing to send", text: None });
        let filled = ctx(Some("c"), [false, true, false], true);
        assert_eq!(classify(&confirm(None, None), &filled), VoiceDirect::PcKey(PcKey::Enter));
        for (bind, key) in [("cancel_char", PcKey::Backspace), ("cancel_all", PcKey::Clear)] {
            let f = decode_line(&format!(r#"{{"v":1,"type":"key","payload":{{"id":"k4","action":"single","bind":"{bind}"}}}}"#)).unwrap();
            assert_eq!(classify(&f, &filled), VoiceDirect::PcKey(key), "{bind}");
        }
    }

    #[test]
    fn choosing_another_chat_ends_a_pc_fill() {
        let key = |id: &str, action: &str, bind: &str| {
            let bind = if bind.is_empty() { String::new() } else { format!(r#","bind":"{bind}""#) };
            decode_line(&format!(r#"{{"v":1,"type":"key","payload":{{"id":"{id}","action":"{action}"{bind}}}}}"#)).unwrap()
        };
        assert!(ends_pc_fill(&key("k2", "single", "chat2")));
        assert!(ends_pc_fill(&key("k1", "single", "")));
        assert!(!ends_pc_fill(&key("k4", "single", "cancel_char")));
        assert!(!ends_pc_fill(&key("k6", "single", "confirm")));
        assert!(!ends_pc_fill(&key("k1", "long", "")));
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
        assert!(f["payload"]["text"].as_str().unwrap().len() <= BOARD_TEXT_MAX);
        assert!(f["payload"].get("msg").is_none());
    }

    #[test]
    fn filled_frame_has_no_text() {
        let f = pc_filled_payload("");
        assert_eq!(f["payload"], serde_json::json!({"text":"","filled":true}));
        assert_eq!(pc_filled_payload("ok")["payload"]["msg"], "ok");
    }
}
