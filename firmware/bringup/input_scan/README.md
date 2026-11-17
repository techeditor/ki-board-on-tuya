# Input scan bring-up (v0.3)

Validates the six mechanical keys and the EC11 rotary encoder on the v0.3
baseboard. All inputs are active-low with internal pull-ups.

- Keys: K1=P24, K2=P25, K3=P26, K4=P27, K5=P23, **K6=P32**
- EC11 encoder: **A=P20, B=P21, push SW=P22**

> v0.3 replaced the v1.x five-way joystick (UP/DOWN/LEFT/RIGHT/CENTER) with the
> EC11 incremental encoder. The three joystick pins P20/P21/P22 are reused for
> the encoder A/B/SW lines, and the former NAV_UP pin is now K6.

## UART protocol

Stable transitions are emitted as protocol-v1 JSONL frames over UART0 at 115200
baud:

```json
{"v":1,"type":"bringup","payload":{"name":"input_scan","status":"ready","keys":6,"encoder":1,"inputs":8,"key_debounce_ms":20}}
{"v":1,"seq":1,"type":"key","payload":{"id":"k6","action":"down","held_ms":0}}
{"v":1,"seq":2,"type":"key","payload":{"id":"k6","action":"up","held_ms":132}}
{"v":1,"seq":3,"type":"enc","payload":{"dir":"cw","steps":1}}
{"v":1,"seq":4,"type":"enc","payload":{"dir":"ccw","steps":1}}
{"v":1,"seq":5,"type":"key","payload":{"id":"enc_sw","action":"down","held_ms":0}}
```

- `key` : one of `k1`..`k6` or `enc_sw`, with `down`/`up` and `held_ms`.
- `enc` : one frame **per detent**, `dir` = `cw` or `ccw`, `steps` = 1.

## Encoder decoding

The EC11 produces two quadrature phases A/B. Each phase is debounced (2 ms),
combined into a 2-bit code `(A<<1)|B`, and fed through a standard 16-entry
quadrature transition table. Valid transitions accumulate +/-1 sub-step; one
detent is reported once four sub-steps in the same direction have accrued
(datasheet: 15 pulses/360deg, full-quadrature, phase A as reference).

## Verify

- Each of K1-K6 produces exactly one `down` and one `up` with a plausible
  `held_ms`; no idle chatter when nothing is pressed.
- Rotating the knob one click CW emits one `{"dir":"cw","steps":1}`; CCW emits
  `ccw`. Direction must match the physical turn (if swapped, exchange A/B).
- Pressing the knob emits `enc_sw` down/up.
