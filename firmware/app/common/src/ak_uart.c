/**
 * @file ak_uart.c
 * @brief UART0 transport backend for the Agentic Keyboard product firmware.
 */

#include "ak_uart.h"

#include "ak_board_io.h"
#include "tal_api.h"
#include "tal_uart.h"
#include "tkl_pinmux.h"

OPERATE_RET ak_uart_init(void)
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

int ak_uart_read(uint8_t *data, uint32_t length)
{
    return tal_uart_read(AK_UART_PORT, data, length);
}

int ak_uart_write(const void *data, uint32_t length)
{
    return tal_uart_write(AK_UART_PORT, (const uint8_t *)data, length);
}
