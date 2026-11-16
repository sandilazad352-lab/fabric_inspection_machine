#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "rs232_reader.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt_mgr";

static SemaphoreHandle_t s_mqtt_mutex = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static bool s_ever_connected = false;
static char s_broker_uri[128];
static char s_lwt_topic[128];
static char s_lwt_msg[128];

static void handle_cmd_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }
    char cmd_buf[192];
    int copy_len = len < (int)sizeof(cmd_buf) - 1 ? len : (int)sizeof(cmd_buf) - 1;
    memcpy(cmd_buf, data, copy_len);
    cmd_buf[copy_len] = '\0';

    ESP_LOGI(TAG, "Received MQTT cmd: %s", cmd_buf);

    if (strstr(cmd_buf, "reboot") != NULL) {
        ESP_LOGW(TAG, "Reboot requested via MQTT cmd");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strstr(cmd_buf, "status") != NULL || strstr(cmd_buf, "ping") != NULL) {
        mqtt_manager_publish_status("online");
    } else if (strstr(cmd_buf, "test_uart") != NULL) {
        rs232_reader_inject_test_frame();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        bool is_reconn = s_ever_connected;
        s_ever_connected = true;
        s_connected = true;
        gateway_state_set_mqtt_status(true, NULL, is_reconn);
        ESP_LOGI(TAG, "Connected to MQTT broker");

        gw_config_t cfg;
        app_config_get(&cfg);
        if (cfg.topic_cmd[0] != '\0') {
            esp_mqtt_client_subscribe(event->client, cfg.topic_cmd, cfg.mqtt_qos);
            ESP_LOGI(TAG, "Subscribed cmd topic: %s", cfg.topic_cmd);
        }
        mqtt_manager_publish_status("online");
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        gateway_state_set_mqtt_status(false, "Disconnected from broker", false);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_cmd_payload(event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR: {
        char err_buf[96] = "MQTT transport/auth error";
        if (event->error_handle) {
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(err_buf, sizeof(err_buf), "Connection refused (0x%x)",
                         event->error_handle->connect_return_code);
            } else if (event->error_handle->esp_transport_sock_errno != 0) {
                snprintf(err_buf, sizeof(err_buf), "Socket errno %d",
                         event->error_handle->esp_transport_sock_errno);
            }
        }
        s_connected = false;
        gateway_state_set_mqtt_status(false, err_buf, false);
        ESP_LOGE(TAG, "%s", err_buf);
        break;
    }
    default:
        break;
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (s_connected) {
            mqtt_manager_publish_status("online");
        }
    }
}

