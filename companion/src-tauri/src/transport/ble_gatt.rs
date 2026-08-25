//! BLE GATT transport backend (Phase 2).
//!
//! Skeleton. Real btleplug scan/connect/notify wiring lands in Task 13. The type
//! exists now so the transport interface is stable across phases.

use super::{BoardTransport, TransportKind};
use serde_json::Value;

/// BLE GATT connection to the board.
pub struct BleGattTransport {
    device_id: String,
}

impl BleGattTransport {
    pub fn new(device_id: impl Into<String>) -> Self {
        Self {
            device_id: device_id.into(),
        }
    }
}

impl BoardTransport for BleGattTransport {
    fn kind(&self) -> TransportKind {
        TransportKind::Ble
    }

    fn endpoint(&self) -> String {
        self.device_id.clone()
    }

    fn write_json(&mut self, _payload: &Value) -> Result<(), String> {
        Err("ble_gatt: not implemented (Task 13)".into())
    }

    fn disconnect(&mut self) {}
}
