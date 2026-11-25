//! FP-4b Route B — the *hands*: drive a **local** Kiro Crew window by focusing
//! it and injecting keystrokes (no HTTP, no session-control API). This is the
//! control plane that supersedes route A for the actual control path.
//!
//! Two layers live here:
//!
//! * [`KiroCrewController`] — Windows-gated `user32` FFI backend that finds the
//!   Kiro Crew window by a configurable title substring, accepts it only when
//!   the owning process is the Crew desktop executable
//!   ([`ALLOWED_CREW_PROCESSES`]), brings it to the foreground, verifies it is
//!   the foreground window, and only then injects text / the Ctrl+K command
//!   bar / named chords. It
//!   reuses the raw `SendInput` pattern from [`crate::inject`] (no new crate).
//!   On non-Windows hosts a stub compiles and returns `Err` / no-ops so the
//!   Linux CI build and the pure router tests below still work.
//!
//! * [`ControlRouter`] / [`route_input`] — a **pure**, IO-free mapping from a
//!   decoded board [`Inbound`] event to a [`ControlAction`]. Table-driven and
//!   unit-tested on Linux (the tests at the bottom run in `cargo test`).

use crate::protocol::{Inbound, KeyAction};

// ───────────────────────────── Chord ─────────────────────────────

/// A named key / key-combination the controller can inject. Kept small and
/// `Copy` so the pure router can hand one to the (Windows) backend by value.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Chord {
    Enter,
    Esc,
    Up,
    Down,
    Left,
    Right,
    /// Backspace — delete the character before the composer caret.
    Backspace,
    /// Ctrl+Backspace — delete the word before the composer caret.
    DeleteWord,
    /// Ctrl+A, Backspace — clear the current composer sentence.
    ClearInput,
    /// Ctrl+K — open the Kiro Crew command bar.
    CtrlK,
    /// Alt+N — jump to chat N (N = 1..=9). Deterministic, unlike a command-bar
    /// text search. (Kiro Crew default shortcut "Jump to chat N".)
    JumpChat(u8),
    /// Ctrl+N — switch Crew instance N (N = 1..=9). Kiro Crew's "Remote Crews"
    /// shortcuts: Ctrl+1 = Switch to Local, Ctrl+2 = remote crew 1,
    /// Ctrl+3 = remote crew 2, … Driven by a K1-K3 long press (500 ms).
    SwitchInstance(u8),
    /// Alt+Left — previous chat.
    PrevChat,
    /// Alt+Right — next chat.
    NextChat,
    /// Alt+Enter — focus the chat text input (composer).
    FocusInput,
    /// Alt+Shift+N — new chat.
    NewChat,
}

impl Chord {
    /// Parse the wire / UI name (case-insensitive) into a chord. Returns `None`
    /// for an unrecognized name so callers can reject rather than guess.
    pub fn parse(name: &str) -> Option<Chord> {
        match name.trim().to_ascii_lowercase().as_str() {
            "enter" | "return" => Some(Chord::Enter),
            "esc" | "escape" => Some(Chord::Esc),
            "up" => Some(Chord::Up),
            "down" => Some(Chord::Down),
            "left" => Some(Chord::Left),
            "right" => Some(Chord::Right),
            "backspace" | "delete_char" | "delete-char" => Some(Chord::Backspace),
            "deleteword" | "delete_word" | "delete-word" => Some(Chord::DeleteWord),
            "clearinput" | "clear_input" | "clear-input" => Some(Chord::ClearInput),
            "ctrlk" | "ctrl+k" | "command_bar" | "commandbar" => Some(Chord::CtrlK),
            "prevchat" | "prev_chat" | "prev-chat" => Some(Chord::PrevChat),
            "nextchat" | "next_chat" | "next-chat" => Some(Chord::NextChat),
            "focusinput" | "focus_input" | "focus-input" => Some(Chord::FocusInput),
            "newchat" | "new_chat" | "new-chat" => Some(Chord::NewChat),
            other => {
                // "chat1".."chat9" -> JumpChat(1..=9)
                if let Some(d) = other.strip_prefix("chat") {
                    if let Ok(n) = d.parse::<u8>() {
                        if (1..=9).contains(&n) {
                            return Some(Chord::JumpChat(n));
                        }
                    }
                }
                // "instance1".."instance9" -> SwitchInstance(1..=9)
                if let Some(d) = other.strip_prefix("instance") {
                    if let Ok(n) = d.parse::<u8>() {
                        if (1..=9).contains(&n) {
                            return Some(Chord::SwitchInstance(n));
                        }
                    }
                }
                None
            }
        }
    }
}

// ─────────────────────── Pure ControlRouter (T2) ───────────────────────

/// FR-03 limited shortcuts, index-aligned with `scene::SHORTCUTS`
/// (`key_enter` .. `key_new_chat`). Only existing Kiro Crew window chords.
pub const SHORTCUT_CHORDS: [Chord; 14] = [
    Chord::Enter,
    Chord::Esc,
    Chord::Up,
    Chord::Down,
    Chord::Left,
    Chord::Right,
    Chord::Backspace,
    Chord::DeleteWord,
    Chord::ClearInput,
    Chord::CtrlK,
    Chord::PrevChat,
    Chord::NextChat,
    Chord::FocusInput,
    Chord::NewChat,
];

/// Chord of shortcut N (1-based), or `None` out of range.
pub fn shortcut_chord(n: u8) -> Option<Chord> {
    SHORTCUT_CHORDS.get((n as usize).checked_sub(1)?).copied()
}

/// The action the router decided a board event should produce. Consumed by the
/// (Windows) [`KiroCrewController`]; pure and IO-free so it is fully testable.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlAction {
    /// Open the command bar (Ctrl+K), type the string, press Enter.
    CommandBar(String),
    /// Inject a single named chord.
    Chord(Chord),
    /// Inject a chord N times in a row (N>=1). Used by the EC11 encoder so a
    /// multi-detent frame scrolls by that many steps.
    ChordRepeat(Chord, u8),
    /// Type free text into the focused input (no command bar, no Enter).
    SendText(String),
    /// Focus the chat composer (Alt+Enter), then type the text — no Enter, so
    /// the user reviews and submits with K5. This is the K4 voice path.
    Compose(String),
    /// Nothing to do for this event (e.g. a key *up*, a ping, an ack).
    None,
}

/// Router context — the small amount of state the pure mapping needs but does
/// not own. `jump_chats[i]` is the chat NUMBER (1..=9) that K(i+1) (K1..K3)
/// jumps to via the deterministic `Alt+N` shortcut. Defaults to `[1, 2, 3]`.
/// The number for each key is user-configurable (persisted in the bind store)
/// and kept in sync here by `lib.rs`.
#[derive(Clone, Debug, Default)]
pub struct ControlCtx {
    /// Chat numbers for K1, K2, K3 (index 0..=2). Configurable by the UI.
    pub jump_chats: Vec<u8>,
    /// What EC11 rotation does in standby (the current scene's `knob`, FR-03).
    pub knob: KnobMode,
}

/// FR-03 scene `knob`: EC11 rotation in standby.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum KnobMode {
    /// Up / Down arrow per detent (the v0.3 behaviour).
    #[default]
    Arrows,
    /// Previous / next chat (Alt+Left / Alt+Right) per detent.
    Chats,
    /// Nothing is injected.
    Off,
}

impl KnobMode {
    /// Scene wire name -> mode; unknown names keep the default.
    pub fn parse(s: &str) -> KnobMode {
        match s {
            "chats" => KnobMode::Chats,
            "none" => KnobMode::Off,
            _ => KnobMode::Arrows,
        }
    }
}

