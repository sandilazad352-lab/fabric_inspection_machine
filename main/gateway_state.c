#include "gateway_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_state_mutex = NULL;
static SemaphoreHandle_t s_log_mutex = NULL;

static gw_metrics_t s_metrics = {0};
static gw_capture_meta_t s_history[GW_MAX_CAPTURES_HISTORY];
static size_t s_history_count = 0;

static uint8_t *s_latest_raw = NULL;
static size_t s_latest_raw_len = 0;
static gw_capture_meta_t s_latest_meta = {0};
static bool s_has_latest = false;

static gw_log_entry_t s_logs[GW_LOG_RING_LINES];
static size_t s_log_head = 0;
static size_t s_log_count = 0;
static uint32_t s_log_seq = 0;
static vprintf_like_t s_orig_vprintf = NULL;

static void strip_ansi_and_copy(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; i++) {
        if (src[i] == '\x1b' && src[i + 1] == '[') {
            i += 2;
            while (src[i] != '\0' && (src[i] < '@' || src[i] > '~')) {
                i++;
            }
            continue;
        }
        if (src[i] == '\r' || src[i] == '\n') {
            continue;
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

static int gateway_vprintf_hook(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);

    if (!xPortInIsrContext() && s_log_mutex) {
        char buf[GW_LOG_LINE_MAX + 32];
        vsnprintf(buf, sizeof(buf), fmt, args_copy);
        gateway_state_push_log(buf);
    }

    va_end(args_copy);
    return ret;
}

void gateway_state_init(void)
{
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    if (!s_latest_raw) {
        s_latest_raw = (uint8_t *)malloc(GW_LATEST_RAW_MAX_BYTES);
    }
    s_orig_vprintf = esp_log_set_vprintf(gateway_vprintf_hook);
}

void gateway_state_push_log(const char *line)
{
    if (!line || !s_log_mutex) {
        return;
    }

    char clean[GW_LOG_LINE_MAX];
    strip_ansi_and_copy(clean, sizeof(clean), line);
    if (clean[0] == '\0') {
        return;
    }

    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    size_t idx = (s_log_head + s_log_count) % GW_LOG_RING_LINES;
    if (s_log_count == GW_LOG_RING_LINES) {
        idx = s_log_head;
        s_log_head = (s_log_head + 1) % GW_LOG_RING_LINES;
    } else {
        s_log_count++;
    }

    s_logs[idx].seq = ++s_log_seq;
    s_logs[idx].timestamp_ms = esp_timer_get_time() / 1000;
    snprintf(s_logs[idx].text, sizeof(s_logs[idx].text), "%s", clean);

    xSemaphoreGive(s_log_mutex);
}

size_t gateway_state_get_logs(uint32_t since_seq, gw_log_entry_t *out, size_t max_items)
{
    if (!out || max_items == 0 || !s_log_mutex) {
        return 0;
    }

    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    size_t copied = 0;
    for (size_t i = 0; i < s_log_count && copied < max_items; i++) {
        size_t idx = (s_log_head + i) % GW_LOG_RING_LINES;
        if (s_logs[idx].seq > since_seq) {
            out[copied++] = s_logs[idx];
        }
    }
    xSemaphoreGive(s_log_mutex);
    return copied;
}

void gateway_state_clear_logs(void)
{
    if (!s_log_mutex) {
        return;
    }
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    s_log_head = 0;
    s_log_count = 0;
    xSemaphoreGive(s_log_mutex);
}

void gateway_state_record_capture(const gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len)
{
    if (!meta || !s_state_mutex) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    s_metrics.total_frames++;
    s_metrics.total_rx_bytes += meta->byte_len;
    s_metrics.last_capture_ms = meta->timestamp_ms;

    gw_capture_meta_t stored = *meta;
    stored.seq = s_metrics.total_frames;

    if (s_history_count < GW_MAX_CAPTURES_HISTORY) {
        for (size_t i = s_history_count; i > 0; i--) {
            s_history[i] = s_history[i - 1];
        }
        s_history[0] = stored;
        s_history_count++;
    } else {
        for (size_t i = GW_MAX_CAPTURES_HISTORY - 1; i > 0; i--) {
            s_history[i] = s_history[i - 1];
        }
        s_history[0] = stored;
    }

    s_latest_meta = stored;
    s_has_latest = true;

    if (s_latest_raw && raw_data && raw_len > 0) {
        size_t copy_len = raw_len > GW_LATEST_RAW_MAX_BYTES ? GW_LATEST_RAW_MAX_BYTES : raw_len;
        memcpy(s_latest_raw, raw_data, copy_len);
        s_latest_raw_len = copy_len;
    } else {
        s_latest_raw_len = 0;
    }

    xSemaphoreGive(s_state_mutex);
}

void gateway_state_mark_latest_mqtt(uint32_t seq, bool ok)
{
    if (!s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (ok) {
        s_metrics.mqtt_pub_ok++;
        s_metrics.last_mqtt_pub_ms = esp_timer_get_time() / 1000;
    } else {
        s_metrics.mqtt_pub_err++;
    }
    if (s_has_latest && s_latest_meta.seq == seq) {
        s_latest_meta.mqtt_published = ok;
    }
    for (size_t i = 0; i < s_history_count; i++) {
        if (s_history[i].seq == seq) {
            s_history[i].mqtt_published = ok;
            break;
        }
    }
    xSemaphoreGive(s_state_mutex);
}

size_t gateway_state_get_history(gw_capture_meta_t *out, size_t max_items)
{
    if (!out || max_items == 0 || !s_state_mutex) {
        return 0;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    size_t count = s_history_count < max_items ? s_history_count : max_items;
    memcpy(out, s_history, count * sizeof(gw_capture_meta_t));
    xSemaphoreGive(s_state_mutex);
    return count;
}

bool gateway_state_get_latest(gw_capture_meta_t *meta_out, uint8_t *raw_out, size_t raw_cap, size_t *raw_len_out)
{
    if (!s_state_mutex) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_has_latest) {
        xSemaphoreGive(s_state_mutex);
        return false;
    }
    if (meta_out) {
        *meta_out = s_latest_meta;
    }
    if (raw_len_out) {
        *raw_len_out = s_latest_raw_len;
    }
    if (raw_out && raw_cap > 0 && s_latest_raw && s_latest_raw_len > 0) {
        size_t n = s_latest_raw_len < raw_cap ? s_latest_raw_len : raw_cap;
        memcpy(raw_out, s_latest_raw, n);
        if (raw_len_out) {
            *raw_len_out = n;
        }
    }
    xSemaphoreGive(s_state_mutex);
    return true;
}

void gateway_state_set_mqtt_status(bool connected, const char *err_msg, bool is_reconnect)
{
    if (!s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_metrics.mqtt_connected = connected;
    if (is_reconnect) {
        s_metrics.mqtt_reconnects++;
    }
    if (err_msg) {
        snprintf(s_metrics.mqtt_last_err, sizeof(s_metrics.mqtt_last_err), "%s", err_msg);
    } else if (connected) {
        s_metrics.mqtt_last_err[0] = '\0';
    }
    xSemaphoreGive(s_state_mutex);
}

void gateway_state_inc_uart_error(void)
{
    if (!s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_metrics.uart_errors++;
    xSemaphoreGive(s_state_mutex);
}

void gateway_state_get_metrics(gw_metrics_t *out)
{
    if (!out || !s_state_mutex) {
        return;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_metrics;
    xSemaphoreGive(s_state_mutex);
}