esp_err_t mqtt_manager_reconfigure(void)
{
    xSemaphoreTake(s_mqtt_mutex, portMAX_DELAY);

    if (s_client) {
        s_connected = false;
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    gw_config_t cfg;
    app_config_get(&cfg);

    if (!cfg.mqtt_enabled || cfg.mqtt_host[0] == '\0') {
        gateway_state_set_mqtt_status(false, cfg.mqtt_enabled ? "No broker host set" : "MQTT disabled", false);
        xSemaphoreGive(s_mqtt_mutex);
        ESP_LOGI(TAG, "MQTT disabled or host empty");
        return ESP_OK;
    }

    if (strstr(cfg.mqtt_host, "://") != NULL) {
        app_config_safe_copy(s_broker_uri, sizeof(s_broker_uri), cfg.mqtt_host);
    } else {
        snprintf(s_broker_uri, sizeof(s_broker_uri), "mqtt://%s:%u", cfg.mqtt_host, (unsigned)cfg.mqtt_port);
    }

    app_config_safe_copy(s_lwt_topic, sizeof(s_lwt_topic), cfg.topic_status);
    snprintf(s_lwt_msg, sizeof(s_lwt_msg), "{\"status\":\"offline\",\"device_id\":\"%s\"}", cfg.device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_broker_uri,
        .credentials.client_id = cfg.mqtt_client_id[0] ? cfg.mqtt_client_id : cfg.device_id,
        .credentials.username = cfg.mqtt_user[0] ? cfg.mqtt_user : NULL,
        .credentials.authentication.password = cfg.mqtt_pass[0] ? cfg.mqtt_pass : NULL,
        .session.keepalive = cfg.mqtt_keepalive,
        .session.last_will.topic = s_lwt_topic[0] ? s_lwt_topic : NULL,
        .session.last_will.msg = s_lwt_topic[0] ? s_lwt_msg : NULL,
        .session.last_will.qos = cfg.mqtt_qos,
        .session.last_will.retain = true,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        gateway_state_set_mqtt_status(false, "Failed to init MQTT client", false);
        xSemaphoreGive(s_mqtt_mutex);
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        gateway_state_set_mqtt_status(false, "MQTT start failed", false);
    } else {
        ESP_LOGI(TAG, "MQTT client connecting to %s", s_broker_uri);
    }

    xSemaphoreGive(s_mqtt_mutex);
    return err;
}

esp_err_t mqtt_manager_init(void)
{
    if (!s_mqtt_mutex) {
        s_mqtt_mutex = xSemaphoreCreateMutex();
    }
    xTaskCreate(heartbeat_task, "mqtt_hb", 3072, NULL, 4, NULL);
    return mqtt_manager_reconfigure();
}

bool mqtt_manager_publish_status(const char *state_label)
{
    gw_config_t cfg;
    app_config_get(&cfg);
    if (!s_connected || !s_client || cfg.topic_status[0] == '\0') {
        return false;
    }

    gw_metrics_t m;
    gateway_state_get_metrics(&m);
    gw_wifi_status_t w;
    wifi_manager_get_status(&w);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", state_label ? state_label : "online");
    cJSON_AddStringToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "device_name", cfg.device_name);
    cJSON_AddStringToObject(root, "location", cfg.location);
    cJSON_AddStringToObject(root, "ip", w.sta_connected ? w.sta_ip : w.ap_ip);
    cJSON_AddNumberToObject(root, "rssi", (double)w.sta_rssi);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "total_frames", (double)m.total_frames);
    cJSON_AddNumberToObject(root, "total_rx_bytes", (double)m.total_rx_bytes);
    cJSON_AddNumberToObject(root, "mqtt_pub_ok", (double)m.mqtt_pub_ok);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return false;
    }

    int msg_id = esp_mqtt_client_publish(s_client, cfg.topic_status, json, 0, cfg.mqtt_qos, 1);
    cJSON_free(json);
    return msg_id >= 0;
}

