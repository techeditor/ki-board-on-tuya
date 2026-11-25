//! Connection preferences (`connection.json`) and the auto-reconnect policy.
//!
//! Everything here is pure except `load` / `save`, so the policy is unit tested
//! without a serial port or a BLE adapter. The supervisor thread in `lib.rs`
//! feeds it the current link state and the ports it found.

use crate::app_dirs;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::time::{Duration, Instant};

const FILE: &str = "connection.json";

/// What the user chose for the link. Persisted across restarts.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(default)]
pub struct Prefs {
    /// "auto" (USB first, then the paired BLE device), "usb" or "ble".
    pub transport: String,
    /// Serial port picked by the user. Empty = the first CH34x board port.
    pub port: String,
    /// BLE device id picked by the user or last connected over BLE. Empty = none.
    pub ble_device: String,
    /// Reconnect automatically after start-up and after a link drop.
    pub auto_connect: bool,
}

impl Default for Prefs {
    fn default() -> Self {
        Self {
            transport: "auto".into(),
            port: String::new(),
            ble_device: String::new(),
            auto_connect: true,
        }
    }
}

impl Prefs {
    /// Unknown transport names fall back to "auto", so a hand-edited file
    /// cannot leave the policy in a state it does not handle.
    pub fn normalized(mut self) -> Self {
        if !matches!(self.transport.as_str(), "auto" | "usb" | "ble") {
            self.transport = "auto".into();
        }
        self
    }
}

fn path() -> PathBuf {
    app_dirs::companion_file(FILE)
}

/// Read the saved preferences. A missing or unreadable file gives the defaults.
pub fn load() -> Prefs {
    std::fs::read_to_string(path())
        .ok()
        .and_then(|text| serde_json::from_str::<Prefs>(&text).ok())
        .unwrap_or_default()
        .normalized()
}

pub fn save(prefs: &Prefs) -> Result<(), String> {
    let path = path();
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|err| format!("create connection dir failed: {err}"))?;
    }
    let text = serde_json::to_string_pretty(&prefs.clone().normalized())
        .map_err(|err| format!("encode connection prefs failed: {err}"))?;
    std::fs::write(&path, text).map_err(|err| format!("save connection prefs failed: {err}"))
}

/// A serial port seen during a scan.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Port {
    pub name: String,
    /// True for the CH34x bridge the board enumerates as.
    pub board: bool,
}

/// The device the supervisor should connect to now.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Target {
    /// `None` = auto-detect the board port.
    Usb(Option<String>),
    /// BLE peripheral id.
    Ble(String),
}

/// Pick the device for the preferences. A port or device the user named must
/// be present; auto mode prefers USB and falls back to the paired BLE device.
pub fn choose_target(prefs: &Prefs, ports: &[Port], ble_paired: bool) -> Option<Target> {
    let usb = || -> Option<Target> {
        if !prefs.port.is_empty() {
            return ports
                .iter()
                .any(|port| port.name == prefs.port)
                .then(|| Target::Usb(Some(prefs.port.clone())));
        }
        ports.iter().any(|port| port.board).then_some(Target::Usb(None))
    };
    let ble = || {
        (ble_paired && !prefs.ble_device.is_empty()).then(|| Target::Ble(prefs.ble_device.clone()))
    };
    match prefs.transport.as_str() {
        "usb" => usb(),
        "ble" => ble(),
        _ => usb().or_else(ble),
    }
}

/// Wait before retry number `attempt` (0-based): 2, 4, 8, 16, then 30 s.
/// The cap keeps a missing device from being scanned more than twice a minute.
pub fn backoff(attempt: u32) -> Duration {
    const STEPS_S: [u64; 5] = [2, 4, 8, 16, 30];
    Duration::from_secs(STEPS_S[(attempt as usize).min(STEPS_S.len() - 1)])
}

#[derive(Debug, PartialEq, Eq)]
pub enum Step {
    Wait,
    Attempt(Target),
}

/// Backoff state for the reconnect supervisor. Reset whenever the link is up
/// or auto-connect is off, so the next drop retries immediately.
#[derive(Default, Debug)]
pub struct Reconnect {
    attempt: u32,
    next_at: Option<Instant>,
}

impl Reconnect {
    /// True when a retry is allowed now. The supervisor checks this before it
    /// enumerates ports, so a missing device costs one scan per backoff step.
    pub fn due(&self, now: Instant) -> bool {
        self.next_at.map_or(true, |at| now >= at)
    }

    /// One supervisor tick. `enabled` is false when auto-connect is off or the
    /// user disconnected by hand. Call `failed` when an `Attempt` does not connect.
    pub fn step(
        &mut self,
        now: Instant,
        connected: bool,
        enabled: bool,
        prefs: &Prefs,
        ports: &[Port],
        ble_paired: bool,
    ) -> Step {
        if connected || !enabled {
            *self = Self::default();
            return Step::Wait;
        }
        if !self.due(now) {
            return Step::Wait;
        }
        match choose_target(prefs, ports, ble_paired) {
            Some(target) => Step::Attempt(target),
            None => {
                self.failed(now);
                Step::Wait
            }
        }
    }

    /// Record a failed attempt and schedule the next one.
    pub fn failed(&mut self, now: Instant) {
        self.next_at = Some(now + backoff(self.attempt));
        self.attempt = self.attempt.saturating_add(1);
    }
}

