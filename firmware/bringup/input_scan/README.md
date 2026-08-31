# Full input scan bring-up

Validates all ten active-low inputs with 20 ms debounce:

- Keys: K1=P24, K2=P25, K3=P26, K4=P27, K5=P23
- Navigation: up=P32, down=P20, left=P21, right=P22, center=P30

Stable transitions are emitted as protocol-v1 `key` and `nav` JSONL frames over
UART0 at 115200 baud. Multiple simultaneous directions are reported
independently.
