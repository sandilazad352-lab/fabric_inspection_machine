#include "rs232_reader.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gateway_state.h"
#include "mqtt_manager.h"

#define RS232_UART_PORT     UART_NUM_1
#define UART_RX_BUF_SIZE    4096
#define READ_CHUNK_SIZE     256
#define READ_TIMEOUT_MS     100
#define HEX_LINE_CHARS      80

static const char *TAG = "rs232_rx";

static SemaphoreHandle_t s_uart_mutex = NULL;
static bool s_driver_installed = false;

typedef struct {
    bool active;
    size_t col_chars;
    size_t total_bytes;
    int64_t start_us;
    int64_t last_rx_us;

    bool found_esc_at;
    bool found_esc_k;
    bool found_cr;
    bool found_lf;
    bool found_esc_l;
    bool found_esc_1;
    size_t esc_k_count;
    size_t raster_stripes;

    uint8_t window[4];
    size_t window_fill;
    uint8_t *raw_buf;
    size_t raw_cap;
} rx_capture_t;

static void analyze_byte(rx_capture_t *st, uint8_t b)
{
    if (st->raw_buf && st->total_bytes < st->raw_cap) {
        st->raw_buf[st->total_bytes] = b;
    }

    if (b == 0x0D) {
        st->found_cr = true;
    } else if (b == 0x0A) {
        st->found_lf = true;
    }

    if (st->window_fill < 4) {
        st->window[st->window_fill++] = b;
    } else {
        st->window[0] = st->window[1];
        st->window[1] = st->window[2];
        st->window[2] = st->window[3];
        st->window[3] = b;
    }

    if (st->window_fill >= 2) {
        uint8_t prev = st->window[st->window_fill - 2];
        uint8_t curr = st->window[st->window_fill - 1];
        if (prev == 0x1B) {
            if (curr == 0x40) {
                st->found_esc_at = true;
            } else if (curr == 0x4B) {
                st->found_esc_k = true;
                st->esc_k_count++;
            } else if (curr == 0x6C) {
                st->found_esc_l = true;
            } else if (curr == 0x31) {
                st->found_esc_1 = true;
            }
        }
    }

    if (st->window_fill == 4 &&
        st->window[0] == 0x1B &&
        st->window[1] == 0x4B &&
        st->window[2] == 0xF0 &&
        st->window[3] == 0x00) {
        st->raster_stripes++;
    }
}

static void reset_capture(rx_capture_t *st, int64_t now_us)
{
    uint8_t *buf = st->raw_buf;
    size_t cap = st->raw_cap;
    memset(st, 0, sizeof(*st));
    st->raw_buf = buf;
    st->raw_cap = cap;
    if (!st->raw_buf) {
        st->raw_cap = GW_LATEST_RAW_MAX_BYTES;
        st->raw_buf = (uint8_t *)malloc(st->raw_cap);
        if (!st->raw_buf) {
            ESP_LOGE(TAG, "Failed to allocate %zu-byte RX capture buffer", st->raw_cap);
            st->raw_cap = 0;
        }
    }
    st->active = true;
    st->start_us = now_us;
    st->last_rx_us = now_us;
}

