#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE   "gw_cfg"
#define NVS_KEY_BLOB    "config_v2"
#define CONFIG_MAGIC    0x46494757U /* "FIGW" */

typedef struct {
    uint32_t magic;
    uint32_t version;
    gw_config_t cfg;
} persisted_config_t;

static SemaphoreHandle_t s_cfg_mutex = NULL;
static gw_config_t s_cfg;

void app_config_safe_copy(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void load_defaults(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    app_config_safe_copy(cfg->device_name, sizeof(cfg->device_name), "FI-Inspection-Gateway");
    snprintf(cfg->device_id, sizeof(cfg->device_id), "fi-gw-%02x%02x%02x", mac[3], mac[4], mac[5]);
    app_config_safe_copy(cfg->location, sizeof(cfg->location), "Inspection-Line-1");

    app_config_safe_copy(cfg->admin_user, sizeof(cfg->admin_user), "admin");
    app_config_safe_copy(cfg->admin_pass, sizeof(cfg->admin_pass), "admin1234");

    cfg->wifi_mode = GW_WIFI_AP_STA;
    app_config_safe_copy(cfg->sta_ssid, sizeof(cfg->sta_ssid), "");
    app_config_safe_copy(cfg->sta_pass, sizeof(cfg->sta_pass), "");
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "FI-GW-%02X%02X%02X", mac[3], mac[4], mac[5]);
    app_config_safe_copy(cfg->ap_pass, sizeof(cfg->ap_pass), "gateway1234");

    cfg->uart_baud = 9600;
    cfg->uart_tx_pin = 4;
    cfg->uart_rx_pin = 5;
    cfg->uart_data_bits = 8;
    cfg->uart_parity = 0;
    cfg->uart_stop_bits = 1;
    cfg->idle_timeout_ms = 2000;
    cfg->max_capture_bytes = 32768;
    cfg->auto_publish_mqtt = true;
    cfg->serial_echo_hex = true;
    cfg->verbose_debug = false;

    cfg->mqtt_enabled = false;
    app_config_safe_copy(cfg->mqtt_host, sizeof(cfg->mqtt_host), "192.168.1.100");
    cfg->mqtt_port = 1883;
    snprintf(cfg->mqtt_client_id, sizeof(cfg->mqtt_client_id), "fi-gw-%02x%02x%02x", mac[3], mac[4], mac[5]);
    app_config_safe_copy(cfg->mqtt_user, sizeof(cfg->mqtt_user), "");
    app_config_safe_copy(cfg->mqtt_pass, sizeof(cfg->mqtt_pass), "");
    cfg->mqtt_keepalive = 60;
    cfg->mqtt_qos = 1;
    cfg->mqtt_retain = false;
    cfg->mqtt_format = GW_MQTT_FMT_JSON_HEX;
    cfg->mqtt_debug_stream = false;

    snprintf(cfg->topic_hex, sizeof(cfg->topic_hex), "factory/inspection/%s/hex", cfg->device_id);
    snprintf(cfg->topic_status, sizeof(cfg->topic_status), "factory/inspection/%s/status", cfg->device_id);
    snprintf(cfg->topic_cmd, sizeof(cfg->topic_cmd), "factory/inspection/%s/cmd", cfg->device_id);
    snprintf(cfg->topic_debug, sizeof(cfg->topic_debug), "factory/inspection/%s/debug", cfg->device_id);
    snprintf(cfg->topic_ack, sizeof(cfg->topic_ack), "factory/inspection/%s/ack", cfg->device_id);

    cfg->require_server_ack = true;
    cfg->ack_timeout_ms = 10000;
    cfg->max_spool_files = 100;
}

