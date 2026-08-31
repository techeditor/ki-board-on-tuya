# Hardware bring-up acceptance results

Physical acceptance log for the expansion baseboard, following the sequence in
`TEST_PLAN.md`. One row per attempt. Record the COM port, board revision,
observed result, and any scope/logic-analyzer measurements.

Firmware SHA-256 is optional during bring-up (fast local iterate/flash loop; the
board and firmware are on hand). Capture it later for release/OTA builds when
traceability matters.

Status legend: PASS / FAIL / PARTIAL / TODO.

## Environment

| Item | Value |
|------|-------|
| Flash host | Windows (`C:\@MyWorks\21_HardWorks\agentic_keyboard_with_tuya`) |
| Flash tool | tyutool v3.2.9 (windows/x86_64) |
| Flash baud | 460800 |
| UART0 monitor baud | 115200 |
| Board revision | v1.0 |

### COM port map (this host) - CONFIRMED 2026-08-31

| COM | Bridge | UART | Baud | Content |
|-----|--------|------|------|---------|
| COM6 | Expansion-board CH340C (self-added Type-C) | **UART0** (P10/P11) | 115200 | Application frames (ready/heartbeat/key/nav/pattern) |
| COM8 | Dev-board CH342F, channel A | **UART0** (P10/P11) | 115200 | Same as COM6 - identical content |
| COM7 | Dev-board CH342F, channel B | **UART1** (debug) | 460800 | SDK debug log: PR_NOTICE/PR_ERR, boot log, HardFault dumps |

Confirmed on the bench: COM6 and COM8 carry identical content because BOTH bridge
the same UART0 (the CH340C and the CH342F channel A both sit on P10/P11). COM7 is
the separate UART1 debug port at 460800 (where the ws2812 HardFault dump came
from).

Practical rules:
- Application JSONL frames: monitor COM6 or COM8 at 115200.
- Firmware debug log / crash dumps: monitor COM7 at 460800.
- COM6 and COM8 both drive UART0, so only one Type-C should be connected at a
  time to avoid bus contention (PRD H15).

(Correction: an earlier note called COM8 a "debug port"; that was wrong - COM8 is
a second UART0 application port, identical to COM6. The debug port is COM7.)

## Results

| Step | App / check | Date | COM | Result | Notes |
|------|-------------|------|-----|--------|-------|
| 0 | Power-off visual inspection | 2026-08-31 | — | PASS | Inspected before power-up |
| 1 | `uart_loopback` | 2026-08-31 | COM6 | PASS | TX/heartbeat OK; RX echo confirmed in a bidirectional terminal |
| 2 | `key1_uart` | 2026-08-31 | COM6 | PASS | See log below |
| 3 | `input_scan` | 2026-08-31 | COM6 | PASS | All 10 inputs OK, incl. concurrent presses |
| 4 | `lcd_st7789` | 2026-08-31 | COM8 | PASS | Solid-white was a reversed FPC; after reseating the flex correctly the color bars/patterns display |
| 5 | `ws2812` | | | TODO | |
| 6 | LED4/LED5 manual check | 2026-08-31 | — | PASS | LED4 follows Type-C VBUS, LED5 follows battery BAT_P; verified independent |
| 7 | Battery/switch manual check | 2026-08-31 | — | PASS | Switch controls battery path (via LED5); Type-C independent; single cell |
| 8 | `wireless_selftest` | 2026-08-31 | COM6 | PASS | Wi-Fi scan finds 13-28 APs; BLE `AK-Bringup` connect/disconnect events, adv restarts |

## Step 2 - key1_uart (2026-08-31, COM6, PASS)

Validates the flashing path (CH340C/UART0 + S8050 auto-reset), UART0 device->PC
communication, and KEY1=P24 input with 20 ms debounce.

Observed over UART0 @ 115200:

- Heartbeat `status:"alive"` every 2000 ms (uptime 388220 -> 410220 ms), stable
  interval confirms the main loop and UART0 TX path.
- 12 complete press/release pairs, seq 1-24, strictly increasing, no gaps, no
  dropped frames.
- Debounce effective: exactly one down + one up per press, no chatter.
- `held_ms` plausible and physical: short taps 90-175 ms; one ~810 ms hold
  (seq 22, near the long-press threshold); one 1550 ms and one 5450 ms long hold
  (seq 20, 24). `down` frames always carry `held_ms:0` by design.

Sample:

