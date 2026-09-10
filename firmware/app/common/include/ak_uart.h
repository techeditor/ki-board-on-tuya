/**
 * @file ak_uart.h
 * @brief UART0 transport backend for the Agentic Keyboard product firmware.
 *
 * Phase-1 transport: CH340C <-> T5 UART0, 115200 8N1, carrying the JSONL
 * protocol v1 (shared/protocol/protocol.md). Kept behind a minimal interface
 * so BLE GATT can slot in later as an alternative backend.
 */

#ifndef AK_UART_H
#define AK_UART_H

#include <stdint.h>

#include "tuya_cloud_types.h"

OPERATE_RET ak_uart_init(void);
int         ak_uart_read(uint8_t *data, uint32_t length);
int         ak_uart_write(const void *data, uint32_t length);

#endif /* AK_UART_H */
