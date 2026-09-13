//! FP-4b Route B — the *hands*: drive a **local** Kiro Crew window by focusing
//! it and injecting keystrokes (no HTTP, no session-control API). This is the
//! control plane that supersedes route A for the actual control path.
//!
//! Two layers live here:
//!
//! * [`KiroCrewController`] — Windows-gated `user32` FFI backend that finds the
//!   Kiro Crew window by a configurable title substring, brings it to the
//!   foreground, and injects text / the Ctrl+K command bar / named chords. It
//!   reuses the raw `SendInput` pattern from [`crate::inject`] (no new crate).
//!   On non-Windows hosts a stub compiles and returns `Err` / no-ops so the
//!   Linux CI build and the pure router tests below still work.
//!
//! * [`ControlRouter`] / [`route_input`] — a **pure**, IO-free mapping from a
//!   decoded board [`Inbound`] event to a [`ControlAction`]. Table-driven and
//!   unit-tested on Linux (the tests at the bottom run in `cargo test`).
//!
//! Design ref: `.kiro/specs/feature-probes-kirocrew-local-control/design.md`.

use crate::protocol::{Inbound, KeyAction, NavDir};

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
    /// Ctrl+K — open the Kiro Crew command bar.
    CtrlK,
    /// Alt+N — jump to chat N (N = 1..=9). Deterministic, unlike a command-bar
    /// text search. (Kiro Crew default shortcut "Jump to chat N".)
    JumpChat(u8),
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
                None
            }
        }
    }
}

// ─────────────────────── Pure ControlRouter (T2) ───────────────────────

/// The action the router decided a board event should produce. Consumed by the
/// (Windows) [`KiroCrewController`]; pure and IO-free so it is fully testable.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlAction {
    /// Open the command bar (Ctrl+K), type the string, press Enter.
    CommandBar(String),
    /// Inject a single named chord.
    Chord(Chord),
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
#[derive(Clone, Debug, Default)]
pub struct ControlCtx {
    /// Chat numbers for K1, K2, K3 (index 0..=2). Configurable by the UI.
    pub jump_chats: Vec<u8>,
}

impl ControlCtx {
    /// Sensible defaults: K1->chat 1, K2->chat 2, K3->chat 3 (Alt+1/2/3).
    pub fn with_default_jumps() -> Self {
        Self { jump_chats: vec![1, 2, 3] }
    }

    fn jump(&self, idx: usize) -> ControlAction {
        match self.jump_chats.get(idx) {
            Some(&n) if (1..=9).contains(&n) => ControlAction::Chord(Chord::JumpChat(n)),
            _ => ControlAction::None,
        }
    }
}

/// Is this a *triggering* key edge? We act on `down` / `long`, never on `up`
/// (so a single physical press produces exactly one action).
fn is_trigger(action: KeyAction) -> bool {
    matches!(action, KeyAction::Down | KeyAction::Long)
}