```json
{"v":1,"type":"bringup","payload":{"name":"key1_uart","status":"alive","uptime_ms":388220}}
{"v":1,"seq":1,"type":"key","payload":{"id":"k1","action":"down","held_ms":0}}
{"v":1,"seq":2,"type":"key","payload":{"id":"k1","action":"up","held_ms":175}}
{"v":1,"seq":20,"type":"key","payload":{"id":"k1","action":"up","held_ms":1550}}
{"v":1,"seq":24,"type":"key","payload":{"id":"k1","action":"up","held_ms":5450}}
```

Note: the boot-time `ready` frame was not observed because the monitor attached
~388 s after boot; this is expected (`ready` is sent once at startup).

Confirmed blocks: CH340C(U3) flash link, S8050 auto-reset, UART0 (P10/P11)
device->PC, KEY1 (P24) pull-up/active-low/debounce, system power, RTOS loop.

## Step 3 - input_scan (2026-08-31, COM6, PASS)

Validates all five mechanical keys and all five navigation directions. Each
input is an independent debounced state machine polled every 5 ms (20 ms
debounce).

Observed over UART0 @ 115200 (seq 1-172):

- All 5 keys reported with clean down/up pairs and correct IDs:
  k1=P24, k2=P25, k3=P26, k4=P27, k5=P23.
- All 5 navigation directions reported with clean down/up pairs and correct
  labels: up=P32, down=P20, left=P21, right=P22, center=P30.
- No idle chatter: between deliberate presses only the 2000 ms heartbeat
  appeared (uptime 6219 -> 72224 ms, stable interval).
- Debounce effective: one down + one up per press; `held_ms` ranged 40-3435 ms,
  including several long holds (center 3435 ms, down 2480/930 ms), all captured.
- Concurrent / overlapping presses handled correctly (independent per-input
  state machines, no cross-talk):
  - seq 125-128: k3 down, k4 down, k3 up, k4 up (two keys overlapping)
  - seq 145-152: interleaved k1/k2 press/release, all correctly paired
  - seq 96/98: a key event nested inside a center hold

This confirms the pin mapping for all 10 inputs and satisfies PRD FR-1/FR-2
(no cross-talk, well-defined multi-input behavior).

Sample:

```json
{"v":1,"seq":1,"type":"key","payload":{"id":"k1","action":"down","held_ms":0}}
{"v":1,"seq":23,"type":"nav","payload":{"dir":"center","action":"down","held_ms":0}}
{"v":1,"seq":27,"type":"nav","payload":{"dir":"up","action":"down","held_ms":0}}
{"v":1,"seq":125,"type":"key","payload":{"id":"k3","action":"down","held_ms":0}}
{"v":1,"seq":126,"type":"key","payload":{"id":"k4","action":"down","held_ms":0}}
{"v":1,"seq":127,"type":"key","payload":{"id":"k3","action":"up","held_ms":135}}
{"v":1,"seq":128,"type":"key","payload":{"id":"k4","action":"up","held_ms":140}}
```

Confirmed blocks: KEY1-5 (P24/P25/P26/P27/P23), NAV up/down/left/right/center
(P32/P20/P21/P22/P30), all with pull-up/active-low/debounce and no cross-talk.

## Step 0 - power-off visual inspection (2026-08-31, PASS)

Performed per the Step 0 detail checklist in `TEST_PLAN.md` before power-up:
solder quality, part orientation, FPC/LCD seating and polarity, battery
polarity and single-cell rule, and multimeter continuity (no rail-to-ground
shorts). No issues found. Note: the board had already run Steps 2/3 powered, so
the "could fry the board" risk was effectively already retired; this pass
formally records the inspection state, including the single-cell confirmation.

## Step 1 - uart_loopback (2026-08-31, COM6, PASS)

This app validates two directions:
- device->PC (TX): boot `ready` frame + 2000 ms heartbeat.
- PC->device (RX): every received byte echoed back unchanged.

Both directions confirmed on COM6 @ 115200 in a bidirectional terminal:

- TX / device->PC: stable `status:"alive"` heartbeat at a 2000 ms interval
  (uptime 38218 -> 58218 ms, +2000 each), confirming the main loop and UART0 TX.
- RX / echo: sent `hello，你好` and the device returned the exact same bytes
  (`hello，你好`), including the multi-byte UTF-8 Chinese characters - confirms
  the RX path and byte-for-byte echo. The echo is a raw byte passthrough with no
  format requirement on the sent data (not JSON, no trailing newline needed).