impl ControlCtx {
    /// Sensible defaults: K1->chat 1, K2->chat 2, K3->chat 3 (Alt+1/2/3);
    /// long-press K1->instance 1 (Local), K2->instance 2, K3->instance 3
    /// (Ctrl+1/2/3).
    pub fn with_default_jumps() -> Self {
        Self { jump_chats: vec![1, 2, 3], knob: KnobMode::Arrows }
    }

    // Note: chat/instance targets travel on the wire in the gesture's `bind`
    // (KeyAction::Chat(n)/Instance(n)) — route_input reads N from the frame, not
    // from this ctx. jump_chats remains only as the persisted
    // default-number store surfaced by the bind UI; they no longer gate routing.
}

/// Must match firmware/app/common/include/ak_input_fsm.h. Retained for future
/// long-press bindings (K6 / the EC11 push); not referenced by the v0.3 router.
#[allow(dead_code)]
const BOARD_LONG_PRESS_MS: u32 = 800;

/// Pure mapping: board event + context → control action. No IO.
///
/// Mapping table (v0.3, shared/protocol/protocol.md "key"). Every normal key and
/// the EC11 push send one gesture frame per physical operation
/// (`single`/`double`/`long`); `bind` carries the action the firmware resolved
/// from its keymap. The router acts on `bind` first.
///
/// | event                                | action |
/// |--------------------------------------|--------|
/// | `asr`/`macro { text }`               | `Compose(text)` — type into the composer (no send) |
/// | key, `bind: chatN`                   | `Chord(JumpChat(n))` — Alt+N |
/// | key, `bind: instanceN`               | `Chord(SwitchInstance(n))` — Ctrl+N |
/// | key, `bind: cancel_char`             | `Chord(Backspace)` |
/// | key, `bind: cancel_all`              | `Chord(ClearInput)` — Ctrl+A, Backspace |
/// | key, `bind: confirm`                 | `Chord(Enter)` |
/// | key, other `bind` (e.g. `settings`)  | `None` — local action on the board |
/// | `enc` `single`, no bind              | `Chord(Enter)` |
/// | K1-K3 `single`/`long`, no bind       | `None` here — Crew API slot select in the app layer; `long` does nothing |
/// | voice key `down`/`up`                | `None` — voice edges, text arrives as `asr`/`macro` |
/// | `encoder { detents>0 }`              | `ChordRepeat(Down, detents)` |
/// | `encoder { detents<0 }`              | `ChordRepeat(Up, |detents|)` |
/// | anything else                        | `None` |
pub fn route_input(ev: &Inbound, ctx: &ControlCtx) -> ControlAction {
    match ev {
        // Recognized speech text -> type into the chat composer (NO send). Both
        // the board-side cloud ASR (`asr` frame) and the PC speech/macro path
        // (`macro` frame) land here, so BOTH voice routes behave identically:
        // the text appears in the Kiro Crew composer and the user submits it
        // with K5 (Enter). No auto-send, so a misrecognition can be reviewed.
        Inbound::Asr { payload, .. } => compose_device_text(&payload.text),
        // K4's recognized speech → type into the chat composer (no send).
        Inbound::Macro { payload, .. } => compose_device_text(&payload.name),

        // Bound gesture: the firmware already resolved (and, for pending-confirm
        // edits, ran) the action; the Companion performs the PC half. Matched
        // for every key id because any (key, gesture) can be bound.
        //   chatN -> Alt+N, instanceN -> Ctrl+N, cancel_char -> Backspace,
        //   cancel_all -> Ctrl+A Backspace, confirm -> Enter.
        // A local-only bind (e.g. `settings` on the EC11 long-press) is None.
        Inbound::Key { payload, .. } if payload.bind.is_some() => match payload.bind {
            Some(KeyAction::Chat(n)) => ControlAction::Chord(Chord::JumpChat(n)),
            Some(KeyAction::Instance(n)) => ControlAction::Chord(Chord::SwitchInstance(n)),
            Some(KeyAction::CancelChar) => ControlAction::Chord(Chord::Backspace),
            Some(KeyAction::CancelAll) => ControlAction::Chord(Chord::ClearInput),
            Some(KeyAction::Confirm) => ControlAction::Chord(Chord::Enter),
            // FR-03 limited shortcut: the firmware only reports it in ① standby.
            Some(KeyAction::Shortcut(n)) => shortcut_chord(n).map_or(ControlAction::None, ControlAction::Chord),
            _ => ControlAction::None,
        },

        // Unbound EC11 push single = confirm (Enter), the v0.3 default.
        Inbound::Key { payload, .. }
            if payload.id == "enc" && payload.action == KeyAction::Single =>
        {
            ControlAction::Chord(Chord::Enter)
        }

        // Everything else is a no-op here: unbound K1-K3 `single` selects the
        // Crew API slot (app layer), an unbound `long` is kept free (sessions
        // are bound from Settings > Bind session), and the voice key's down/up
        // edges start/stop speech input.

        // EC11 rotation: scroll by one arrow key per detent. Clockwise
        // (positive) moves down / to the next item; counter-clockwise (negative)
        // moves up. The chord is injected once per detent (capped to avoid a
        // runaway from a malformed frame).
        Inbound::Encoder { payload, .. } => {
            let mag = payload.detents.unsigned_abs().min(16) as u8;
            let (fwd, back) = match ctx.knob {
                KnobMode::Arrows => (Chord::Down, Chord::Up),
                KnobMode::Chats => (Chord::NextChat, Chord::PrevChat),
                KnobMode::Off => return ControlAction::None,
            };
            if mag == 0 {
                ControlAction::None
            } else if payload.detents > 0 {
                ControlAction::ChordRepeat(fwd, mag)
            } else {
                ControlAction::ChordRepeat(back, mag)
            }
        }

        // Key/nav `up` edges, voice edges, ping/pong/ack, hello_ack, unknown.
        // Any frame type this router does not know (including future
        // display-only frames such as Chat replies) is never injected.
        // Chat-mode turns (CP-01): the agent's reply and the user's Chat
        // utterance are shown in the Chat panel only, never typed into Crew.
        Inbound::TuyaChat { .. } => ControlAction::None,
        _ => ControlAction::None,
    }
}

/// Longest device-supplied text (asr/macro) the Companion will type, in chars.
/// Longer text is refused, not truncated, so a partial command is never typed.
pub const MAX_DEVICE_TEXT_CHARS: usize = 1000;

/// Make device-supplied text safe to type into the composer (CP-01).
///
/// Board frames are unauthenticated, so the text must never carry a key that
/// acts on its own: CR/LF (a typed Enter would submit), Tab, Esc and every
/// other control character are removed (line breaks and tabs become a space),
/// as are the bidi override/isolate characters that can hide text. Submitting
/// stays a separate, explicit confirm key on the device. Returns `None` for
/// empty or over-long text.
pub fn sanitize_device_text(raw: &str) -> Option<String> {
    let mut out = String::with_capacity(raw.len());
    for c in raw.chars() {
        match c {
            '\r' | '\n' | '\t' | '\u{2028}' | '\u{2029}' => out.push(' '),
            '\u{202A}'..='\u{202E}' | '\u{2066}'..='\u{2069}' | '\u{200E}' | '\u{200F}' => {}
            c if c.is_control() => {}
            c => out.push(c),
        }
    }
    let text = out.trim();
    if text.is_empty() || text.chars().count() > MAX_DEVICE_TEXT_CHARS {
        return None;
    }
    Some(text.to_string())
}

