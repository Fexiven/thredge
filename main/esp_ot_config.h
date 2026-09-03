/*
 * OpenThread platform configuration for the ESP32-C5 single-chip border router.
 *
 * Adapted from esp-thread-br/examples/basic_thread_border_router. The
 * important difference: a RADIO_MODE_NATIVE branch, because the ESP32-C5
 * drives its own on-chip 802.15.4 radio instead of an external RCP.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_openthread_types.h"
#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_RADIO_NATIVE
/* ESP32-C5: on-chip 802.15.4 radio, no RCP. */
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG() \
    {                                         \
        .radio_mode = RADIO_MODE_NATIVE,      \
    }
#elif CONFIG_OPENTHREAD_RADIO_SPINEL_UART
#include "driver/uart.h"
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()              \
    {                                                      \
        .radio_mode = RADIO_MODE_UART_RCP,                 \
        .radio_uart_config = {                             \
            .port = 1,                                     \
            .uart_config =                                 \
                {                                          \
                    .baud_rate = 460800,                   \
                    .data_bits = UART_DATA_8_BITS,         \
                    .parity = UART_PARITY_DISABLE,         \
                    .stop_bits = UART_STOP_BITS_1,         \
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, \
                    .rx_flow_ctrl_thresh = 0,              \
                    .source_clk = UART_SCLK_DEFAULT,       \
                },                                         \
            .rx_pin = CONFIG_PIN_TO_RCP_TX,                \
            .tx_pin = CONFIG_PIN_TO_RCP_RX,                \
        },                                                 \
    }
#else
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()              \
    {                                                      \
        .radio_mode = RADIO_MODE_SPI_RCP,                  \
        .radio_spi_config = {                              \
            .host_device = SPI2_HOST,                      \
            .dma_channel = 2,                              \
            .spi_interface =                               \
                {                                          \
                    .mosi_io_num = CONFIG_PIN_TO_RCP_MOSI, \
                    .miso_io_num = CONFIG_PIN_TO_RCP_MISO, \
                    .sclk_io_num = CONFIG_PIN_TO_RCP_SCLK, \
                    .quadwp_io_num = -1,                   \
                    .quadhd_io_num = -1,                   \
                },                                         \
            .spi_device =                                  \
                {                                          \
                    .cs_ena_pretrans = 2,                  \
                    .input_delay_ns = 100,                 \
                    .mode = 0,                             \
                    .clock_speed_hz = 2500 * 1000,         \
                    .spics_io_num = CONFIG_PIN_TO_RCP_CS,  \
                    .queue_size = 5,                       \
                },                                         \
            .intr_pin = CONFIG_PIN_TO_RCP_BOOT,            \
        },                                                 \
    }
#endif /* radio mode */

/* No RCP on this board; empty config keeps the launch API happy. */
#define ESP_OPENTHREAD_RCP_UPDATE_CONFIG() \
    {                                      \
        0                                  \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()               \
    {                                                      \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE, \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
