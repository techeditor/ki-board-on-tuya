/**
 * @file agentic_board_io.h
 * @brief Authoritative IO mapping for the Agentic Keyboard expansion baseboard.
 */

#ifndef AGENTIC_BOARD_IO_H
#define AGENTIC_BOARD_IO_H

#include "tuya_cloud_types.h"

/* Expansion-board USB-UART bridge: CH340C <-> T5 UART0. */
#define AK_UART_PORT           TUYA_UART_NUM_0
#define AK_UART_RX_PIN         TUYA_IO_PIN_10
#define AK_UART_TX_PIN         TUYA_IO_PIN_11
#define AK_UART_BAUDRATE       115200U
#define AK_UART_RX_BUFFER_SIZE 256U

/* Five mechanical switches: internal pull-up, pressed level is low. */
#define AK_KEY1_PIN           TUYA_GPIO_NUM_24
#define AK_KEY2_PIN           TUYA_GPIO_NUM_25
#define AK_KEY3_PIN           TUYA_GPIO_NUM_26
#define AK_KEY4_PIN           TUYA_GPIO_NUM_27
#define AK_KEY5_PIN           TUYA_GPIO_NUM_23
#define AK_KEY_ACTIVE_LEVEL   TUYA_GPIO_LEVEL_LOW
#define AK_KEY_INACTIVE_LEVEL TUYA_GPIO_LEVEL_HIGH

/* Five-way navigation switch: internal pull-up, pressed level is low. */
#define AK_NAV_UP_PIN       TUYA_GPIO_NUM_32
#define AK_NAV_DOWN_PIN     TUYA_GPIO_NUM_20
#define AK_NAV_LEFT_PIN     TUYA_GPIO_NUM_21
#define AK_NAV_RIGHT_PIN    TUYA_GPIO_NUM_22
#define AK_NAV_CENTER_PIN   TUYA_GPIO_NUM_30
#define AK_NAV_ACTIVE_LEVEL TUYA_GPIO_LEVEL_LOW

/* ST7789 SPI0 LCD. Native panel orientation is 240x320. */
#define AK_LCD_SPI_PORT       TUYA_SPI_NUM_0
#define AK_LCD_SPI_CS_PIN     TUYA_GPIO_NUM_15
#define AK_LCD_SPI_SCK_PIN    TUYA_GPIO_NUM_14
#define AK_LCD_SPI_MOSI_PIN   TUYA_GPIO_NUM_16
#define AK_LCD_DC_PIN         TUYA_GPIO_NUM_17
#define AK_LCD_RST_PIN        TUYA_GPIO_NUM_6
#define AK_LCD_BACKLIGHT_PIN  TUYA_GPIO_NUM_8
#define AK_LCD_POWER_PIN      TUYA_GPIO_NUM_7
#define AK_LCD_NATIVE_WIDTH   240U
#define AK_LCD_NATIVE_HEIGHT  320U
#define AK_LCD_SPI_CLOCK_HZ   48000000U
#define AK_LCD_CONTROL_ACTIVE TUYA_GPIO_LEVEL_HIGH

/*
 * Three WS2812 LEDs through the SN74LV1T34 level shifter.
 * v1.2: driven by the official SPI WS2812 driver over SPI1. The data line is
 * SPI1_MOSI = P4 (fixed by the SDK pinmux, TKL_SPI1_MOSI_PIN = GPIO_4); the
 * whole SPI1 group P2/P3/P4/P5 is claimed by tkl_spi_init but only MOSI carries
 * data. Was P13 GPIO bit-bang in v1.0 (failed nanosecond timing).
 */
#define AK_WS2812_SPI_PORT TUYA_SPI_NUM_1
#define AK_WS2812_DATA_PIN TUYA_GPIO_NUM_4
#define AK_WS2812_COUNT    3U

#endif /* AGENTIC_BOARD_IO_H */