fn compose_device_text(raw: &str) -> ControlAction {
    match sanitize_device_text(raw) {
        Some(text) => ControlAction::Compose(text),
        None => ControlAction::None,
    }
}

/// Executable names whose windows may receive injected keys (CP-02). The
/// Windows Kiro Crew desktop app installs as `KiroCrew.exe`. A browser tab
/// titled "Kiro Crew" is deliberately NOT a target: any page can set that
/// title.
pub const ALLOWED_CREW_PROCESSES: &[&str] = &["KiroCrew.exe"];

/// Whether a full process image path belongs to an allowed Crew executable.
/// Compares the file name only, case-insensitively (Windows semantics).
pub fn is_allowed_crew_image(image_path: &str) -> bool {
    let name = image_path.rsplit(['\\', '/']).next().unwrap_or("");
    ALLOWED_CREW_PROCESSES
        .iter()
        .any(|allowed| allowed.eq_ignore_ascii_case(name))
}

/// Thin stateful wrapper so callers can hold configuration (the `ControlCtx`)
/// alongside the routing entry point. Pure — no IO.
#[derive(Clone, Debug, Default)]
pub struct ControlRouter {
    pub ctx: ControlCtx,
}

impl ControlRouter {
    pub fn new(ctx: ControlCtx) -> Self {
        Self { ctx }
    }

    pub fn route(&self, ev: &Inbound) -> ControlAction {
        route_input(ev, &self.ctx)
    }
}

// ─────────────────────── KiroCrewController (T1) ───────────────────────

/// Default window-title substrings tried in order when none is configured.
pub const DEFAULT_TITLE_CANDIDATES: &[&str] = &["Kiro Crew", "Agentic"];

/// Wait between bringing the Crew window to the front and injecting the
/// instance-switch Ctrl+N (FR-05, 100-200 ms).
pub const INSTANCE_SWITCH_SETTLE_MS: u64 = 150;

/// One `[KEY-TIMING]` log line for a K1-K3 press: the path it took and the
/// milliseconds each stage took, measured from the frame reaching the
/// Companion (`frame -> ... -> total`). `window` is how long the keyboard held
/// the gesture back after the release (the double-click window; 0 when the
/// key has no double binding); `hold` is the press time of a long press, which
/// is not part of the latency, so it is not added to `total`.
pub fn timing_line(key: &str, gesture: &str, path: &str, window_ms: u32, hold_ms: Option<u32>, stages: &[(&str, std::time::Duration)]) -> String {
    let mut out = format!("[KEY-TIMING] {key} {gesture} path={path} window={window_ms}ms");
    if let Some(hold) = hold_ms {
        out.push_str(&format!(" hold={hold}ms"));
    }
    let mut total = std::time::Duration::ZERO;
    for (name, d) in stages {
        total += *d;
        out.push_str(&format!(" {name}={}ms", d.as_millis()));
    }
    out.push_str(&format!(" total={}ms", total.as_millis()));
    out
}

// ============================== Windows ==============================

#[cfg(windows)]
mod windows_impl {
    use super::{is_allowed_crew_image, Chord, DEFAULT_TITLE_CANDIDATES, INSTANCE_SWITCH_SETTLE_MS};
    use std::mem::size_of;

    // --- Minimal Win32 bindings (raw FFI, mirrors inject.rs — no `windows` crate). ---
    type HWND = isize;
    type LPARAM = isize;
    type BOOL = i32;
    type WPARAM = usize;

    const INPUT_KEYBOARD: u32 = 1;
    const KEYEVENTF_EXTENDEDKEY: u32 = 0x0001;
    const KEYEVENTF_KEYUP: u32 = 0x0002;
    const KEYEVENTF_UNICODE: u32 = 0x0004;
    const KEYEVENTF_SCANCODE: u32 = 0x0008;
    const MAPVK_VK_TO_VSC: u32 = 0;

    // Virtual-key codes we use for chords / the command bar.
    const VK_RETURN: u16 = 0x0D;
    const VK_ESCAPE: u16 = 0x1B;
    const VK_BACK: u16 = 0x08;
    const VK_LEFT: u16 = 0x25;
    const VK_UP: u16 = 0x26;
    const VK_RIGHT: u16 = 0x27;
    const VK_DOWN: u16 = 0x28;
    const VK_CONTROL: u16 = 0x11;
    const VK_MENU: u16 = 0x12; // Alt
    const VK_SHIFT: u16 = 0x10;
    const VK_A: u16 = 0x41; // 'A'
    const VK_K: u16 = 0x4B; // 'K'
    const VK_N: u16 = 0x4E; // 'N'

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct KEYBDINPUT {
        w_vk: u16,
        w_scan: u16,
        dw_flags: u32,
        time: u32,
        dw_extra_info: usize,
    }

    #[repr(C)]
    #[derive(Clone, Copy)]
    struct INPUT {
        r#type: u32,
        ki: KEYBDINPUT,
        _pad: [u8; 8],
    }

    // EnumWindows callback signature.
    type WNDENUMPROC = unsafe extern "system" fn(HWND, LPARAM) -> BOOL;