The echoed text runs directly into the following heartbeat frame with no
separator; expected, since echoed bytes are passed through verbatim (no `\n`)
while heartbeat frames carry their own trailing `\n`.

Sample (» sent to device, « returned by device):

```json
{"v":1,"type":"bringup","payload":{"name":"uart_loopback","status":"alive","uptime_ms":38218}}
» hello，你好
« hello，你好{"v":1,"type":"bringup","payload":{"name":"uart_loopback","status":"alive","uptime_ms":48218}}
```

Confirmed blocks: CH340C(U3) UART0 bridge both directions (P10/P11 device<->PC),
RTOS loop, UART0 TX/RX at 115200 8N1.

## Step 4 - lcd_st7789 (2026-08-31, COM8, FAIL - wiring)

Symptom: the panel is lit but shows a constant solid white; it does not follow
the firmware's 2 s pattern cycle.

Firmware is confirmed healthy. On UART0 (found on COM8, CH342-A):

- `status:"ready"` with width=240, height=320, spi_hz=48000000 -> display
  register, open, and PSRAM frame-buffer allocation all succeeded.
- Steady 2 s cycle: color_bars -> red -> green -> blue -> white -> black -> ...
- Every frame reports `result:0` (draw + flush to panel returned success).

So the T5 side (software through SPI output) is fully working and pixel data is
being pushed, but the panel never shows anything but white. Backlight and panel
power are OK (screen is lit). The fault is in the SPI data path / control lines
/ FPC seating, not in software. Panel controller is ST7789V, which is
command/timing compatible with the ST7789 driver - not the cause.

CL28CK230-18A driver is ST7789V (compatible with the ST7789 driver; not the
cause).

Suspect order:
1. FPC misseated / off-by-one / reversed (PRD 3.3 warns an older doc was shifted
   by one pin). 18P order: Pin1=GND, Pin2=RST. A shifted/reversed flex scrambles
   SCK/MOSI/DC/CS and typically yields a lit-but-constant-color panel - matches.
2. SCK(P14/FPC3/J1.14), MOSI(P16/FPC6/J1.13), DC(P17/FPC4/J1.12) continuity from
   FPC pad to T5 header (cold joint / open / bridge). Also CS(P15/FPC5/J1.15),
   RST(P6/FPC2/J2.18).
3. SCK/MOSI series damping resistors (EDA R11/R12) open or wrong value.

RESOLVED (2026-08-31): the FPC was reversed. After reseating the flex in the
correct orientation, the panel shows the pattern cycle (color bars + solid
RGB/white/black), confirming the whole SPI path (SCK/MOSI/DC/CS/RST) is good.
The earlier solid-white was exactly the "reversed/off-by-one flex scrambles the
SPI lines -> lit but constant color" failure that PRD 3.3 warns about. Firmware
was never at fault. Step 4 -> PASS.

Root cause CONFIRMED (2026-08-31): the PCB FPC connector (FPC1) footprint pin
order is MIRRORED (Pin1<->Pin18). FPC1 sits on the bottom layer and its footprint
was not mirrored to match, so the physical pin order is reversed while the
logical netlist (FPC.1=GND, .2=RST, .3=SCK ... .9=VCC) is correct. This is a
board design defect, not an assembly mistake.

Workaround on this v1.0 board: insert the flex REVERSED, so the two mirrorings
cancel and every pin lands correctly - which is why the color bars now display.

Cross-referenced/updated:
- `hardware/eda/pcb_review_report_2026-08-24.md`: H4 changed from "pending
  visual check" to "confirmed defect"; the original review only checked the
  bottom/top window orientation and missed that bottom-layer placement mirrors
  the pin order, so H4 was never closed until now.
- `hardware/pinmap/netlist.md`: FPC1 row annotated with the mirror defect and the
  reversed-insert workaround.

Follow-ups:
- Next board revision MUST fix the FPC1 footprint pin order.
- On this board, verify the reversed flex makes reliable contact (watch for
  flicker/banding from a forced/half-seated fit); note whether VCC/GND ever
  bridged a signal pin during the reversed-insert attempts (panel currently
  works, so likely no damage).

### Second issue on the same step: colors bit-inverted (fixed in firmware)

