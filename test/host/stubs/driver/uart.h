#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
typedef enum { UART_DATA_8_BITS = 3 } uart_word_length_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;
typedef enum { UART_SIGNAL_INV_DISABLE = 0, UART_SIGNAL_TXD_INV = 1 << 5 } uart_signal_inv_t;
typedef struct {
    int baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uart_sclk_t source_clk;
} uart_config_t;
#define UART_PIN_NO_CHANGE (-1)
esp_err_t uart_driver_install(uart_port_t p, int rx, int tx, int qsize, QueueHandle_t *q, int flags);
esp_err_t uart_param_config(uart_port_t p, const uart_config_t *cfg);
esp_err_t uart_set_pin(uart_port_t p, int tx, int rx, int rts, int cts);
esp_err_t uart_set_rx_timeout(uart_port_t p, int symbols);
esp_err_t uart_set_line_inverse(uart_port_t p, uint32_t mask);
esp_err_t uart_flush_input(uart_port_t p);
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t wait);
int uart_write_bytes(uart_port_t p, const void *src, size_t n);
int uart_read_bytes(uart_port_t p, void *buf, uint32_t n, TickType_t wait);