    #[link(name = "user32")]
    extern "system" {
        fn SendInput(c_inputs: u32, p_inputs: *const INPUT, cb_size: i32) -> u32;
        fn MapVirtualKeyW(u_code: u32, u_map_type: u32) -> u32;
        fn EnumWindows(lp_enum_func: WNDENUMPROC, l_param: LPARAM) -> BOOL;
        fn GetWindowTextW(hwnd: HWND, lp_string: *mut u16, n_max_count: i32) -> i32;
        fn GetWindowTextLengthW(hwnd: HWND) -> i32;
        fn IsWindowVisible(hwnd: HWND) -> BOOL;
        fn SetForegroundWindow(hwnd: HWND) -> BOOL;
        fn ShowWindow(hwnd: HWND, n_cmd_show: i32) -> BOOL;
        fn IsIconic(hwnd: HWND) -> BOOL;
        fn SendMessageW(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> isize;
        fn GetForegroundWindow() -> HWND;
        fn GetWindowThreadProcessId(hwnd: HWND, lp_process_id: *mut u32) -> u32;
        fn AttachThreadInput(id_attach: u32, id_attach_to: u32, f_attach: BOOL) -> BOOL;
        fn BringWindowToTop(hwnd: HWND) -> BOOL;
    }

    #[link(name = "kernel32")]
    extern "system" {
        fn GetCurrentThreadId() -> u32;
        fn OpenProcess(desired_access: u32, inherit_handle: BOOL, process_id: u32) -> HANDLE;
        fn QueryFullProcessImageNameW(
            process: HANDLE,
            flags: u32,
            exe_name: *mut u16,
            size: *mut u32,
        ) -> BOOL;
        fn CloseHandle(handle: HANDLE) -> BOOL;
    }

    type HANDLE = isize;
    const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
    const SW_RESTORE: i32 = 9;
    const SW_SHOW: i32 = 5;

    // --- SendInput helpers (same shape as inject.rs). ---

    fn send(inputs: &[INPUT]) -> Result<(), String> {
        let sent = unsafe { SendInput(inputs.len() as u32, inputs.as_ptr(), size_of::<INPUT>() as i32) };
        if sent as usize == inputs.len() {
            Ok(())
        } else {
            Err(format!("SendInput injected {sent}/{} events", inputs.len()))
        }
    }

    fn unicode_event(unit: u16, key_up: bool) -> INPUT {
        let mut flags = KEYEVENTF_UNICODE;
        if key_up {
            flags |= KEYEVENTF_KEYUP;
        }
        INPUT {
            r#type: INPUT_KEYBOARD,
            ki: KEYBDINPUT { w_vk: 0, w_scan: unit, dw_flags: flags, time: 0, dw_extra_info: 0 },
            _pad: [0; 8],
        }
    }

    /// Build a physical key event with a scan code whenever Windows can map the
    /// virtual key. Kiro Crew routes chat shortcuts by `KeyboardEvent.code`
    /// (`Digit1` … `Digit9`); Chromium/WebView cannot reliably derive that code
    /// from a SendInput packet whose scan code is zero. Fall back to the virtual
    /// key for the rare values MapVirtualKeyW cannot map.
    fn vkey_event(vk: u16, key_up: bool) -> INPUT {
        let scan = unsafe { MapVirtualKeyW(vk as u32, MAPVK_VK_TO_VSC) } as u16;
        let mut flags = if key_up { KEYEVENTF_KEYUP } else { 0 };
        let extended = matches!(vk, VK_LEFT | VK_UP | VK_RIGHT | VK_DOWN);
        let (w_vk, w_scan) = if scan != 0 {
            flags |= KEYEVENTF_SCANCODE;
            if extended {
                flags |= KEYEVENTF_EXTENDEDKEY;
            }
            (0, scan)
        } else {
            (vk, 0)
        };
        INPUT {
            r#type: INPUT_KEYBOARD,
            ki: KEYBDINPUT {
                w_vk,
                w_scan,
                dw_flags: flags,
                time: 0,
                dw_extra_info: 0,
            },
            _pad: [0; 8],
        }
    }

    fn type_text_raw(text: &str) -> Result<(), String> {
        let mut events = Vec::with_capacity(text.len() * 2);
        for unit in text.encode_utf16() {
            events.push(unicode_event(unit, false));
            events.push(unicode_event(unit, true));
        }
        if events.is_empty() {
            return Ok(());
        }
        send(&events)
    }

    fn tap(vk: u16) -> Result<(), String> {
        send(&[vkey_event(vk, false), vkey_event(vk, true)])
    }

    /// LCTRL + <vk> as one SendInput batch.
    fn ctrl_tap(vk: u16) -> Result<(), String> {
        send(&[
            vkey_event(VK_CONTROL, false),
            vkey_event(vk, false),
            vkey_event(vk, true),
            vkey_event(VK_CONTROL, true),
        ])
    }

    /// Ctrl+A followed by Backspace clears the focused composer.
    fn clear_input() -> Result<(), String> {
        send(&[
            vkey_event(VK_CONTROL, false),
            vkey_event(VK_A, false),
            vkey_event(VK_A, true),
            vkey_event(VK_CONTROL, true),
            vkey_event(VK_BACK, false),
            vkey_event(VK_BACK, true),
        ])
    }

    /// LCTRL↓ K↓ K↑ LCTRL↑ — the command-bar hotkey.
    fn ctrl_k() -> Result<(), String> {
        send(&[
            vkey_event(VK_CONTROL, false),
            vkey_event(VK_K, false),
            vkey_event(VK_K, true),
            vkey_event(VK_CONTROL, true),
        ])
    }

    /// Alt + <vk> (Alt↓ vk↓ vk↑ Alt↑).
    fn alt_tap(vk: u16) -> Result<(), String> {
        send(&[
            vkey_event(VK_MENU, false),
            vkey_event(vk, false),
            vkey_event(vk, true),
            vkey_event(VK_MENU, true),
        ])
    }

    /// Alt + Shift + <vk>.
    fn alt_shift_tap(vk: u16) -> Result<(), String> {
        send(&[
            vkey_event(VK_MENU, false),
            vkey_event(VK_SHIFT, false),
            vkey_event(vk, false),
            vkey_event(vk, true),
            vkey_event(VK_SHIFT, true),
            vkey_event(VK_MENU, true),
        ])
    }

    /// Inject one chord. Simple chords tap a single VK; the Kiro Crew shortcuts
    /// use Alt / Alt+Shift combos.
    fn inject_chord(c: Chord) -> Result<(), String> {
        match c {
            Chord::Enter => tap(VK_RETURN),
            Chord::Esc => tap(VK_ESCAPE),
            Chord::Up => tap(VK_UP),
            Chord::Down => tap(VK_DOWN),
            Chord::Left => tap(VK_LEFT),
            Chord::Right => tap(VK_RIGHT),
            Chord::Backspace => tap(VK_BACK),
            Chord::DeleteWord => ctrl_tap(VK_BACK),
            Chord::ClearInput => clear_input(),
            Chord::CtrlK => ctrl_k(),
            Chord::JumpChat(n) if (1..=9).contains(&n) => alt_tap(0x30 + n as u16), // Alt+'1'..'9'
            Chord::JumpChat(n) => Err(format!("JumpChat({n}) out of range 1..=9")),
            Chord::SwitchInstance(n) if (1..=9).contains(&n) => ctrl_tap(0x30 + n as u16), // Ctrl+'1'..'9'
            Chord::SwitchInstance(n) => Err(format!("SwitchInstance({n}) out of range 1..=9")),
            Chord::PrevChat => alt_tap(VK_LEFT),  // Alt+Left
            Chord::NextChat => alt_tap(VK_RIGHT), // Alt+Right
            Chord::FocusInput => alt_tap(VK_RETURN), // Alt+Enter
            Chord::NewChat => alt_shift_tap(VK_N),   // Alt+Shift+N
        }
    }

    // --- Window enumeration + focus. ---

    struct FindCtx {
        needle_lower: String,
        found: HWND,
    }

    /// Full image path of the process owning `hwnd`, or `None`.
    unsafe fn window_process_image(hwnd: HWND) -> Option<String> {
        let mut pid: u32 = 0;
        GetWindowThreadProcessId(hwnd, &mut pid);
        if pid == 0 {
            return None;
        }
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if process == 0 {
            return None;
        }
        let mut buf = vec![0u16; 1024];
        let mut len = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(process, 0, buf.as_mut_ptr(), &mut len);
        CloseHandle(process);
        if ok == 0 {
            return None;
        }
        Some(String::from_utf16_lossy(&buf[..len as usize]))
    }

    unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
        let ctx = &mut *(lparam as *mut FindCtx);
        if IsWindowVisible(hwnd) == 0 {
            return 1; // keep enumerating
        }
        let len = GetWindowTextLengthW(hwnd);
        if len <= 0 {
            return 1;
        }
        let mut buf = vec![0u16; (len as usize) + 1];
        let copied = GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
        if copied <= 0 {
            return 1;
        }
        let title = String::from_utf16_lossy(&buf[..copied as usize]);
        if !title.to_lowercase().contains(&ctx.needle_lower) {
            return 1;
        }
        // CP-02: a title is attacker-controllable (any browser tab or document
        // can say "Kiro Crew"); only a window owned by the Crew executable counts.
        let allowed = window_process_image(hwnd)
            .map(|image| is_allowed_crew_image(&image))
            .unwrap_or(false);
        if allowed {
            ctx.found = hwnd;
            return 0; // stop enumerating — first visible verified match wins
        }
        1
    }

    /// Find the first visible top-level window whose title contains `needle`
    /// (case-insensitive) AND whose process is an allowed Crew executable.
    fn find_window_by_title(needle: &str) -> Option<HWND> {
        let mut ctx = FindCtx { needle_lower: needle.to_lowercase(), found: 0 };
        unsafe {
            EnumWindows(enum_proc, &mut ctx as *mut FindCtx as LPARAM);
        }
        if ctx.found != 0 {
            Some(ctx.found)
        } else {
            None
        }
    }

    /// Windows controller: focuses a Kiro Crew window then injects.
    pub struct WinController {
        hwnd: Option<HWND>,
        title_match: String,
    }