/// Auto-connect switch and the manual-disconnect latch, kept in app state.
#[derive(Debug)]
pub struct LinkState {
    pub prefs: Prefs,
    /// Set by the user's Disconnect. Cleared by a user Connect or a restart.
    pub manual_disconnect: bool,
}

impl LinkState {
    pub fn load() -> Self {
        Self {
            prefs: load(),
            manual_disconnect: false,
        }
    }

    /// Whether the supervisor may reconnect on its own right now.
    pub fn auto_enabled(&self) -> bool {
        self.prefs.auto_connect && !self.manual_disconnect
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn port(name: &str, board: bool) -> Port {
        Port {
            name: name.into(),
            board,
        }
    }

    fn prefs(transport: &str, port: &str, ble: &str) -> Prefs {
        Prefs {
            transport: transport.into(),
            port: port.into(),
            ble_device: ble.into(),
            auto_connect: true,
        }
    }

    #[test]
    fn defaults_are_auto_with_autoconnect() {
        let p = Prefs::default();
        assert_eq!(p.transport, "auto");
        assert!(p.auto_connect);
        assert!(p.port.is_empty() && p.ble_device.is_empty());
    }

    #[test]
    fn unknown_transport_falls_back_to_auto() {
        assert_eq!(prefs("serial", "", "").normalized().transport, "auto");
    }

    #[test]
    fn auto_prefers_board_usb_over_paired_ble() {
        let ports = [port("COM3", false), port("COM6", true)];
        assert_eq!(choose_target(&prefs("auto", "", "dev1"), &ports, true), Some(Target::Usb(None)));
    }

    #[test]
    fn auto_falls_back_to_paired_ble_when_no_board_port() {
        let ports = [port("COM3", false)];
        assert_eq!(
            choose_target(&prefs("auto", "", "dev1"), &ports, true),
            Some(Target::Ble("dev1".into()))
        );
    }

    #[test]
    fn auto_without_board_or_pairing_has_no_target() {
        assert_eq!(choose_target(&prefs("auto", "", "dev1"), &[], false), None);
        assert_eq!(choose_target(&prefs("auto", "", ""), &[port("COM6", true)], false), Some(Target::Usb(None)));
    }

    #[test]
    fn named_port_must_be_present() {
        let ports = [port("COM6", true), port("COM9", false)];
        assert_eq!(
            choose_target(&prefs("usb", "COM9", ""), &ports, false),
            Some(Target::Usb(Some("COM9".into())))
        );
        // Missing named port: no fallback to another board port.
        assert_eq!(choose_target(&prefs("usb", "COM4", ""), &ports, false), None);
    }

    #[test]
    fn manual_usb_ignores_ble() {
        assert_eq!(choose_target(&prefs("usb", "", "dev1"), &[], true), None);
    }

    #[test]
    fn manual_ble_needs_pairing_and_device() {
        assert_eq!(choose_target(&prefs("ble", "", "dev1"), &[port("COM6", true)], true), Some(Target::Ble("dev1".into())));
        assert_eq!(choose_target(&prefs("ble", "", "dev1"), &[], false), None);
        assert_eq!(choose_target(&prefs("ble", "", ""), &[], true), None);
    }

    #[test]
    fn backoff_steps_then_caps_at_thirty_seconds() {
        let secs: Vec<u64> = (0..7).map(|n| backoff(n).as_secs()).collect();
        assert_eq!(secs, vec![2, 4, 8, 16, 30, 30, 30]);
    }

    #[test]
    fn missing_device_backs_off_instead_of_retrying_every_tick() {
        let mut policy = Reconnect::default();
        let p = prefs("usb", "", "");
        let t0 = Instant::now();
        assert_eq!(policy.step(t0, false, true, &p, &[], false), Step::Wait);
        // Still inside the first 2 s backoff: not even due, so no port scan is needed.
        assert!(!policy.due(t0 + Duration::from_secs(1)));
        assert!(policy.due(t0 + Duration::from_secs(2)));
    }

    #[test]
    fn attempt_is_returned_when_target_present() {
        let mut policy = Reconnect::default();
        let p = prefs("auto", "", "");
        let ports = [port("COM6", true)];
        assert_eq!(
            policy.step(Instant::now(), false, true, &p, &ports, false),
            Step::Attempt(Target::Usb(None))
        );
    }

    #[test]
    fn disabled_or_connected_never_attempts_and_resets_backoff() {
        let mut policy = Reconnect::default();
        let p = prefs("auto", "", "");
        let ports = [port("COM6", true)];
        let t0 = Instant::now();
        policy.failed(t0);
        assert_eq!(policy.step(t0, false, false, &p, &ports, false), Step::Wait);
        assert!(policy.due(t0), "disabling clears the backoff");
        policy.failed(t0);
        assert_eq!(policy.step(t0 + Duration::from_secs(60), true, true, &p, &ports, false), Step::Wait);
        assert!(policy.due(t0), "a connected link clears the backoff");
    }

    #[test]
    fn enabled_policy_respects_manual_disconnect_latch() {
        let mut link = LinkState {
            prefs: prefs("auto", "", ""),
            manual_disconnect: false,
        };
        assert!(link.auto_enabled());
        link.manual_disconnect = true;
        assert!(!link.auto_enabled());
        link.manual_disconnect = false;
        link.prefs.auto_connect = false;
        assert!(!link.auto_enabled());
    }
}
