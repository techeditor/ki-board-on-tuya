//! Pluggable transport layer. Mirrors ki-board's structure: a `BoardTransport`
//! trait with concrete backends (UART now, BLE GATT later). Kept IO-agnostic at
//! the trait level so the protocol layer can be reused across Phase 1/2/3.
//!
//! Backends do not depend on Tauri. A background reader thread parses nothing
//! itself; it forwards complete protocol lines over a [`LineSender`] channel.
//! The Tauri layer (Windows only) drains that channel, decodes each line with
//! [`crate::protocol`], and re-emits it as a Tauri event to the UI.

pub mod ble_gatt;
pub mod usb_serial;

use serde::Serialize;
use serde_json::Value;
use std::sync::mpsc::Sender;

/// Channel the reader thread uses to hand complete text lines to the app layer.
pub type LineSender = Sender<String>;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum TransportKind {
    None,
    Usb,
    Ble,
}

/// A connection to the board. Phase 1 = UART / USB-CDC; Phase 2 = BLE GATT.
pub trait BoardTransport: Send {
    fn kind(&self) -> TransportKind;
    fn endpoint(&self) -> String;
    fn write_json(&mut self, payload: &Value) -> Result<(), String>;
    fn disconnect(&mut self);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transport_kind_serializes_lowercase() {
        let json = serde_json::to_string(&TransportKind::Usb).unwrap();
        assert_eq!(json, "\"usb\"");
    }
}