After the flex was reseated the bars displayed, but the solid frames were
bit-inverted: firmware red->cyan, green->magenta, blue->yellow, white->black,
black->white. Every frame matched a bitwise NOT, which is display inversion,
not a wiring or byte-order issue.

Root cause: the stock TuyaOpen ST7789 init sequence
(`src/peripherals/display/tdd_display/src/spi/tdd_disp_spi_st7789.c`,
`cST7789_INIT_SEQ`) ends with `ST7789_INVON` (0x21). Some IPS panels need that,
but the CL28CK230-18A does not, so it displayed everything inverted.

Fix (firmware, `lcd_st7789/src/tuya_app_main.c`): added a local copy of the init
sequence `s_st7789_init_seq_noinv` with the final `INVON` replaced by `INVOFF`
(0x20), injected via `tdd_disp_spi_st7789_set_init_seq()` before register. Built
clean on EC2 (`lcd_st7789_QIO_0.1.0.bin`). After flashing, all colors correct:
bars in order, and solid red/green/blue/white/black display correctly.

Panel adaptation note (reuse later): the CL28CK230-18A needs INVOFF, not the
stock INVON. The product firmware LCD module (Task 9) must apply the same
no-inversion init sequence.

Step 4 -> PASS (both the reversed-flex hardware workaround and the INVOFF
firmware fix in place).

### Serial-output investigation (code-confirmed)

Question: why did COM6 show nothing during lcd_st7789 while COM7/COM8 worked in
earlier steps?

Code review of the shared helper `common/src/bringup_uart.c` settles the
firmware side. Every bring-up app (uart_loopback, key1_uart, input_scan,
lcd_st7789) uses this one helper and none opens a UART of its own:

- `tkl_io_pinmux_config(AK_UART_RX_PIN=P10, TUYA_UART0_RX)`
- `tkl_io_pinmux_config(AK_UART_TX_PIN=P11, TUYA_UART0_TX)`
- `AK_UART_PORT = TUYA_UART_NUM_0`, 115200 8N1

Conclusion: all four apps emit their application frames on **UART0 (P10/P11) at
115200** - lcd_st7789 is identical to the earlier steps. The `ready` / pattern
frames are `bringup_uart_printf` output, so they are on UART0 by construction;
receiving them on COM8 proves COM8 is bridged to UART0 for that session.

Separately, `PR_NOTICE`/`PR_ERR` debug logs go through `tal_log_init` +
`tkl_log_output` on the SDK debug port (UART1, 460800) - a different port from
the `bringup_uart` application frames, present in every app.

Therefore the "COM6 silent during lcd" symptom is NOT a firmware issue and NOT
a wrong-port-in-code issue. Both CH340C (COM6) and CH342F (COM7/8) bridge the
same UART0; which COM receives depends on which Type-C bridge was powered/active
during that session. Most likely the expansion-board Type-C (COM6/CH340C) was
not plugged/powered during Step 4, so CH340C was dead while the dev-board
CH342F carried UART0.

Bench fact (from operator): during the lcd_st7789 run the expansion-board
Type-C (COM6) WAS plugged in, and the board was powered from the battery.
COM7/COM8 also appeared, which means the dev-board Type-C was ALSO plugged in at
the same time - i.e. both Type-C bridges were connected together.

This changes the analysis. Two relevant facts from PRD 3.6 / H15/H16:

1. CH340C is powered by ITS OWN Type-C VBUS: `VBUS -> LDO (HE9073A33M5R 3.3V) ->
   CH340C VCC`. It does NOT draw from the battery or system rail. So the battery
   powering the T5 system does not power CH340C; only the expansion Type-C VBUS
   does. If that VBUS/LDO chain is not actually delivering 3.3V to CH340C, the
   chip is dead and COM6 stays silent even though UART0 data reaches its RX pin.
2. Both bridges share UART0 (P10/P11); PRD explicitly says connect only one
   Type-C at a time (H15, bus contention). This run had both plugged in.

So COM6 silence is likely either (A) CH340C not actually powered (VBUS->LDO->VCC
issue), or (B) contention/steering from having both Type-C plugged at once.

Decisive test (do on the bench): power the system from the battery, plug ONLY
the expansion-board Type-C (COM6) for data, UNPLUG the dev-board Type-C, then
monitor COM6 at 115200.
- If COM6 now outputs -> the earlier silence was the two-Type-C combination
  (matches PRD "only one at a time"); not a hardware defect.