/// Pure mapping: board event + context → control action. No IO.
///
/// Mapping table (design R2.3), using Kiro Crew's real default shortcuts:
///
/// | event                          | action                              |
/// |--------------------------------|-------------------------------------|
/// | `macro { name }` (recognized text) | `Compose(name)` — K4 voice → focus composer + type (no send) |
/// | `key k1` (down/long)           | `Chord(JumpChat(1))` — Alt+1        |
/// | `key k2` (down/long)           | `Chord(JumpChat(2))` — Alt+2        |
/// | `key k3` (down/long)           | `Chord(JumpChat(3))` — Alt+3        |
/// | `key k4` (down/long)           | `None` — K4 is push-to-talk; text arrives later as `macro` |
/// | `key k5` (down/long)           | `Chord(Enter)` — submit / continue  |
/// | `nav left/right`               | `Chord(PrevChat/NextChat)` — Alt+←/→ |
/// | `nav up/down`                  | `Chord(Up/Down)` — arrow keys       |
/// | `nav center` (down/long)       | `Chord(Enter)`                      |
/// | anything else / any `up` edge  | `None`                              |
pub fn route_input(ev: &Inbound, ctx: &ControlCtx) -> ControlAction {
    match ev {
        // K4's recognized dictation → type into the chat composer (no send).
        Inbound::Macro { payload, .. } => {
            let text = payload.name.trim();
            if text.is_empty() {
                ControlAction::None
            } else {
                ControlAction::Compose(text.to_string())
            }
        }

        Inbound::Key { payload, .. } if is_trigger(payload.action) => {
            match payload.id.as_str() {
                "k1" => ctx.jump(0),
                "k2" => ctx.jump(1),
                "k3" => ctx.jump(2),
                // K4 = push-to-talk: pressing it starts voice; the recognized
                // text comes back later as a `macro` frame (handled above).
                "k4" => ControlAction::None,
                // K5 = submit / continue.
                "k5" => ControlAction::Chord(Chord::Enter),
                _ => ControlAction::None,
            }
        }

        Inbound::Nav { payload, .. } if is_trigger(payload.action) => match payload.dir {
            NavDir::Up => ControlAction::Chord(Chord::Up),
            NavDir::Down => ControlAction::Chord(Chord::Down),
            // Joystick left/right = previous/next chat (Alt+←/→) — deterministic.
            NavDir::Left => ControlAction::Chord(Chord::PrevChat),
            NavDir::Right => ControlAction::Chord(Chord::NextChat),
            NavDir::Center => ControlAction::Chord(Chord::Enter),
            NavDir::Unknown => ControlAction::None,
        },

        // Key/nav `up` edges, voice edges, ping/pong/ack, hello_ack, unknown.
        _ => ControlAction::None,
    }
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

// ============================== Windows ==============================

#[cfg(windows)]
mod windows_impl {
    use super::{Chord, DEFAULT_TITLE_CANDIDATES};
    use std::mem::size_of;

    // --- Minimal Win32 bindings (raw FFI, mirrors inject.rs — no `windows` crate). ---
    type HWND = isize;
    type LPARAM = isize;
    type BOOL = i32;
    type WPARAM = usize;

    const INPUT_KEYBOARD: u32 = 1;
    const KEYEVENTF_KEYUP: u32 = 0x0002;
    const KEYEVENTF_UNICODE: u32 = 0x0004;

    // Virtual-key codes we use for chords / the command bar.
    const VK_RETURN: u16 = 0x0D;
    const VK_ESCAPE: u16 = 0x1B;
    const VK_LEFT: u16 = 0x25;
    const VK_UP: u16 = 0x26;
    const VK_RIGHT: u16 = 0x27;
    const VK_DOWN: u16 = 0x28;
    const VK_CONTROL: u16 = 0x11;
    const VK_MENU: u16 = 0x12; // Alt
    const VK_SHIFT: u16 = 0x10;
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
        fn EnumWindows(lp_enum_func: WNDENUMPROC, l_param: LPARAM) -> BOOL;
        fn GetWindowTextW(hwnd: HWND, lp_string: *mut u16, n_max_count: i32) -> i32;
        fn GetWindowTextLengthW(hwnd: HWND) -> i32;
        fn IsWindowVisible(hwnd: HWND) -> BOOL;
        fn SetForegroundWindow(hwnd: HWND) -> BOOL;
        fn ShowWindow(hwnd: HWND, n_cmd_show: i32) -> BOOL;
        fn IsIconic(hwnd: HWND) -> BOOL;
        fn SendMessageW(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> isize;
    }

    const SW_RESTORE: i32 = 9;

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

    fn vkey_event(vk: u16, key_up: bool) -> INPUT {
        INPUT {
            r#type: INPUT_KEYBOARD,
            ki: KEYBDINPUT {
                w_vk: vk,
                w_scan: 0,
                dw_flags: if key_up { KEYEVENTF_KEYUP } else { 0 },
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
            Chord::CtrlK => ctrl_k(),
            Chord::JumpChat(n) if (1..=9).contains(&n) => alt_tap(0x30 + n as u16), // Alt+'1'..'9'
            Chord::JumpChat(n) => Err(format!("JumpChat({n}) out of range 1..=9")),
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
        if title.to_lowercase().contains(&ctx.needle_lower) {
            ctx.found = hwnd;
            return 0; // stop enumerating — first visible match wins
        }
        1
    }

    /// Find the first visible top-level window whose title contains `needle`
    /// (case-insensitive). Returns `None` if none match.
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
                        "control: no window matches title {:?} (nothing injected)",
                        if self.title_match.trim().is_empty() {
                            DEFAULT_TITLE_CANDIDATES.join(" / ")
                        } else {
                            self.title_match.clone()
                        }
                    )
                })?;

            unsafe {
                if IsIconic(hwnd) != 0 {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                if SetForegroundWindow(hwnd) == 0 {
                    // Non-fatal: foreground can be refused by the OS focus rules,
                    // but the window handle is valid. Record and continue.
                    eprintln!("control: SetForegroundWindow refused (window still targeted)");
                }
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
            self.focus()?;
            alt_tap(VK_RETURN)?; // Alt+Enter = focus text input
            type_text_raw(text)
        }

        pub fn chord(&mut self, keys: Chord) -> Result<(), String> {
            self.focus()?;
            inject_chord(keys)
        }
    }

    // Silence dead-code warnings for the SendMessageW binding kept for parity /
    // future close-menu use; referenced here so the linker keeps it honest.
    #[allow(dead_code)]
    fn _keep_sendmessage_linked() -> unsafe extern "system" fn(HWND, u32, WPARAM, LPARAM) -> isize {
        SendMessageW
    }
}

// ============================ Non-Windows ============================

#[cfg(not(windows))]
mod stub_impl {
    use super::Chord;