static void finalize_capture(rx_capture_t *st, const gw_config_t *cfg, const char *reason)
{
    if (cfg->serial_echo_hex) {
        if (st->col_chars > 0) {
            putchar('\n');
        }
        printf("--- CAPTURE END (%zu bytes, reason: %s) ---\n", st->total_bytes, reason);
        fflush(stdout);
    }

    uint32_t dur_ms = (uint32_t)((st->last_rx_us - st->start_us) / 1000LL);
    uint32_t rate_bps = (dur_ms > 0) ? (uint32_t)((st->total_bytes * 1000ULL) / dur_ms) : (uint32_t)st->total_bytes;

    gw_capture_meta_t meta = {
        .seq = 0,
        .timestamp_ms = esp_timer_get_time() / 1000LL,
        .byte_len = st->total_bytes,
        .duration_ms = dur_ms,
        .rate_bps = rate_bps,
        .found_esc_at = st->found_esc_at,
        .found_esc_k = st->found_esc_k,
        .found_cr = st->found_cr,
        .found_lf = st->found_lf,
        .found_esc_l = st->found_esc_l,
        .found_esc_1 = st->found_esc_1,
        .esc_k_count = st->esc_k_count,
        .raster_stripes = st->raster_stripes,
        .mqtt_published = false,
    };
    app_config_safe_copy(meta.end_reason, sizeof(meta.end_reason), reason);

    size_t stored_len = st->total_bytes < st->raw_cap ? st->total_bytes : st->raw_cap;
    gateway_state_record_capture(&meta, st->raw_buf, stored_len);

    /* Free RX scratch buffer before MQTT publish so max heap is available */
    if (st->raw_buf) {
        free(st->raw_buf);
        st->raw_buf = NULL;
        st->raw_cap = 0;
    }

    gw_capture_meta_t saved_meta = {0};
    gateway_state_get_latest(&saved_meta, NULL, 0, NULL);

    ESP_LOGI(TAG, "Frame #%lu captured: %zu bytes (%lu ms, ESC_K=%zu, stripes=%zu, reason=%s)",
             (unsigned long)saved_meta.seq, st->total_bytes, (unsigned long)dur_ms,
             st->esc_k_count, st->raster_stripes, reason);

    if (cfg->auto_publish_mqtt && cfg->mqtt_enabled && stored_len > 0) {
        uint8_t *pub_buf = (uint8_t *)malloc(stored_len);
        size_t got_len = 0;
        if (pub_buf && gateway_state_get_latest(NULL, pub_buf, stored_len, &got_len) && got_len > 0) {
            bool ok = mqtt_manager_publish_capture(&saved_meta, pub_buf, got_len);
            gateway_state_mark_latest_mqtt(saved_meta.seq, ok);
        } else {
            ESP_LOGE(TAG, "OOM copying %zu bytes for MQTT publish", stored_len);
            gateway_state_mark_latest_mqtt(saved_meta.seq, false);
        }
        free(pub_buf);
    }

    st->active = false;
}