static void clamp_config(gw_config_t *cfg)
{
    if (cfg->wifi_mode > GW_WIFI_AP_ONLY) {
        cfg->wifi_mode = GW_WIFI_AP_STA;
    }
    if (cfg->uart_baud < 1200 || cfg->uart_baud > 921600) {
        cfg->uart_baud = 9600;
    }
    if (cfg->uart_tx_pin < 0 || cfg->uart_tx_pin > 28) {
        cfg->uart_tx_pin = 4;
    }
    if (cfg->uart_rx_pin < 0 || cfg->uart_rx_pin > 28 || cfg->uart_rx_pin == cfg->uart_tx_pin) {
        cfg->uart_rx_pin = (cfg->uart_tx_pin == 5) ? 4 : 5;
    }
    if (cfg->uart_data_bits != 7 && cfg->uart_data_bits != 8) {
        cfg->uart_data_bits = 8;
    }
    if (cfg->uart_parity > 2) {
        cfg->uart_parity = 0;
    }
    if (cfg->uart_stop_bits != 1 && cfg->uart_stop_bits != 2) {
        cfg->uart_stop_bits = 1;
    }
    if (cfg->idle_timeout_ms < 200 || cfg->idle_timeout_ms > 30000) {
        cfg->idle_timeout_ms = 2000;
    }
    if (cfg->max_capture_bytes < 512 || cfg->max_capture_bytes > 32768) {
        cfg->max_capture_bytes = 32768;
    }
    if (cfg->mqtt_port == 0) {
        cfg->mqtt_port = 1883;
    }
    if (cfg->mqtt_keepalive < 10) {
        cfg->mqtt_keepalive = 60;
    }
    if (cfg->mqtt_qos > 2) {
        cfg->mqtt_qos = 1;
    }
    if (cfg->mqtt_format > GW_MQTT_FMT_RAW_BIN) {
        cfg->mqtt_format = GW_MQTT_FMT_JSON_HEX;
    }
    if (cfg->admin_user[0] == '\0') {
        app_config_safe_copy(cfg->admin_user, sizeof(cfg->admin_user), "admin");
    }
    if (cfg->admin_pass[0] == '\0') {
        app_config_safe_copy(cfg->admin_pass, sizeof(cfg->admin_pass), "admin1234");
    }
    if (cfg->ap_ssid[0] == '\0') {
        app_config_safe_copy(cfg->ap_ssid, sizeof(cfg->ap_ssid), "FI-Gateway-Setup");
    }
    if (cfg->ack_timeout_ms < 1000 || cfg->ack_timeout_ms > 120000) {
        cfg->ack_timeout_ms = 10000;
    }
    if (cfg->max_spool_files < 5 || cfg->max_spool_files > 120) {
        cfg->max_spool_files = 100;
    }
}

static esp_err_t save_to_nvs_locked(const gw_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    persisted_config_t blob = {
        .magic = CONFIG_MAGIC,
        .version = 2,
        .cfg = *cfg,
    };

    err = nvs_set_blob(handle, NVS_KEY_BLOB, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_config_init(void)
{
    if (!s_cfg_mutex) {
        s_cfg_mutex = xSemaphoreCreateMutex();
    }

    load_defaults(&s_cfg);

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        persisted_config_t blob;
        size_t sz = sizeof(blob);
        if (nvs_get_blob(handle, NVS_KEY_BLOB, &blob, &sz) == ESP_OK &&
            sz == sizeof(blob) &&
            blob.magic == CONFIG_MAGIC &&
            blob.version == 2) {
            s_cfg = blob.cfg;
            clamp_config(&s_cfg);
        } else {
            /* Migrate v1 config prefix if present so existing Wi-Fi/MQTT settings are preserved */
            persisted_config_t v1_blob = {.magic = 0, .version = 0, .cfg = s_cfg};
            size_t v1_sz = sizeof(v1_blob);
            if (nvs_get_blob(handle, "config_v1", &v1_blob, &v1_sz) == ESP_OK &&
                v1_sz > 8 && v1_sz <= sizeof(v1_blob) &&
                v1_blob.magic == CONFIG_MAGIC &&
                v1_blob.version == 1) {
                s_cfg = v1_blob.cfg;
                clamp_config(&s_cfg);
                save_to_nvs_locked(&s_cfg);
            }
        }
        nvs_close(handle);
    }

    return ESP_OK;
}

void app_config_get(gw_config_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t app_config_set(const gw_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_config_t updated = *in;
    clamp_config(&updated);

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    esp_err_t err = save_to_nvs_locked(&updated);
    if (err == ESP_OK) {
        s_cfg = updated;
    }
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_factory_reset(void)
{
    gw_config_t def;
    load_defaults(&def);
    return app_config_set(&def);
}
