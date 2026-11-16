#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MAX_CAPTURES_HISTORY  8
#define GW_LATEST_RAW_MAX_BYTES  32768
#define GW_LOG_RING_LINES        80
#define GW_LOG_LINE_MAX          160

typedef struct {
    uint32_t seq;
    int64_t timestamp_ms;
    size_t byte_len;
    uint32_t duration_ms;
    uint32_t rate_bps;
    bool found_esc_at;
    bool found_esc_k;
    bool found_cr;
    bool found_lf;
    bool found_esc_l;
    bool found_esc_1;
    size_t esc_k_count;
    size_t raster_stripes;
    char end_reason[16];
    bool mqtt_published;
} gw_capture_meta_t;

typedef struct {
    uint32_t seq;
    int64_t timestamp_ms;
    char text[GW_LOG_LINE_MAX];
} gw_log_entry_t;

typedef struct {
    uint32_t total_frames;
    uint64_t total_rx_bytes;
    uint32_t uart_errors;
    uint32_t mqtt_pub_ok;
    uint32_t mqtt_pub_err;
    uint32_t mqtt_reconnects;
    bool mqtt_connected;
    char mqtt_last_err[96];
    int64_t last_capture_ms;
    int64_t last_mqtt_pub_ms;
} gw_metrics_t;

void gateway_state_init(void);
void gateway_state_record_capture(const gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len);
void gateway_state_mark_latest_mqtt(uint32_t seq, bool ok);
size_t gateway_state_get_history(gw_capture_meta_t *out, size_t max_items);
bool gateway_state_get_latest(gw_capture_meta_t *meta_out, uint8_t *raw_out, size_t raw_cap, size_t *raw_len_out);

void gateway_state_set_mqtt_status(bool connected, const char *err_msg, bool is_reconnect);
void gateway_state_inc_uart_error(void);
void gateway_state_get_metrics(gw_metrics_t *out);

void gateway_state_push_log(const char *line);
size_t gateway_state_get_logs(uint32_t since_seq, gw_log_entry_t *out, size_t max_items);
void gateway_state_clear_logs(void);

#ifdef __cplusplus
}
#endif
