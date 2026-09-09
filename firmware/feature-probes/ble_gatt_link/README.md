# FP-2 · ble-gatt-link

Wireless board↔companion link over **BLE GATT** — the MVP's wireless transport.
The board is a BLE peripheral exposing Tuya's fixed command service and speaks
the **same JSONL protocol v1** (`shared/protocol/protocol.md`) as the UART
backend, so the companion parser is reused unchanged; only the wire changes
from USB to BLE.

## GATT layout (Tuya fixed command service)

| Attribute | UUID | Direction | Use |
|-----------|------|-----------|-----|
| Service | `0x1910` | — | Tuya command service |
| Notify char | `0x2B10` | board → companion | key/nav events, hello_ack, ping (JSONL) |
| Write char | `0x2B11` | companion → board | commands / status (logged, counted) |

Advertises as **`AK-KB`** (also lists service `0x1910` in the adv data).

## What it validates

1. Advertise, accept a central (PC companion or **nRF Connect**), negotiate MTU.
2. Detect notify subscription (CCCD) on `0x2B10`.
3. Key/nav presses → JSONL frames **notified over the air**.
4. Companion **writes** to `0x2B11` → received, echoed to UART, counted.

Everything is mirrored to **UART0** (P10/P11, 115200 8N1) as JSONL so the link is
observable on serial while testing. Tuya debug log stays on UART1 @ 460800.

## Test procedure (no companion yet)

1. Flash, open the UART0 monitor (COM6) — you should see:
   ```json
   {"v":1,"type":"ack",...}                      // hello_ack, transport "ble-gatt"
   {"v":1,"type":"link","payload":{"ble":"advertising","name":"AK-KB"}}
   ```
2. On a phone, open **nRF Connect** → scan → connect to `AK-KB`.
3. Enable notifications on char `0x2B10`. UART shows `"ble":"connected"` then the
   `hello_ack` is also pushed over BLE.
4. Press keys / move the nav switch → JSONL `key`/`nav` frames appear in nRF
   Connect's notify log **and** on UART0.
5. Write bytes to char `0x2B11` in nRF Connect → UART0 prints a `link_rx` frame
   with your bytes.

> **MTU:** default ATT MTU is 23 (20-byte payload). Frames longer than that need
> an MTU exchange; the board replies with MTU 247 on request. nRF Connect and the
> companion both request a larger MTU, so full frames get through after connect.

## Build & flash

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/ble_gatt_link
tos.py config choice -c TUYA_T5AI_CORE.config
tos.py build
```

Flash from Windows (expansion board = COM6):

```powershell
.\flash.ps1 -Port COM6 -App "firmware/feature-probes/ble_gatt_link" -Monitor
```

## Reused modules

`firmware/app/common/`: `ak_uart` (UART0), `ak_input` + `ak_input_fsm` (debounced
5 keys + 5-way nav), `ak_protocol` (JSONL v1 builders). This probe adds only the
BLE peripheral glue in `src/tuya_app_main.c`.