bool mqtt_manager_publish_capture(const gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len)
{
    gw_config_t cfg;
    app_config_get(&cfg);
    if (!cfg.mqtt_enabled || !s_connected || !s_client || cfg.topic_hex[0] == '\0' || !raw_data || raw_len == 0) {
        ESP_LOGW(TAG, "Skip MQTT publish (enabled=%d, connected=%d, topic='%s', len=%zu)",
                 cfg.mqtt_enabled, s_connected, cfg.topic_hex, raw_len);
        return false;
    }

    int msg_id = -1;
    if (cfg.mqtt_format == GW_MQTT_FMT_RAW_BIN) {
        msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, (const char *)raw_data, (int)raw_len,
                                           cfg.mqtt_qos, cfg.mqtt_retain ? 1 : 0);
        if (msg_id < 0 && cfg.mqtt_qos > 0) {
            msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, (const char *)raw_data, (int)raw_len,
                                               0, cfg.mqtt_retain ? 1 : 0);
        }
        return msg_id >= 0;
    }

    static const char hex_lut[] = "0123456789ABCDEF";

    if (cfg.mqtt_format == GW_MQTT_FMT_RAW_HEX) {
        char *hex_str = (char *)malloc(raw_len * 2 + 1);
        if (!hex_str) {
            ESP_LOGE(TAG, "OOM allocating %zu-byte raw HEX buffer (free=%lu)",
                     raw_len * 2 + 1, (unsigned long)esp_get_free_heap_size());
            return false;
        }
        for (size_t i = 0; i < raw_len; i++) {
            hex_str[i * 2] = hex_lut[(raw_data[i] >> 4) & 0x0F];
            hex_str[i * 2 + 1] = hex_lut[raw_data[i] & 0x0F];
        }
        hex_str[raw_len * 2] = '\0';

        msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, hex_str, (int)(raw_len * 2),
                                           cfg.mqtt_qos, cfg.mqtt_retain ? 1 : 0);
        if (msg_id < 0 && cfg.mqtt_qos > 0) {
            ESP_LOGW(TAG, "QoS%u outbox full, retrying QoS0", (unsigned)cfg.mqtt_qos);
            msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, hex_str, (int)(raw_len * 2),
                                               0, cfg.mqtt_retain ? 1 : 0);
        }
        free(hex_str);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Published raw HEX frame #%lu (%zu bytes) to %s",
                     (unsigned long)meta->seq, raw_len, cfg.topic_hex);
        } else {
            ESP_LOGE(TAG, "Failed publishing raw HEX frame #%lu", (unsigned long)meta->seq);
        }
        return msg_id >= 0;
    }

    /* Single-buffer JSON+HEX envelope to minimize peak heap usage */
    char header[512];
    int hlen = snprintf(
        header, sizeof(header),
        "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"location\":\"%s\","
        "\"seq\":%lu,\"timestamp_ms\":%lld,\"bytes\":%zu,\"duration_ms\":%lu,"
        "\"rate_bps\":%lu,\"esc_k_count\":%zu,\"raster_stripes\":%zu,\"reason\":\"%s\","
        "\"commands\":{\"esc_at\":%s,\"esc_k\":%s,\"cr\":%s,\"lf\":%s,\"esc_l\":%s,\"esc_1\":%s},"
        "\"hex\":\"",
        cfg.device_id, cfg.device_name, cfg.location,
        (unsigned long)meta->seq, (long long)meta->timestamp_ms, meta->byte_len,
        (unsigned long)meta->duration_ms, (unsigned long)meta->rate_bps,
        meta->esc_k_count, meta->raster_stripes, meta->end_reason,
        meta->found_esc_at ? "true" : "false",
        meta->found_esc_k ? "true" : "false",
        meta->found_cr ? "true" : "false",
        meta->found_lf ? "true" : "false",
        meta->found_esc_l ? "true" : "false",
        meta->found_esc_1 ? "true" : "false"
    );
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) {
        ESP_LOGE(TAG, "JSON header overflow");
        return false;
    }

    size_t total_len = (size_t)hlen + (raw_len * 2) + 2; /* closing "} */
    char *payload = (char *)malloc(total_len + 1);
    if (!payload) {
        ESP_LOGE(TAG, "OOM allocating %zu-byte MQTT payload (free=%lu)",
                 total_len + 1, (unsigned long)esp_get_free_heap_size());
        return false;
    }

    memcpy(payload, header, (size_t)hlen);
    char *p = payload + hlen;
    for (size_t i = 0; i < raw_len; i++) {
        *p++ = hex_lut[(raw_data[i] >> 4) & 0x0F];
        *p++ = hex_lut[raw_data[i] & 0x0F];
    }
    *p++ = '"';
    *p++ = '}';
    *p = '\0';

    msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, payload, (int)total_len,
                                       cfg.mqtt_qos, cfg.mqtt_retain ? 1 : 0);
    if (msg_id < 0 && cfg.mqtt_qos > 0) {
        ESP_LOGW(TAG, "MQTT QoS%u publish failed (%zu bytes), retrying QoS0", (unsigned)cfg.mqtt_qos, total_len);
        msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, payload, (int)total_len,
                                           0, cfg.mqtt_retain ? 1 : 0);
    }
    free(payload);

    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published frame #%lu (%zu bytes, payload=%zu B) to %s",
                 (unsigned long)meta->seq, raw_len, total_len, cfg.topic_hex);
    } else {
        ESP_LOGE(TAG, "Failed publishing frame #%lu (%zu B) to %s",
                 (unsigned long)meta->seq, total_len, cfg.topic_hex);
    }
    return msg_id >= 0;
}

bool mqtt_manager_publish_test(void)
{
    gw_config_t cfg;
    app_config_get(&cfg);
    if (!s_connected || !s_client || cfg.topic_hex[0] == '\0') {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "gateway_test");
    cJSON_AddStringToObject(root, "device_id", cfg.device_id);
    cJSON_AddStringToObject(root, "device_name", cfg.device_name);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        return false;
    }

    int msg_id = esp_mqtt_client_publish(s_client, cfg.topic_hex, payload, 0, cfg.mqtt_qos, 0);
    cJSON_free(payload);
    return msg_id >= 0;
}