    impl WinController {
        pub fn new(title_match: Option<String>) -> Self {
            Self {
                hwnd: None,
                title_match: title_match.unwrap_or_default(),
            }
        }

        pub fn set_title_match(&mut self, title: String) {
            self.title_match = title;
            self.hwnd = None; // force re-resolve on next focus
        }

        /// Resolve + foreground the Kiro Crew window. Verifies a match BEFORE any
        /// injection (AC1.5); returns `Err` and injects nothing if none found.
        pub fn focus(&mut self) -> Result<(), String> {
            self.focus_with(true)
        }

        /// Bring the window to the front without synthesising any key event
        /// (no foreground-lock Alt tap). May fail where `focus` would succeed.
        pub fn bring_to_front(&mut self) -> Result<(), String> {
            self.focus_with(false)
        }

        fn focus_with(&mut self, alt_tap: bool) -> Result<(), String> {
            // Candidate list: the configured substring first, else the defaults.
            let candidates: Vec<String> = if self.title_match.trim().is_empty() {
                DEFAULT_TITLE_CANDIDATES.iter().map(|s| s.to_string()).collect()
            } else {
                vec![self.title_match.clone()]
            };

            let hwnd = candidates
                .iter()
                .find_map(|needle| find_window_by_title(needle))
                .ok_or_else(|| {
                    format!(
                        "control: no KiroCrew.exe window matches title {:?} (nothing injected)",
                        if self.title_match.trim().is_empty() {
                            DEFAULT_TITLE_CANDIDATES.join(" / ")
                        } else {
                            self.title_match.clone()
                        }
                    )
                })?;

            // Force the target to the foreground. A bare SetForegroundWindow is
            // refused by Windows' focus-stealing rules when our process is not the
            // current foreground owner, so the injected keys would land in whatever
            // window is actually focused. The reliable path is to attach OUR input
            // queue AND the current foreground window's input queue to the target's
            // thread for the duration of the call — Windows then treats us as the
            // foreground thread and honours SetForegroundWindow. Detach in all cases.
            let ok = unsafe {
                let fg = GetForegroundWindow();
                let target_thread = GetWindowThreadProcessId(hwnd, std::ptr::null_mut());
                let fg_thread = GetWindowThreadProcessId(fg, std::ptr::null_mut());
                let cur_thread = GetCurrentThreadId();

                let attached_cur = target_thread != cur_thread
                    && AttachThreadInput(cur_thread, target_thread, 1) != 0;
                let attached_fg = fg_thread != 0
                    && fg_thread != target_thread
                    && fg_thread != cur_thread
                    && AttachThreadInput(fg_thread, target_thread, 1) != 0;

                if IsIconic(hwnd) != 0 {
                    ShowWindow(hwnd, SW_RESTORE);
                } else {
                    ShowWindow(hwnd, SW_SHOW);
                }
                BringWindowToTop(hwnd);
                let mut set = SetForegroundWindow(hwnd) != 0;
                if alt_tap && (!set || GetForegroundWindow() != hwnd) {
                    // Foreground lock: Windows grants SetForegroundWindow to
                    // the process that received the last input event. A
                    // synthetic Alt tap makes this process that one; Alt alone
                    // opens no menu once focus moves.
                    let _ = send(&[vkey_event(VK_MENU, false), vkey_event(VK_MENU, true)]);
                    set = SetForegroundWindow(hwnd) != 0;
                }

                if attached_fg {
                    AttachThreadInput(fg_thread, target_thread, 0);
                }
                if attached_cur {
                    AttachThreadInput(cur_thread, target_thread, 0);
                }
                set
            };
            if !ok {
                // The attach dance usually still lands focus even when the
                // return is 0; the foreground check below decides.
                eprintln!("control: SetForegroundWindow returned 0 (verifying foreground)");
            }
            // Let the target actually receive focus before keys are injected —
            // SendInput otherwise races the foreground switch and keys are dropped
            // or land in the previous window.
            std::thread::sleep(std::time::Duration::from_millis(40));
            // CP-03: SendInput goes to whatever window is in front. Inject only
            // when the verified Crew window really is the foreground window.
            if unsafe { GetForegroundWindow() } != hwnd {
                self.hwnd = None;
                return Err("control: Kiro Crew window did not take focus (nothing injected)".into());
            }
            self.hwnd = Some(hwnd);
            Ok(())
        }

        pub fn send_text(&mut self, text: &str) -> Result<(), String> {
            self.focus()?;
            type_text_raw(text)
        }

        pub fn command_bar(&mut self, cmd: &str) -> Result<(), String> {
            self.focus()?;
            ctrl_k()?;
            type_text_raw(cmd)?;
            tap(VK_RETURN)
        }

        /// Focus the composer (Alt+Enter), then type — no Enter (K5 submits).
        pub fn compose(&mut self, text: &str) -> Result<(), String> {
            // Focus the window only, then type. This previously injected Alt+Enter
            // to "focus the composer", but that chord SUBMITS in Kiro Crew — so it
            // auto-sent whatever was already in the composer. Rely on the composer
            // being the default focused control; typing never submits, and the user
            // still submits explicitly with K5 (Enter).
            self.focus()?;
            type_text_raw(text)
        }

        pub fn chord(&mut self, keys: Chord) -> Result<(), String> {
            self.focus()?;
            if matches!(keys, Chord::SwitchInstance(_)) {
                // FR-05: the instance switch re-renders the whole dashboard;
                // give the freshly focused window time to take keyboard input
                // so Ctrl+N is not dropped.
                std::thread::sleep(std::time::Duration::from_millis(INSTANCE_SWITCH_SETTLE_MS));
                if unsafe { GetForegroundWindow() } != self.hwnd.unwrap_or(0) {
                    return Err("control: Kiro Crew window lost focus (nothing injected)".into());
                }
            }
            inject_chord(keys)
        }
    }

    // Silence dead-code warnings for the Win32 bindings kept for the window
    // enumeration path / future close-menu use but not otherwise referenced;
    // touched here so the linker keeps them honest.
    #[allow(dead_code)]
    fn _keep_win32_linked() -> (
        unsafe extern "system" fn(HWND, u32, WPARAM, LPARAM) -> isize,
        unsafe extern "system" fn(HWND, *mut u16, i32) -> i32,
        unsafe extern "system" fn(HWND) -> i32,
    ) {
        (SendMessageW, GetWindowTextW, GetWindowTextLengthW)
    }
}

// ============================ Non-Windows ============================

#[cfg(not(windows))]
mod stub_impl {
    use super::Chord;

    /// Non-Windows stub: compiles and unit-tests on Linux. `focus` is a no-op
    /// Ok (so router logic can be exercised end to end without a real window);
    /// the injecting methods return `Err` — there is no window system to drive.
    ///
    /// macOS (feat/macos-companion branch) implements this same interface:
    /// `focus` activates the Crew app (`NSRunningApplication.activate`) and
    /// verifies it is frontmost; `chord(SwitchInstance(n))` waits
    /// `INSTANCE_SWITCH_SETTLE_MS` after focus before injecting Cmd+N.
    pub struct StubController {
        #[allow(dead_code)]
        title_match: String,
    }

    impl StubController {
        pub fn new(title_match: Option<String>) -> Self {
            Self { title_match: title_match.unwrap_or_default() }
        }

        pub fn set_title_match(&mut self, title: String) {
            self.title_match = title;
        }

        pub fn focus(&mut self) -> Result<(), String> {
            Ok(())
        }

        pub fn bring_to_front(&mut self) -> Result<(), String> {
            Ok(())
        }

