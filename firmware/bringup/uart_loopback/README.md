# UART0 loopback bring-up

Validates bidirectional communication through the expansion-board CH340C:

- UART0 RX=P10, TX=P11
- 115200 8N1, no flow control
- Ready frame on boot
- Heartbeat every two seconds
- Every received byte is echoed unchanged

Open the UART0 COM port at 115200 baud, type any text, and verify the exact
bytes are returned.
