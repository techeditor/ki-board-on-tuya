# UART0 loopback bring-up

Tests the wired link to the PC in both directions.

- UART0: RX=P10, TX=P11, 115200 8N1, no flow control.
- On v0.3, UART0 reaches the PC through the T5AI-Core USB port (on-board USB
  serial bridge). The expansion board has no USB and is not involved; this
  test checks the dev board and the cable. (On v0.2 hardware the same pins went
  through the CH340C on the expansion board.)
- A `ready` frame on boot, an `alive` heartbeat every two seconds.
- Every received byte is echoed back unchanged.

```json
{"v":1,"type":"bringup","payload":{"name":"uart_loopback","status":"ready","uart":0,"baud":115200}}
{"v":1,"type":"bringup","payload":{"name":"uart_loopback","status":"alive","uptime_ms":2000}}
```

Open the UART0 serial port at 115200 baud, type any text, and check that the
exact bytes come back.

Build and flash instructions are in the parent [`README.md`](../README.md).
