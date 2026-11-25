//! Watches the `hello` / `hello_ack` handshake of the current connection.
//!
//! The keyboard answers `hello` with `hello_ack`. If the ack never arrives
//! (older firmware, or a frame the firmware dropped, as when `hello_ack`
//! overflowed its 192-byte buffer) the menus and PC voice recognition silently
//! stop working. This tracks whether the ack is overdue so the UI can say so.
//! A keyboard that is still starting (a 4 s custom splash, cloud bring-up)
//! answers late, so the first miss resends `hello` once; only a second miss
//! warns. Pure state, no threads or locks, so it is unit-testable with fake clocks.

use std::time::{Duration, Instant};

/// How long after `hello` (and again after the one resend) the keyboard has to answer.
pub const HELLO_ACK_TIMEOUT: Duration = Duration::from_secs(3);

#[derive(Debug, Default)]
pub struct HandshakeWatch {
    conn: u64,
    sent: Option<Instant>,
    acked: bool,
    resent: bool,
    warned: bool,
}

impl HandshakeWatch {
    /// `hello` is about to be sent on connection `conn`.
    pub fn begin(&mut self, conn: u64, now: Instant) {
        *self = Self { conn, sent: Some(now), acked: false, resent: false, warned: false };
    }

    /// Returns true once, when the first `hello` on `conn` is overdue: the
    /// caller sends `hello` again, and the timeout restarts from `now`.
    pub fn resend(&mut self, conn: u64, now: Instant) -> bool {
        let Some(sent) = self.sent else { return false };
        if self.conn != conn || self.acked || self.resent {
            return false;
        }
        if now.saturating_duration_since(sent) < HELLO_ACK_TIMEOUT {
            return false;
        }
        self.resent = true;
        self.sent = Some(now);
        true
    }

    /// `hello_ack` arrived on `conn`. Returns true when this clears a warning
    /// already shown (the ack was late), so the caller can log it.
    pub fn ack(&mut self, conn: u64) -> bool {
        if self.sent.is_none() || self.conn != conn {
            return false;
        }
        self.acked = true;
        std::mem::take(&mut self.warned)
    }

    /// Returns true once, when `conn` is still unanswered after the resend.
    /// The caller logs it then.
    pub fn poll(&mut self, conn: u64, now: Instant) -> bool {
        let Some(sent) = self.sent else { return false };
        if self.conn != conn || self.acked || self.warned || !self.resent {
            return false;
        }
        if now.saturating_duration_since(sent) < HELLO_ACK_TIMEOUT {
            return false;
        }
        self.warned = true;
        true
    }

    /// The UI warning: shown from the timeout until the ack, for the current
    /// connection only.
    pub fn missing(&self, conn: u64) -> bool {
        self.conn == conn && self.warned
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(base: Instant, ms: u64) -> Instant {
        base + Duration::from_millis(ms)
    }

    #[test]
    fn resends_once_then_warns_once() {
        let t0 = Instant::now();
        let mut w = HandshakeWatch::default();
        w.begin(1, t0);
        assert!(!w.resend(1, at(t0, 2_999)));
        assert!(!w.poll(1, at(t0, 2_999)));
        assert!(!w.poll(1, at(t0, 3_000)), "no warning before the resend");
        assert!(w.resend(1, at(t0, 3_000)));
        assert!(!w.resend(1, at(t0, 9_000)), "hello is resent once");
        assert!(!w.missing(1));
        assert!(!w.poll(1, at(t0, 5_999)), "the timeout restarts at the resend");
        assert!(w.poll(1, at(t0, 6_000)));
        assert!(w.missing(1));
        assert!(!w.poll(1, at(t0, 12_000)), "the log line is written once");
        assert!(w.missing(1));
    }

    #[test]
    fn ack_in_time_never_resends_or_warns() {
        let t0 = Instant::now();
        let mut w = HandshakeWatch::default();
        w.begin(1, t0);
        assert!(!w.ack(1));
        assert!(!w.resend(1, at(t0, 10_000)));
        assert!(!w.poll(1, at(t0, 10_000)));
        assert!(!w.missing(1));
    }

    #[test]
    fn ack_after_the_resend_never_warns() {
        // A keyboard still on its boot splash answers after the resend.
        let t0 = Instant::now();
        let mut w = HandshakeWatch::default();
        w.begin(1, t0);
        assert!(w.resend(1, at(t0, 3_000)));
        assert!(!w.ack(1), "no warning was shown, so nothing to clear");
        assert!(!w.poll(1, at(t0, 9_000)));
        assert!(!w.missing(1));
    }

    #[test]
    fn late_ack_clears_the_warning() {
        let t0 = Instant::now();
        let mut w = HandshakeWatch::default();
        w.begin(1, t0);
        assert!(w.resend(1, at(t0, 3_000)));
        assert!(w.poll(1, at(t0, 6_500)));
        assert!(w.ack(1), "the caller logs that the ack came late");
        assert!(!w.missing(1));
        assert!(!w.ack(1), "a second ack clears nothing");
    }

    #[test]
    fn a_new_connection_resets_and_old_ones_are_ignored() {
        let t0 = Instant::now();
        let mut w = HandshakeWatch::default();
        w.begin(1, t0);
        assert!(w.resend(1, at(t0, 3_000)));
        assert!(w.poll(1, at(t0, 6_000)));
        w.begin(2, at(t0, 7_000));
        assert!(!w.missing(2), "the warning belongs to the old connection");
        assert!(!w.missing(1));
        // Timers left over from connection 1 must not act on connection 2.
        assert!(!w.resend(1, at(t0, 11_000)));
        assert!(!w.poll(1, at(t0, 14_000)));
        assert!(!w.ack(1));
        assert!(w.resend(2, at(t0, 10_000)));
        assert!(w.poll(2, at(t0, 13_000)));
        assert!(w.missing(2));
    }

    #[test]
    fn never_armed_never_warns() {
        let mut w = HandshakeWatch::default();
        assert!(!w.resend(0, Instant::now() + Duration::from_secs(60)));
        assert!(!w.poll(0, Instant::now() + Duration::from_secs(60)));
        assert!(!w.ack(0));
        assert!(!w.missing(0));
    }
}
