# KEY1 + UART0 bring-up

Validates the first expansion-baseboard input and the wired PC transport:

- KEY1: T5 P24 / header J2.13, internal pull-up, active low.
- UART0: RX=P10, TX=P11, 115200 8N1, no flow control.
- Debounce: 20 ms.
- Debug logs: separate UART1 port at 460800.

The application sends a ready frame on boot, a heartbeat every two seconds,
and protocol-v1 `key` frames on stable press/release transitions.

Expected UART0 output:

```json
{"v":1,"type":"bringup","payload":{"name":"key1_uart","status":"ready","key_gpio":24,"uart":0,"baud":115200}}
{"v":1,"seq":1,"type":"key","payload":{"id":"k1","action":"down","held_ms":0}}
{"v":1,"seq":2,"type":"key","payload":{"id":"k1","action":"up","held_ms":153}}
```

Build and flash instructions are in the parent `bringup/README.md`.