- If COM6 is still silent -> CH340C power chain (H16). Meter: expansion Type-C
  VBUS = 5V; LDO output / CH340C VCC = 3.3V; V3 tied to VCC.

Note this matters for the shipping product: the enclosure exposes only the
expansion-board Type-C, so the CH340C/COM6 path must work reliably on battery +
its own Type-C alone.

## Step 6 - LED4/LED5 manual check (2026-08-31, PASS)

Pure hardware visual check, no firmware and no GPIO involved. LED4/LED5 are
plain red 0603 indicators wired directly to power rails:

- LED4: `VBUS -> R6 -> LED4 -> GND` = Type-C VBUS (5V) present indicator.
- LED5: `BAT_P -> R10 -> LED5 -> GND` = battery (BAT_P) present indicator.

Method - toggle each source and observe the two LEDs, which must be independent:

| Condition | LED4 (VBUS) | LED5 (BAT) |
|-----------|-------------|------------|
| All off (Type-C out, battery switch OFF) | off | off |
| Type-C only (battery switch OFF) | on | off |
| Battery switch ON only (Type-C out) | off | on |
| Type-C + battery switch ON | on | on |

If an LED stays dark, meter its branch: confirm the source rail (VBUS ~5V /
BAT_P ~cell voltage), then the series resistor (R6/R10) and LED polarity for a
cold joint or reversed part.

Result: all four conditions matched the table. LED4 tracks Type-C VBUS and LED5
tracks battery BAT_P, independently. PASS.

## Step 7 - Battery/switch manual check (2026-08-31, PASS)

Pure hardware check. Wiring: `BAT_CELL+ -> SW_BAT (SK12D07VG5) -> BAT+` net
(shared with the dev-board JST). Type-C VBUS feeds only the CH340C via the LDO;
it does not power the system rail.

Method / pass criteria:

1. Switch controls the battery path: toggling SW1 turns LED5 on/off, proving the
   switch is in series with BAT+.
2. Type-C behavior matches the schematic: with Type-C only (battery switch OFF),
   LED4 is on (VBUS present) and the CH340C serial bridge enumerates, while the
   system main power still comes from battery/dev-board (Type-C does not feed the
   system rail).
3. Only one cell connected: the expansion-board JST (BT1) and the dev-board
   battery holder share the same BAT net, so exactly one cell may be connected at
   a time (parallel cells back-feed - unsafe).

Optional meter checks: switch continuity (ON conducts, OFF open); BAT+ net
voltage appears/disappears with the switch; VBUS ~5V.

Result: SW1 cleanly gates LED5 (battery path), Type-C stays independent of the
battery path, and only a single cell is connected. PASS.

## Step 8 - wireless_selftest (2026-08-31, COM6, PASS)

Validates the T5 radio (Wi-Fi scan + BLE advertising) with no credentials, no
provisioning, and no Tuya cloud activation. Frames observed over UART0 @ 115200.

Method:
1. Flash `wireless_selftest`, monitor UART0 (COM6) at 115200.
2. Read the periodic `wifi":"scan_ok"` frames (a scan every 10 s).
3. From a phone/PC BLE scanner, find `AK-Bringup`, then connect and disconnect.

Observed - all three criteria met:

- Wi-Fi scan (2.4 GHz only): repeated `scan_ok` with `ap_count` 13-28 and
  `best_rssi` -47 to -56 dBm (strong). No `scan_error`. RF/antenna path good.
- BLE visible + connect: `ble":"connected","result":0` seen twice - the device
  advertised as `AK-Bringup` (per the ready frame) and accepted connections.
- Disconnect + adv restart: `ble":"disconnected","reason":19` twice, and a
  second `connected` after the first disconnect confirms advertising auto-restarts
  and reconnection works. `reason:19` = remote-user-terminated (the scanner app
  ended the link), which is normal, not a fault.

Sample:

```json
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","status":"ready","ble_name":"AK-Bringup"}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"advertising"}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","wifi":"scan_ok","ap_count":28,"best_rssi":-47}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"connected","result":0}}
{"v":1,"type":"bringup","payload":{"name":"wireless_selftest","ble":"disconnected","reason":19}}
```

Scope: this app only scans and advertises; it does not join an AP, provision, or
activate cloud. Connected networking is validated later in provisioning / BLE HID.

Confirmed blocks: T5 2.4 GHz Wi-Fi RX/scan path, BLE peripheral advertising,
connect/disconnect lifecycle, advertising auto-restart.
