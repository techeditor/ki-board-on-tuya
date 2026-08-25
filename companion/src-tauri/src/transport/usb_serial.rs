//! UART / USB-CDC transport backend (Phase 1).
//!
//! Skeleton. Real serialport wiring, VID/PID auto-discovery, and a line-reader
//! thread land in Task 6. For now this only defines the type and the trait impl
//! shape so the crate compiles and the interface is stable.

use super::{BoardTransport, TransportKind};
use serde_json::Value;

/// USB-CDC / UART connection to the board.
pub struct UsbSerialTransport {
    port_name: String,
}

impl UsbSerialTransport {
    pub fn new(port_name: impl Into<String>) -> Self {
        Self {
            port_name: port_name.into(),
        }
    }
}

impl BoardTransport for UsbSerialTransport {
    fn kind(&self) -> TransportKind {
        TransportKind::Usb
    }

    fn endpoint(&self) -> String {
        self.port_name.clone()
    }

    fn write_json(&mut self, _payload: &Value) -> Result<(), String> {
        Err("usb_serial: not implemented (Task 6)".into())
    }

    fn disconnect(&mut self) {}
}
