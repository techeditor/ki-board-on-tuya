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

/*
 * Six mechanical switches (v0.3: hot-swap sockets, K1-K6): internal pull-up,
 * pressed level is low. v0.2 had five keys; K6=P32 is the old NAV_UP pin, now
 * reused for the sixth key after the five-way joystick was replaced by EC11.
 */
#define AK_KEY1_PIN           TUYA_GPIO_NUM_24
#define AK_KEY2_PIN           TUYA_GPIO_NUM_25
#define AK_KEY3_PIN           TUYA_GPIO_NUM_26
#define AK_KEY4_PIN           TUYA_GPIO_NUM_27
#define AK_KEY5_PIN           TUYA_GPIO_NUM_23
#define AK_KEY6_PIN           TUYA_GPIO_NUM_32
#define AK_KEY_ACTIVE_LEVEL   TUYA_GPIO_LEVEL_LOW
#define AK_KEY_INACTIVE_LEVEL TUYA_GPIO_LEVEL_HIGH

/*
 * EC11 incremental rotary encoder (v0.3: replaces the five-way joystick).
 * Datasheet EC11I-152 (datasheets/EC11I-152 Datasheet.pdf), verified:
 *   - A/B two quadrature phases + a common terminal C (C tied to GND).
 *   - Push switch terminals D/E (E tied to GND).
 *   - 15 pulses/360deg with detents; datasheet 4-3 recommends using phase A as
 *     the reference for direction decoding.
 * Pins reuse the old joystick GPIOs: A=P20, B=P21, SW=P22.
 * A/B have RC hardware debounce (10k pull-up to 3V3 + 10k series + 0.01uF);
 * firmware still runs a quadrature state machine. SW is active-low to GND.
 * The old NAV_CENTER pin P30 is freed (NC) in v0.3.
 */
#define AK_EC11_A_PIN        TUYA_GPIO_NUM_20
#define AK_EC11_B_PIN        TUYA_GPIO_NUM_21
#define AK_EC11_SW_PIN       TUYA_GPIO_NUM_22
#define AK_EC11_SW_ACTIVE_LEVEL TUYA_GPIO_LEVEL_LOW

/*
 * ST7789 SPI0 LCD. Native panel orientation is 240x320.
 * v0.3: panel changed from 18P CL28CK230-18A to 12P T280C7-C12-26 (ST7789P3,
 * 2.8" IPS). SPI/BL pins are unchanged. The old LCD_PWR enable on P7 is REMOVED
 * because the new 12P panel wires VDD (FPC pin 9) straight to 3V3 with no power
 * switch -- P7 is repurposed as the LCD TE (tearing-effect) input.
 *
 * NOTE (hardware doc mismatch, pending final confirmation): the v0.3 schematic
 * review (2026-10-01) routes TE to P7 via a 10k series resistor as a GPIO
 * input, while the change-scope pin table leaves FPC TE open/NC. Either way
 * bring-up must NOT drive P7 as an output. We define it as an input and do not
 * actively use TE; revisit once the schematic is frozen.
 */
#define AK_LCD_SPI_PORT       TUYA_SPI_NUM_0
#define AK_LCD_SPI_CS_PIN     TUYA_GPIO_NUM_15
#define AK_LCD_SPI_SCK_PIN    TUYA_GPIO_NUM_14
#define AK_LCD_SPI_MOSI_PIN   TUYA_GPIO_NUM_16
#define AK_LCD_DC_PIN         TUYA_GPIO_NUM_17
#define AK_LCD_RST_PIN        TUYA_GPIO_NUM_6
#define AK_LCD_BACKLIGHT_PIN  TUYA_GPIO_NUM_8
#define AK_LCD_TE_PIN         TUYA_GPIO_NUM_7
#define AK_LCD_NATIVE_WIDTH   240U
#define AK_LCD_NATIVE_HEIGHT  320U
#define AK_LCD_SPI_CLOCK_HZ   48000000U
#define AK_LCD_CONTROL_ACTIVE TUYA_GPIO_LEVEL_HIGH

/*
 * WS2812 LEDs through the SN74LV1T34 level shifter.
 * v0.3: 7 LEDs (one per key K1-K6 + LED7 near the EC11 encoder), single-wire
 * daisy chain. v1.2 was 3 LEDs. Driven by the official SPI WS2812 driver over
 * SPI1. The data line is SPI1_MOSI = P4 (fixed by the SDK pinmux,
 * TKL_SPI1_MOSI_PIN = GPIO_4); the whole SPI1 group P2/P3/P4/P5 is claimed by
 * tkl_spi_init but only MOSI carries data. Was P13 GPIO bit-bang in v1.0.
 */
#define AK_WS2812_SPI_PORT TUYA_SPI_NUM_1
#define AK_WS2812_DATA_PIN TUYA_GPIO_NUM_4
#define AK_WS2812_COUNT    7U

#endif /* AGENTIC_BOARD_IO_H */
