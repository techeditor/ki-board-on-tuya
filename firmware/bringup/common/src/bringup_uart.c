/**
 * @file bringup_uart.c
 * @brief Shared UART0 helpers for expansion-baseboard bring-up applications.
 */

#include <stdarg.h>
#include <stdio.h>

#include "agentic_board_io.h"
#include "bringup_uart.h"
#include "tal_api.h"
#include "tal_uart.h"
#include "tkl_pinmux.h"

#define BRINGUP_UART_FORMAT_BUFFER_SIZE 256U

OPERATE_RET bringup_uart_init(void)
{
    OPERATE_RET    rt       = OPRT_OK;
    TAL_UART_CFG_T uart_cfg = {0};

    TUYA_CALL_ERR_RETURN(tkl_io_pinmux_config(AK_UART_RX_PIN, TUYA_UART0_RX));
    TUYA_CALL_ERR_RETURN(tkl_io_pinmux_config(AK_UART_TX_PIN, TUYA_UART0_TX));

    uart_cfg.base_cfg.baudrate = AK_UART_BAUDRATE;
    uart_cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    uart_cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    uart_cfg.base_cfg.parity   = TUYA_UART_PARITY_TYPE_NONE;
    uart_cfg.base_cfg.flowctrl = TUYA_UART_FLOWCTRL_NONE;
    uart_cfg.rx_buffer_size    = AK_UART_RX_BUFFER_SIZE;
    uart_cfg.open_mode         = 0;

    return tal_uart_init(AK_UART_PORT, &uart_cfg);
}

int bringup_uart_read(uint8_t *data, uint32_t length)
{
    return tal_uart_read(AK_UART_PORT, data, length);
}

int bringup_uart_write(const void *data, uint32_t length)
{
    return tal_uart_write(AK_UART_PORT, (const uint8_t *)data, length);
}

int bringup_uart_printf(const char *format, ...)
{
    char    buffer[BRINGUP_UART_FORMAT_BUFFER_SIZE];
    va_list args;
    int     length;

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (length <= 0) {
        return length;
    }
    if (length >= (int)sizeof(buffer)) {
        length = (int)sizeof(buffer) - 1;
    }

    return bringup_uart_write(buffer, (uint32_t)length);
}
