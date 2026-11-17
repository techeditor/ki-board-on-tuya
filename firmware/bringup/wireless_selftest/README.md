# Wi-Fi and BLE self-test

Tests the T5 radio on the T5AI-Core board without credentials or cloud
activation. The expansion board is not involved, so this test is the same on
every hardware revision.

- Scans nearby 2.4 GHz Wi-Fi access points every ten seconds and reports the
  number found and the best RSSI.
- Advertises over BLE as `AK-Bringup` (peripheral role).
- Reports BLE lifecycle events and restarts advertising after a disconnect.
- All reports are JSON lines on UART0 at 115200 baud (the T5AI-Core USB port).

```json
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","status":"ready","ble_name":"AK-Bringup"}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"advertising"}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","wifi":"scan_ok","ap_count":7,"best_rssi":-48}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"connected","result":0}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"disconnected","reason":19}}
```

Pass:

- At least one Wi-Fi network is found in a normal RF environment.
- A phone or PC BLE scanner sees `AK-Bringup`.
- Connecting and disconnecting produce `connected` / `disconnected` frames,
  and the board advertises again afterwards.

This app does not join an AP, store credentials or activate Tuya cloud
services. It does not test the product firmware's BLE GATT service or its
encrypted channel.

Build and flash instructions are in the parent [`README.md`](../README.md).
