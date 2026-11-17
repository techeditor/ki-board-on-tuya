/**
 * @file bringup_uart.h
 * @brief Shared UART0 helpers for expansion-baseboard bring-up applications.
 */

#ifndef BRINGUP_UART_H
#define BRINGUP_UART_H

#include <stdint.h>

#include "tuya_cloud_types.h"

OPERATE_RET bringup_uart_init(void);
int         bringup_uart_read(uint8_t *data, uint32_t length);
int         bringup_uart_write(const void *data, uint32_t length);
int         bringup_uart_printf(const char *format, ...);

#endif /* BRINGUP_UART_H */
