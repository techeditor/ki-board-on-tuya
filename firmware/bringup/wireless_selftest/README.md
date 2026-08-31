# Wi-Fi and BLE self-test

Validates the T5 radio without credentials or cloud activation:

- Scans nearby 2.4 GHz Wi-Fi access points every ten seconds.
- Advertises over BLE as `AK-Bringup`.
- Reports Wi-Fi scan count/best RSSI and BLE lifecycle events over UART0 at
  115200 baud.

Acceptance:

- At least one nearby Wi-Fi network is found in a normal RF environment.
- A phone or PC BLE scanner can see `AK-Bringup`.
- Connecting and disconnecting produces UART events and advertising restarts.

This application does not connect to an AP, provision credentials, or activate
Tuya cloud services.