        pub fn send_text(&mut self, _text: &str) -> Result<(), String> {
            Err("control: windows only".to_string())
        }

        pub fn command_bar(&mut self, _cmd: &str) -> Result<(), String> {
            Err("control: windows only".to_string())
        }

        pub fn compose(&mut self, _text: &str) -> Result<(), String> {
            Err("control: windows only".to_string())
        }

        pub fn chord(&mut self, _keys: Chord) -> Result<(), String> {
            Err("control: windows only".to_string())
        }
    }
}

// ─────────────── Public controller facade (platform-neutral) ───────────────

#[cfg(windows)]
type Backend = windows_impl::WinController;
#[cfg(not(windows))]
type Backend = stub_impl::StubController;

/// The FP-4b controller: focus a local Kiro Crew window and inject text / the
/// command bar / chords. Windows-backed for real; a compiling no-op/Err stub
/// elsewhere. Construct with [`KiroCrewController::new`].
pub struct KiroCrewController {
    backend: Backend,
}

impl KiroCrewController {
    /// `title_match` — a window-title substring; `None`/empty uses
    /// [`DEFAULT_TITLE_CANDIDATES`] ("Kiro Crew" / "Agentic").
    pub fn new(title_match: Option<String>) -> Self {
        Self { backend: Backend::new(title_match) }
    }

    /// Re-configure the title substring to match (clears any cached handle).
    pub fn set_title_match(&mut self, title: String) {
        self.backend.set_title_match(title);
    }

    /// Resolve + foreground the window. Verifies a match before injecting
    /// (AC1.5); on non-Windows this is a no-op `Ok`.
    pub fn focus(&mut self) -> Result<(), String> {
        self.backend.focus()
    }

    /// Bring the verified Crew window to the front and inject nothing, not
    /// even the foreground-lock Alt tap (K1-K3 chat select).
    pub fn bring_to_front(&mut self) -> Result<(), String> {
        self.backend.bring_to_front()
    }

    /// Focus, then type `text` (no command bar, no Enter).
    pub fn send_text(&mut self, text: &str) -> Result<(), String> {
        self.backend.send_text(text)
    }

    /// Focus, open the command bar (Ctrl+K), type `cmd`, press Enter.
    pub fn command_bar(&mut self, cmd: &str) -> Result<(), String> {
        self.backend.command_bar(cmd)
    }

    /// Focus the composer (Alt+Enter), then type `text` (no Enter).
    pub fn compose(&mut self, text: &str) -> Result<(), String> {
        self.backend.compose(text)
    }

    /// Focus, then inject a single named [`Chord`].
    pub fn chord(&mut self, keys: Chord) -> Result<(), String> {
        self.backend.chord(keys)
    }

    /// Execute a routed [`ControlAction`] against the window. `None` is a no-op.
    pub fn apply(&mut self, action: &ControlAction) -> Result<(), String> {
        match action {
            ControlAction::CommandBar(cmd) => self.command_bar(cmd),
            ControlAction::Chord(c) => self.chord(*c),
            ControlAction::ChordRepeat(c, n) => {
                for _ in 0..(*n).max(1) {
                    self.chord(*c)?;
                }
                Ok(())
            }
            ControlAction::SendText(t) => self.send_text(t),
            ControlAction::Compose(t) => self.compose(t),
            ControlAction::None => Ok(()),
        }
    }
}

