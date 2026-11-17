# KEY1 + UART0 bring-up

The smallest input test: one key and the wired link to the PC.

- KEY1: T5 P24 / header J2.13, internal pull-up, active low. Only KEY1 is
  read; use `input_scan` for all six keys and the knob.
- UART0: RX=P10, TX=P11, 115200 8N1, no flow control. On v0.3 UART0 reaches
  the PC through the T5AI-Core USB port (on-board USB serial bridge); the
  expansion board has no USB.
- Debounce: 20 ms.
- Debug log: separate UART1 at 460800.

On boot the app sends a `ready` frame, then an `alive` heartbeat every two
seconds, and a protocol-v1 `key` frame on each stable press and release.

Expected UART0 output:

```json
{"v":1,"type":"bringup","payload":{"name":"key1_uart","status":"ready","key_gpio":24,"uart":0,"baud":115200}}
{"v":1,"type":"bringup","payload":{"name":"key1_uart","status":"alive","uptime_ms":2000}}
{"v":1,"seq":1,"type":"key","payload":{"id":"k1","action":"down","held_ms":0}}
{"v":1,"seq":2,"type":"key","payload":{"id":"k1","action":"up","held_ms":153}}
```

Pass: one `down` and one `up` per press, with a plausible `held_ms`, and no
frames while the key is idle.

Build and flash instructions are in the parent [`README.md`](../README.md).
