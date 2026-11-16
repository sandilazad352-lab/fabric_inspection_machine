#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_WIFI_AP_STA = 0,
    GW_WIFI_STA_ONLY = 1,
    GW_WIFI_AP_ONLY = 2,
} gw_wifi_mode_t;

typedef enum {
    GW_MQTT_FMT_JSON_HEX = 0,
    GW_MQTT_FMT_RAW_HEX = 1,
    GW_MQTT_FMT_RAW_BIN = 2,
} gw_mqtt_fmt_t;

typedef struct {
    /* Device identity */
    char device_name[48];
    char device_id[48];
    char location[64];

    /* Admin credentials */
    char admin_user[32];
    char admin_pass[64];

    /* Wi-Fi settings */
    uint8_t wifi_mode;      /* gw_wifi_mode_t */
    char sta_ssid[33];
    char sta_pass[65];
    char ap_ssid[33];
    char ap_pass[65];

    /* RS-232 / UART1 settings */
    int32_t uart_baud;
    int32_t uart_tx_pin;
    int32_t uart_rx_pin;
    uint8_t uart_data_bits; /* 7 or 8 */
    uint8_t uart_parity;    /* 0=none, 1=odd, 2=even */
    uint8_t uart_stop_bits; /* 1 or 2 */
    uint32_t idle_timeout_ms;
    uint32_t max_capture_bytes;
    bool auto_publish_mqtt;
    bool serial_echo_hex;
    bool verbose_debug;

    /* MQTT broker & topics */
    bool mqtt_enabled;
    char mqtt_host[96];
    uint16_t mqtt_port;
    char mqtt_client_id[48];
    char mqtt_user[48];
    char mqtt_pass[64];
    uint16_t mqtt_keepalive;
    uint8_t mqtt_qos;
    bool mqtt_retain;
    uint8_t mqtt_format;    /* gw_mqtt_fmt_t */
    bool mqtt_debug_stream;

    char topic_hex[128];
    char topic_status[128];
    char topic_cmd[128];
    char topic_debug[128];
} gw_config_t;

void app_config_safe_copy(char *dst, size_t dst_sz, const char *src);
esp_err_t app_config_init(void);
void app_config_get(gw_config_t *out);
esp_err_t app_config_set(const gw_config_t *in);
esp_err_t app_config_factory_reset(void);

#ifdef __cplusplus
}
#endif