    /// Non-Windows stub: compiles and unit-tests on Linux. `focus` is a no-op
    /// Ok (so router logic can be exercised end to end without a real window);
    /// the injecting methods return `Err` — there is no window system to drive.
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
        HelloAckPayload, KeyAction, KeyPayload, MacroPayload, NavPayload, VoicePayload, VoiceState,
    };

    fn key(id: &str, action: KeyAction) -> Inbound {
        Inbound::Key {
            seq: Some(1),
            payload: KeyPayload { id: id.into(), action, held_ms: 0 },
        }
    }

    fn nav(dir: NavDir, action: KeyAction) -> Inbound {
        Inbound::Nav {
            seq: Some(1),
            payload: NavPayload { dir, action, held_ms: 0 },
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
        assert_eq!(Chord::parse("ctrl+k"), Some(Chord::CtrlK));
        assert_eq!(Chord::parse("command_bar"), Some(Chord::CtrlK));
        assert_eq!(Chord::parse("prevchat"), Some(Chord::PrevChat));
        assert_eq!(Chord::parse("next-chat"), Some(Chord::NextChat));
        assert_eq!(Chord::parse("focus_input"), Some(Chord::FocusInput));
        assert_eq!(Chord::parse("newchat"), Some(Chord::NewChat));
        assert_eq!(Chord::parse("chat3"), Some(Chord::JumpChat(3)));
        assert_eq!(Chord::parse("chat9"), Some(Chord::JumpChat(9)));
        assert_eq!(Chord::parse("chat0"), None);
        assert_eq!(Chord::parse("nope"), None);
        assert_eq!(Chord::parse(""), None);
    }

    // ── Router: K4 recognized text → compose into the chat input ───

    #[test]
    fn macro_text_routes_to_compose() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(
            route_input(&mac("summarize the file"), &ctx),
            ControlAction::Compose("summarize the file".into())
        );
    }

    #[test]
    fn empty_macro_is_none() {
        let ctx = ControlCtx::default();
        assert_eq!(route_input(&mac("   "), &ctx), ControlAction::None);
    }

    // ── Router: K5 → Enter ─────────────────────────────────────

    #[test]
    fn k5_down_routes_to_enter_chord() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&key("k5", KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::Enter)
        );
        // Long press also triggers.
        assert_eq!(
            route_input(&key("k5", KeyAction::Long), &ctx),
            ControlAction::Chord(Chord::Enter)
        );
    }

    // ── Router: K1–K3 → Alt+N jump-chat chords ─────────────────

    #[test]
    fn k1_k2_k3_route_to_jumpchat_chords() {
        let ctx = ControlCtx::with_default_jumps(); // [1,2,3]
        assert_eq!(
            route_input(&key("k1", KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::JumpChat(1))
        );
        assert_eq!(
            route_input(&key("k2", KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::JumpChat(2))
        );
        assert_eq!(
            route_input(&key("k3", KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::JumpChat(3))
        );
    }

    #[test]
    fn k1_k2_k3_with_empty_config_are_none() {
        let ctx = ControlCtx { jump_chats: vec![] };
        assert_eq!(route_input(&key("k1", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k2", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k3", KeyAction::Down), &ctx), ControlAction::None);
    }

    #[test]
    fn k4_press_is_none_text_comes_via_macro() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(route_input(&key("k4", KeyAction::Down), &ctx), ControlAction::None);
    }

    // ── Router: nav → arrows / prev-next chat / center ─────────

    #[test]
    fn nav_directions_route_to_expected_chords() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&nav(NavDir::Up, KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::Up)
        );
        assert_eq!(
            route_input(&nav(NavDir::Down, KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::Down)
        );
        assert_eq!(
            route_input(&nav(NavDir::Left, KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::PrevChat)
        );
        assert_eq!(
            route_input(&nav(NavDir::Right, KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::NextChat)
        );
    }

    #[test]
    fn nav_center_routes_to_enter() {
        let ctx = ControlCtx::default();
        assert_eq!(
            route_input(&nav(NavDir::Center, KeyAction::Down), &ctx),
            ControlAction::Chord(Chord::Enter)
        );
    }

    // ── Router: non-triggering / ignored events → None ─────────

    #[test]
    fn key_up_edges_do_not_trigger() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(route_input(&key("k5", KeyAction::Up), &ctx), ControlAction::None);
        assert_eq!(route_input(&key("k1", KeyAction::Up), &ctx), ControlAction::None);
    }

    #[test]
    fn nav_up_edge_does_not_trigger() {
        let ctx = ControlCtx::default();
        assert_eq!(route_input(&nav(NavDir::Up, KeyAction::Up), &ctx), ControlAction::None);
    }

    #[test]
    fn unknown_key_and_housekeeping_frames_are_none() {
        let ctx = ControlCtx::with_default_jumps();
        assert_eq!(route_input(&key("k9", KeyAction::Down), &ctx), ControlAction::None);
        assert_eq!(route_input(&Inbound::Ping, &ctx), ControlAction::None);
        assert_eq!(route_input(&Inbound::Pong, &ctx), ControlAction::None);
        assert_eq!(
            route_input(
                &Inbound::Voice { seq: None, payload: VoicePayload { state: VoiceState::Start } },
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
        assert_eq!(
            router.route(&key("k5", KeyAction::Down)),
            ControlAction::Chord(Chord::Enter)
        );
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
        // apply() delegates: None is Ok, others Err on the stub.
        assert!(c.apply(&ControlAction::None).is_ok());
        assert!(c.apply(&ControlAction::Chord(Chord::Up)).is_err());
    }
}