esp_err_t rs232_reader_reconfigure(void)
{
    gw_config_t cfg;
    app_config_get(&cfg);

    uart_word_length_t data_bits = (cfg.uart_data_bits == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    if (cfg.uart_parity == 1) {
        parity = UART_PARITY_ODD;
    } else if (cfg.uart_parity == 2) {
        parity = UART_PARITY_EVEN;
    }
    uart_stop_bits_t stop_bits = (cfg.uart_stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    const uart_config_t uart_cfg = {
        .baud_rate = cfg.uart_baud,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    if (!s_driver_installed) {
        ESP_ERROR_CHECK(uart_driver_install(RS232_UART_PORT, UART_RX_BUF_SIZE, 0, 0, NULL, 0));
        s_driver_installed = true;
    }
    ESP_ERROR_CHECK(uart_param_config(RS232_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS232_UART_PORT, cfg.uart_tx_pin, cfg.uart_rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(RS232_UART_PORT);
    xSemaphoreGive(s_uart_mutex);

    ESP_LOGI(TAG, "UART1 configured: TX=GPIO%ld, RX=GPIO%ld @ %ld %u%c%u",
             (long)cfg.uart_tx_pin, (long)cfg.uart_rx_pin, (long)cfg.uart_baud,
             (unsigned)cfg.uart_data_bits,
             cfg.uart_parity == 1 ? 'O' : (cfg.uart_parity == 2 ? 'E' : 'N'),
             (unsigned)cfg.uart_stop_bits);
    return ESP_OK;
}

static void rs232_task(void *arg)
{
    (void)arg;
    static const char hex_lut[] = "0123456789ABCDEF";
    uint8_t rx_buf[READ_CHUNK_SIZE];
    char out_buf[READ_CHUNK_SIZE * 2 + (READ_CHUNK_SIZE * 2 / HEX_LINE_CHARS) + 4];

    rx_capture_t st = {0};

    while (1) {
        gw_config_t cfg;
        app_config_get(&cfg);

        int len = 0;
        if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            len = uart_read_bytes(RS232_UART_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(READ_TIMEOUT_MS));
            xSemaphoreGive(s_uart_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t now_us = esp_timer_get_time();

        if (len > 0) {
            if (!st.active) {
                reset_capture(&st, now_us);
                if (cfg.serial_echo_hex) {
                    printf("\n--- CAPTURE START ---\n");
                }
            }

            size_t out_pos = 0;
            for (int i = 0; i < len; i++) {
                uint8_t b = rx_buf[i];
                analyze_byte(&st, b);
                if (cfg.serial_echo_hex) {
                    out_buf[out_pos++] = hex_lut[(b >> 4) & 0x0F];
                    out_buf[out_pos++] = hex_lut[b & 0x0F];
                    st.col_chars += 2;
                    if (st.col_chars >= HEX_LINE_CHARS) {
                        out_buf[out_pos++] = '\n';
                        st.col_chars = 0;
                    }
                }
                st.total_bytes++;
            }
            if (cfg.serial_echo_hex && out_pos > 0) {
                fwrite(out_buf, 1, out_pos, stdout);
                fflush(stdout);
            }

            st.last_rx_us = now_us;
            if (st.total_bytes >= cfg.max_capture_bytes) {
                finalize_capture(&st, &cfg, "max_bytes");
            }
        } else if (st.active) {
            if ((now_us - st.last_rx_us) >= ((int64_t)cfg.idle_timeout_ms * 1000LL)) {
                finalize_capture(&st, &cfg, "idle_gap");
            }
        }
    }
}

void rs232_reader_inject_test_frame(void)
{
    gw_config_t cfg;
    app_config_get(&cfg);

    /* Synthetic ESC/P 240-byte stripe receipt ("FI-GW TEST" pattern) */
    const size_t stripe_bytes = 240;
    const size_t stripes = 4;
    const size_t total = 5 + stripes * (6 + stripe_bytes) + 4;
    uint8_t *buf = (uint8_t *)calloc(1, total);
    if (!buf) {
        return;
    }

    size_t pos = 0;
    buf[pos++] = 0x1B; buf[pos++] = 0x40; /* ESC @ */
    buf[pos++] = 0x1B; buf[pos++] = 0x31; buf[pos++] = 0x00; /* ESC 1 0 */

    for (size_t s = 0; s < stripes; s++) {
        buf[pos++] = 0x0D; buf[pos++] = 0x0A;
        buf[pos++] = 0x1B; buf[pos++] = 0x4B; buf[pos++] = 0xF0; buf[pos++] = 0x00;
        for (size_t x = 0; x < stripe_bytes; x++) {
            /* Inverted bitmap bits: 0 = ink, 1 = paper */
            if (s == 0 || s == stripes - 1) {
                buf[pos++] = (x % 8 < 4) ? 0xFE : 0xFF;
            } else {
                buf[pos++] = ((x >= 40 && x <= 200) && ((x + s * 7) % 16 < 6)) ? 0x81 : 0xFF;
            }
        }
    }
    buf[pos++] = 0x0D; buf[pos++] = 0x0A;
    buf[pos++] = 0x1B; buf[pos++] = 0x31; buf[pos++] = 0x03;

    rx_capture_t st = {0};
    st.raw_cap = pos;
    st.raw_buf = buf;
    reset_capture(&st, esp_timer_get_time());
    for (size_t i = 0; i < pos; i++) {
        analyze_byte(&st, buf[i]);
        st.total_bytes++;
    }
    st.last_rx_us = st.start_us + 25000;
    finalize_capture(&st, &cfg, "sim_test");
    free(buf);
}

esp_err_t rs232_reader_init(void)
{
    if (!s_uart_mutex) {
        s_uart_mutex = xSemaphoreCreateMutex();
    }
    ESP_ERROR_CHECK(rs232_reader_reconfigure());
    xTaskCreate(rs232_task, "rs232_task", 5120, NULL, 10, NULL);
    return ESP_OK;
}
