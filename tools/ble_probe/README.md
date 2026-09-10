# BLE probe — PC-side test client for FP-2 `ble_gatt_link`

`ak_ble_test.py` connects to the board (advertising as **`AK-KB`**) using your
computer's own Bluetooth adapter (via [bleak](https://github.com/hbldh/bleak) —
no extra hardware) and exercises Tuya's fixed command service:

| UUID     | Role   | Direction         | On the board |
|----------|--------|-------------------|--------------|
| `0x1910` | service | —                | Tuya command service V1 |
| `0x2B10` | notify  | board → PC       | `tal_ble_server_common_send()` pushes key/nav/ping/link JSONL |
| `0x2B11` | write   | PC → board       | arrives as `TAL_BLE_EVT_WRITE_REQ` → UART `link_rx` + LCD RX counter |

> These UUIDs are Tuya's, defined in the SDK at
> `src/tal_bluetooth/include/tal_bluetooth_def.h`. This client just talks to them;
> it is the minimal rehearsal of the companion's future BLE backend.

## Run (on your Windows / macOS / Linux machine — not the EC2 build host)

```bash
pip install -r requirements.txt      # or: pip install bleak
python ak_ble_test.py                 # scan for AK-KB, connect, interactive
python ak_ble_test.py --name AK-KB    # explicit advertised name
python ak_ble_test.py --address <MAC> # skip scan, connect directly
python ak_ble_test.py --send "hello"  # send one line to 0x2B11 after connect
```

- **Test notify:** after it prints `subscribed to 0x2B10`, press keys / move the
  nav switch on the board — each `key`/`nav` frame prints as `NOTIFY  {...}`, and
  the board's LCD `TX` counter increments.
- **Test write:** type text at the `write>` prompt and press Enter — the board's
  UART shows `{"type":"link_rx", ...}`, its LCD shows `Last: rx N bytes`, and the
  `RX` counter increments.
- Ctrl+C to disconnect.

## Platform notes

- **Windows:** needs Windows 10 1709+ (bleak uses WinRT). Turn Bluetooth on; no
  pairing needed for this service.
- **Linux:** needs BlueZ ≥ 5.43 and a working adapter (`bluetoothctl` sees it).
- **macOS:** first run prompts for Bluetooth permission; grant it to your terminal.

If it connects but can't find `0x2B10`/`0x2B11`, the script dumps all discovered
services/characteristics so you can see what the board actually exposed.