// ──────────────────────────── Tests (Linux) ────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::{
        EncoderPayload, HelloAckPayload, KeyAction, KeyPayload, MacroPayload, VoicePayload,
        VoiceState,
    };

    fn key(id: &str, action: KeyAction) -> Inbound {
        Inbound::Key {
            seq: Some(1),
            payload: KeyPayload { id: id.into(), action, held_ms: 0, wait_ms: 0, bind: None, text: None, slot: None },
        }
    }

    fn bound(id: &str, gesture: KeyAction, bind: KeyAction) -> Inbound {
        Inbound::Key {
            seq: Some(1),
            payload: KeyPayload { id: id.into(), action: gesture, held_ms: 0, wait_ms: 0, bind: Some(bind), text: None, slot: None },
        }
    }

    fn key_held(id: &str, action: KeyAction, held_ms: u32) -> Inbound {
        Inbound::Key {
            seq: Some(1),
            payload: KeyPayload { id: id.into(), action, held_ms, wait_ms: 0, bind: None, text: None, slot: None },
        }
    }

    #[test]
    fn scene_shortcuts_route_to_crew_chords() {
        assert_eq!(SHORTCUT_CHORDS.len(), crate::scene::SHORTCUTS.len());
        let r = ControlRouter::default();
        for (i, name) in crate::scene::SHORTCUTS.iter().enumerate() {
            let f = crate::protocol::decode_line(&format!(
                r#"{{"v":1,"seq":2,"type":"key","payload":{{"id":"k2","action":"double","bind":"{name}","held_ms":0}}}}"#
            ))
            .unwrap();
            assert_eq!(r.route(&f), ControlAction::Chord(SHORTCUT_CHORDS[i]), "{name}");
        }
        assert_eq!(r.route(&bound("k1", KeyAction::Single, KeyAction::Shortcut(9))), ControlAction::Chord(Chord::ClearInput));
        assert_eq!(r.route(&bound("k1", KeyAction::Single, KeyAction::Shortcut(15))), ControlAction::None);
        assert_eq!(r.route(&bound("k1", KeyAction::Single, KeyAction::Shortcut(0))), ControlAction::None);
        assert_eq!(r.route(&bound("enc", KeyAction::Double, KeyAction::SceneMenu)), ControlAction::None);
    }

    #[test]
    fn scene_knob_modes() {
        let mut r = ControlRouter::default();
        assert_eq!(r.route(&encoder(2)), ControlAction::ChordRepeat(Chord::Down, 2));
        r.ctx.knob = KnobMode::parse("chats");
        assert_eq!(r.route(&encoder(2)), ControlAction::ChordRepeat(Chord::NextChat, 2));
        assert_eq!(r.route(&encoder(-1)), ControlAction::ChordRepeat(Chord::PrevChat, 1));
        r.ctx.knob = KnobMode::parse("none");
        assert_eq!(r.route(&encoder(3)), ControlAction::None);
        assert_eq!(KnobMode::parse("spin"), KnobMode::Arrows);
    }

    fn encoder(detents: i32) -> Inbound {
        Inbound::Encoder {
            seq: Some(1),
            payload: EncoderPayload { detents },
        }
    }

    fn mac(name: &str) -> Inbound {
        Inbound::Macro {
            seq: None,
            payload: MacroPayload { name: name.into() },
        }
    }

    // ── Chord parsing ──────────────────────────────────────────

    #[test]
    fn chord_parse_is_case_insensitive_and_rejects_unknown() {
        assert_eq!(Chord::parse("Enter"), Some(Chord::Enter));
        assert_eq!(Chord::parse("RETURN"), Some(Chord::Enter));
        assert_eq!(Chord::parse("esc"), Some(Chord::Esc));
        assert_eq!(Chord::parse("escape"), Some(Chord::Esc));
        assert_eq!(Chord::parse(" up "), Some(Chord::Up));
        assert_eq!(Chord::parse("backspace"), Some(Chord::Backspace));
        assert_eq!(Chord::parse("delete-word"), Some(Chord::DeleteWord));
        assert_eq!(Chord::parse("clear_input"), Some(Chord::ClearInput));
        assert_eq!(Chord::parse("ctrl+k"), Some(Chord::CtrlK));
        assert_eq!(Chord::parse("command_bar"), Some(Chord::CtrlK));
        assert_eq!(Chord::parse("prevchat"), Some(Chord::PrevChat));
        assert_eq!(Chord::parse("next-chat"), Some(Chord::NextChat));
        assert_eq!(Chord::parse("focus_input"), Some(Chord::FocusInput));
        assert_eq!(Chord::parse("newchat"), Some(Chord::NewChat));
        assert_eq!(Chord::parse("chat3"), Some(Chord::JumpChat(3)));
        assert_eq!(Chord::parse("chat9"), Some(Chord::JumpChat(9)));
        assert_eq!(Chord::parse("chat0"), None);
        assert_eq!(Chord::parse("instance1"), Some(Chord::SwitchInstance(1)));
        assert_eq!(Chord::parse("INSTANCE3"), Some(Chord::SwitchInstance(3)));
        assert_eq!(Chord::parse("instance0"), None);
        assert_eq!(Chord::parse("instance10"), None);
        assert_eq!(Chord::parse("nope"), None);
        assert_eq!(Chord::parse(""), None);
    }

    // ── Router: K4 recognized text → compose into the chat input ───

    #[test]
    fn device_text_never_carries_enter_or_control_keys() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(
            route_input(&mac("rm -rf /\r\nyes\n"), &ctx),
            ControlAction::Compose("rm -rf /  yes".into())
        );
        assert_eq!(
            sanitize_device_text("a\u{1b}[2Jb\u{7f}\tc\u{0}"),
            Some("a[2Jb c".into())
        );
        assert_eq!(sanitize_device_text("ok\u{202E}gnp.exe"), Some("okgnp.exe".into()));
        assert_eq!(sanitize_device_text("\r\n\u{1b}"), None);
        assert_eq!(sanitize_device_text("你好，Kiro"), Some("你好，Kiro".into()));
    }

    #[test]
    fn device_text_over_limit_is_refused_not_truncated() {
        let ctx = ControlCtx::with_default_jumps();
        let max = "字".repeat(MAX_DEVICE_TEXT_CHARS);
        assert_eq!(route_input(&mac(&max), &ctx), ControlAction::Compose(max.clone()));
        let over = format!("{max}x");
        assert_eq!(route_input(&mac(&over), &ctx), ControlAction::None);
    }

    #[test]
    fn enter_only_comes_from_an_explicit_confirm_key() {
        let ctx = ControlCtx::with_default_jumps();
        let enter = ControlAction::Chord(Chord::Enter);
        assert_eq!(route_input(&bound("k6", KeyAction::Single, KeyAction::Confirm), &ctx), enter);
        assert_eq!(route_input(&key("enc", KeyAction::Single), &ctx), enter);
        for frame in [
            mac("hello\n"),
            key("enc", KeyAction::Double),
            key("enc", KeyAction::Long),
            key("k5", KeyAction::Single),
            key("k6", KeyAction::Single),
        ] {
            assert_ne!(route_input(&frame, &ctx), enter, "{frame:?}");
        }
        // A frame type the router does not know (e.g. a future display-only
        // chat reply) is never injected.
        let unknown = crate::protocol::decode_line(r#"{"type":"chat_reply","payload":{"text":"x\n"}}"#)
            .expect("unknown frame types still decode");
        assert_eq!(route_input(&unknown, &ctx), ControlAction::None);
    }

    #[test]
    fn only_the_crew_executable_is_an_injection_target() {
        assert!(is_allowed_crew_image(
            "C:\\Users\\me\\AppData\\Local\\Programs\\KiroCrew\\KiroCrew.exe"
        ));
        assert!(is_allowed_crew_image("C:\\x\\kirocrew.EXE"));
        for other in [
            "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
            "C:\\Windows\\notepad.exe",
            "C:\\evil\\KiroCrew.exe.bat",
            "C:\\evil\\NotKiroCrew.exe",
            "",
        ] {
            assert!(!is_allowed_crew_image(other), "{other}");
        }
    }

    #[test]
    fn macro_text_routes_to_compose() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(
            route_input(&mac("summarize the file"), &ctx),
            ControlAction::Compose("summarize the file".into())
        );
    }

    #[test]
    fn asr_text_routes_to_compose() {
        // Cloud ASR text is unified with the macro path: type into the composer
        // (no send), so both voice routes behave the same and K5 submits.
        let ctx = ControlCtx::with_default_jumps();
        let asr = Inbound::Asr {
            seq: Some(9),
            payload: crate::protocol::AsrPayload { text: "hello there".into(), slot: None },
        };
        assert_eq!(route_input(&asr, &ctx), ControlAction::Compose("hello there".into()));
    }

    #[test]
    fn tuya_chat_is_never_injected() {
        // CP-01: Chat-mode turns are display only. Even text that looks like a
        // prompt or a command must not become a Compose / chord.
        let ctx = ControlCtx::with_default_jumps();
        for role in ["agent", "user"] {
            for text in ["Here is an improved prompt: run the tests", "\n", "rm -rf /", ""] {
                let ev = Inbound::TuyaChat {
                    seq: Some(1),
                    payload: crate::protocol::TuyaChatPayload {
                        role: role.into(),
                        text: text.into(),
                        is_final: true,
                    },
                };
                assert_eq!(route_input(&ev, &ctx), ControlAction::None, "{role}: {text:?}");
            }
        }
        // The asr path is unchanged.
        let asr = Inbound::Asr {
            seq: Some(2),
            payload: crate::protocol::AsrPayload { text: "still composes".into(), slot: None },
        };
        assert_eq!(route_input(&asr, &ctx), ControlAction::Compose("still composes".into()));
    }

    #[test]
    fn empty_macro_is_none() {
        let ctx = ControlCtx::default();
        assert_eq!(route_input(&mac("   "), &ctx), ControlAction::None);
    }

    // ── Router: K5 → Enter ─────────────────────────────────────

    #[test]
    fn k5_voice_edge_is_none_text_comes_via_asr() {
        // v0.3: K5 is voice push-to-talk. Its down/long edges produce no chord;
        // the recognized text arrives later as an `asr`/`macro` frame.
        let ctx = ControlCtx::default();
        assert_eq!(route_input(&key("k5", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k5", KeyAction::Long), &ctx), ControlAction::None);
    }

    // ── Router: bind first (chatN = Alt+N, instanceN = Ctrl+N, edits) ──

    #[test]
    fn chat_action_frame_jumps_regardless_of_key() {
        // The resolved target N travels in `bind`; any key/gesture bound to
        // chatN jumps to chat N.
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&bound("k1", KeyAction::Single, KeyAction::Chat(5)), &ctx),
            ControlAction::Chord(Chord::JumpChat(5))
        );
        // Works for a non-k1-k3 key too (stage 乙: bind chat anywhere).
        assert_eq!(
            route_input(&bound("k6", KeyAction::Long, KeyAction::Chat(2)), &ctx),
            ControlAction::Chord(Chord::JumpChat(2))
        );
    }

    #[test]
    fn k1_k3_single_falls_back_to_alt_1_2_3() {
        // Crew API/WS unavailable: the router is the fallback. The firmware's
        // default keymap binds K1-K3 single to chat1..chat3 (user-editable), so
        // the fallback injects Alt+1/2/3.
        let ctx = ControlCtx::default();
        for (id, n) in [("k1", 1u8), ("k2", 2), ("k3", 3)] {
            assert_eq!(
                route_input(&bound(id, KeyAction::Single, KeyAction::Chat(n)), &ctx),
                ControlAction::Chord(Chord::JumpChat(n)),
                "{id}"
            );
        }
    }

    #[test]
    fn instance_action_frame_switches_regardless_of_key() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&bound("k1", KeyAction::Double, KeyAction::Instance(1)), &ctx),
            ControlAction::Chord(Chord::SwitchInstance(1))
        );
        assert_eq!(
            route_input(&bound("k4", KeyAction::Single, KeyAction::Instance(3)), &ctx),
            ControlAction::Chord(Chord::SwitchInstance(3))
        );
    }

    #[test]
    fn k1_k3_raw_edges_are_none() {
        let ctx = ControlCtx::with_default_jumps();
        // Raw edges are no-ops at the router (only the voice key sends them).
        for id in ["k1", "k2", "k3"] {
            assert_eq!(
                route_input(&key(id, KeyAction::Down), &ctx),
                ControlAction::None,
                "{id} Down should be None"
            );
            assert_eq!(
                route_input(&key(id, KeyAction::Long), &ctx),
                ControlAction::None,
                "{id} Long should be None"
            );
            assert_eq!(
                route_input(&key_held(id, KeyAction::Up, 120), &ctx),
                ControlAction::None,
                "{id} Up should be None"
            );
        }
    }

    #[test]
    fn timing_line_sums_the_stages() {
        use std::time::Duration;
        let line = timing_line(
            "k1",
            "single",
            "api",
            0,
            None,
            &[("select", Duration::from_micros(800)), ("front", Duration::from_millis(43))],
        );
        assert_eq!(line, "[KEY-TIMING] k1 single path=api window=0ms select=0ms front=43ms total=43ms");
    }

    #[test]
    fn timing_line_reports_window_and_long_hold() {
        use std::time::Duration;
        let single = timing_line("k2", "single", "api", 251, None, &[("select", Duration::from_millis(1))]);
        assert_eq!(single, "[KEY-TIMING] k2 single path=api window=251ms select=1ms total=1ms");
        let long = timing_line("k3", "long", "keys", 0, Some(503), &[("focus+keys", Duration::from_millis(190))]);
        assert_eq!(long, "[KEY-TIMING] k3 long path=keys window=0ms hold=503ms focus+keys=190ms total=190ms");
    }

    #[test]
    fn unbound_gestures_are_none_at_the_router() {
        // Unbound K1-K3 single is the app layer's Crew API slot select; an
        // unbound double or long does nothing.
        let ctx = ControlCtx::default();
        for g in [KeyAction::Single, KeyAction::Double, KeyAction::Long] {
            assert_eq!(route_input(&key("k1", g), &ctx), ControlAction::None, "{g:?}");
        }
        // A local-only bind (the board opened its settings page) is a no-op.
        assert_eq!(
            route_input(&bound("enc", KeyAction::Long, KeyAction::Unknown), &ctx),
            ControlAction::None
        );
    }

    #[test]
    fn pending_confirm_edits_map_to_composer_chords() {
        // v0.3 pending-confirm: the gesture carries the edit as its bind.
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(
            route_input(&bound("k4", KeyAction::Single, KeyAction::CancelChar), &ctx),
            ControlAction::Chord(Chord::Backspace)
        );
        assert_eq!(
            route_input(&bound("k4", KeyAction::Long, KeyAction::CancelAll), &ctx),
            ControlAction::Chord(Chord::ClearInput)
        );
        assert_eq!(
            route_input(&bound("k6", KeyAction::Single, KeyAction::Confirm), &ctx),
            ControlAction::Chord(Chord::Enter)
        );
    }

    #[test]
    fn k4_k6_raw_edges_are_none_outside_pending() {
        // Outside pending-confirm the firmware does not emit actionable edges
        // for K4/K6; the router treats their raw down/long as no-ops.
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(route_input(&key("k4", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k6", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k6", KeyAction::Long), &ctx), ControlAction::None);
    }

    #[test]
    fn encoder_clockwise_scrolls_down() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&encoder(1), &ctx),
            ControlAction::ChordRepeat(Chord::Down, 1)
        );
        // Multi-detent clockwise repeats Down by the detent count.
        assert_eq!(
            route_input(&encoder(3), &ctx),
            ControlAction::ChordRepeat(Chord::Down, 3)
        );
    }

    #[test]
    fn encoder_counter_clockwise_scrolls_up() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&encoder(-1), &ctx),
            ControlAction::ChordRepeat(Chord::Up, 1)
        );
        assert_eq!(
            route_input(&encoder(-2), &ctx),
            ControlAction::ChordRepeat(Chord::Up, 2)
        );
    }

    #[test]
    fn encoder_zero_detents_is_none() {
        let ctx = ControlCtx::default();
        assert_eq!(route_input(&encoder(0), &ctx), ControlAction::None);
    }

    #[test]
    fn enc_push_single_confirms_with_enter() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&key("enc", KeyAction::Single), &ctx),
            ControlAction::Chord(Chord::Enter)
        );
        assert_eq!(route_input(&key("enc", KeyAction::Double), &ctx), ControlAction::None);
    }

    // ── Router: non-triggering / ignored events → None ─────────

    #[test]
    fn key_up_edges_do_not_trigger() {
        let ctx = ControlCtx::with_default_jumps();
        // K5 is not a jump key; its Up edge is a no-op.
        assert_eq!(route_input(&key("k5", KeyAction::Up), &ctx), ControlAction::None);
        // (K1-K3 Up edges are covered above — all None now.)
    }

    #[test]
    fn enc_push_up_edge_does_not_trigger() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&key("enc", KeyAction::Up), &ctx),
            ControlAction::None
        );
    }

    #[test]
    fn unknown_key_and_housekeeping_frames_are_none() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(route_input(&key("k9", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&Inbound::Ping, &ctx), ControlAction::None);
        assert_eq!(route_input(&Inbound::Pong, &ctx), ControlAction::None);
        assert_eq!(
            route_input(
                &Inbound::Voice { seq: None, payload: VoicePayload { state: VoiceState::Start, slot: None } },
                &ctx
            ),
            ControlAction::None
        );
        assert_eq!(
            route_input(&Inbound::HelloAck(HelloAckPayload::default()), &ctx),
            ControlAction::None
        );
    }

    // ── ControlRouter wrapper ──────────────────────────────────

    #[test]
    fn control_router_wraps_route_input() {
        let router = ControlRouter::new(ControlCtx::with_default_jumps());
        // EC11 push confirms with Enter.
        assert_eq!(
            router.route(&key("enc", KeyAction::Single)),
            ControlAction::Chord(Chord::Enter)
        );
        // A resolved chat action frame jumps to its target N.
        assert_eq!(
            router.route(&bound("k1", KeyAction::Single, KeyAction::Chat(1))),
            ControlAction::Chord(Chord::JumpChat(1))
        );
        // A resolved instance action frame switches Crew instance.
        assert_eq!(
            router.route(&bound("k1", KeyAction::Double, KeyAction::Instance(1))),
            ControlAction::Chord(Chord::SwitchInstance(1))
        );
        assert_eq!(router.route(&key("k1", KeyAction::Down)), ControlAction::None);
    }

    // ── Stub controller behaviour on non-Windows ───────────────

    #[cfg(not(windows))]
    #[test]
    fn stub_controller_focus_ok_inject_err() {
        let mut c = KiroCrewController::new(None);
        assert!(c.focus().is_ok());
        assert!(c.send_text("x").is_err());
        assert!(c.command_bar("cmd").is_err());
        assert!(c.chord(Chord::Enter).is_err());
        // apply() delegates: no-op is safe; injection is Windows-only.
        assert!(c.apply(&ControlAction::None).is_ok());
        assert!(c.apply(&ControlAction::Chord(Chord::Up)).is_err());
    }
}
