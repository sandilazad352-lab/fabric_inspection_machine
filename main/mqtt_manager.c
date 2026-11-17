#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "ota_manager.h"
#include "rs232_reader.h"
#include "spool_manager.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt_mgr";

static SemaphoreHandle_t s_mqtt_mutex = NULL;
static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static bool s_ever_connected = false;
static char s_broker_uri[128];
static char s_lwt_topic[128];
static char s_lwt_msg[128];

static void handle_ack_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }
    char buf[256];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *obj = cJSON_Parse(buf);
    if (!obj) {
        ESP_LOGW(TAG, "Invalid JSON on ACK topic: %s", buf);
        return;
    }

    uint32_t seq = 0;
    const cJSON *jseq = cJSON_GetObjectItem(obj, "seq");
    if (!cJSON_IsNumber(jseq)) {
        jseq = cJSON_GetObjectItem(obj, "msg_id");
    }
    if (cJSON_IsNumber(jseq) && jseq->valuedouble > 0) {
        seq = (uint32_t)jseq->valuedouble;
    }

    bool ok = false;
    const char *reason = NULL;
    const cJSON *jstat = cJSON_GetObjectItem(obj, "status");
    const cJSON *jack = cJSON_GetObjectItem(obj, "ack");
    const cJSON *jreason = cJSON_GetObjectItem(obj, "reason");
    if (cJSON_IsString(jreason)) {
        reason = jreason->valuestring;
    }

    if (cJSON_IsBool(jack)) {
        ok = cJSON_IsTrue(jack);
    } else if (cJSON_IsString(jstat)) {
        const char *st = jstat->valuestring;
        if (strcmp(st, "ok") == 0 || strcmp(st, "ack") == 0 || strcmp(st, "received") == 0) {
            ok = true;
        } else {
            ok = false;
            if (!reason) {
                reason = st;
            }
        }
    }

    bool has_crc = false;
    uint32_t server_crc = 0;
    const cJSON *jcrc = cJSON_GetObjectItem(obj, "crc32");
    if (cJSON_IsString(jcrc) && jcrc->valuestring[0] != '\0') {
        server_crc = (uint32_t)strtoul(jcrc->valuestring, NULL, 16);
        has_crc = true;
    } else if (cJSON_IsNumber(jcrc)) {
        server_crc = (uint32_t)jcrc->valuedouble;
        has_crc = true;
    }

    if (seq > 0) {
        spool_manager_on_server_ack(seq, ok, has_crc, server_crc, reason);
    } else {
        ESP_LOGW(TAG, "ACK missing valid seq/msg_id: %s", buf);
    }
    cJSON_Delete(obj);
}

static void handle_cmd_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }
    char cmd_buf[384];
    int copy_len = len < (int)sizeof(cmd_buf) - 1 ? len : (int)sizeof(cmd_buf) - 1;
    memcpy(cmd_buf, data, copy_len);
    cmd_buf[copy_len] = '\0';

    ESP_LOGI(TAG, "Received MQTT cmd: %s", cmd_buf);

    if (strncmp(cmd_buf, "ota:", 4) == 0) {
        ota_manager_start_url(cmd_buf + 4);
        return;
    }
    if (cmd_buf[0] == '{') {
        cJSON *obj = cJSON_Parse(cmd_buf);
        if (obj) {
            const cJSON *cmd = cJSON_GetObjectItem(obj, "cmd");
            const cJSON *url = cJSON_GetObjectItem(obj, "url");
            if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, "ota") == 0 && cJSON_IsString(url)) {
                ota_manager_start_url(url->valuestring);
                cJSON_Delete(obj);
                return;
            }
            cJSON_Delete(obj);
        }
    }

    if (strstr(cmd_buf, "reboot") != NULL) {
        ESP_LOGW(TAG, "Reboot requested via MQTT cmd");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strstr(cmd_buf, "status") != NULL || strstr(cmd_buf, "ping") != NULL) {
        mqtt_manager_publish_status("online");
    } else if (strstr(cmd_buf, "test_uart") != NULL) {
        rs232_reader_inject_test_frame();
    } else if (strstr(cmd_buf, "sync") != NULL) {
        spool_manager_trigger_sync();
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

        gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
        if (cfg) {
            app_config_get(cfg);
            if (cfg->topic_cmd[0] != '\0') {
                esp_mqtt_client_subscribe(event->client, cfg->topic_cmd, cfg->mqtt_qos);
                ESP_LOGI(TAG, "Subscribed cmd topic: %s", cfg->topic_cmd);
            }
            if (cfg->topic_ack[0] != '\0') {
                esp_mqtt_client_subscribe(event->client, cfg->topic_ack, cfg->mqtt_qos);
                ESP_LOGI(TAG, "Subscribed ACK topic: %s", cfg->topic_ack);
            }
            free(cfg);
        }
        mqtt_manager_publish_status("online");
        spool_manager_trigger_sync();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        gateway_state_set_mqtt_status(false, "Disconnected from broker", false);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        if (!event->topic || event->topic_len <= 0) {
            break;
        }
        gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
        if (!cfg) {
            break;
        }
        app_config_get(cfg);
        size_t ack_len = strlen(cfg->topic_ack);
        if (ack_len > 0 && (size_t)event->topic_len == ack_len &&
            strncmp(event->topic, cfg->topic_ack, ack_len) == 0) {
            handle_ack_payload(event->data, event->data_len);
        } else {
            handle_cmd_payload(event->data, event->data_len);
        }
        free(cfg);
        break;
    }
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

static void ip_got_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (!s_connected) {
        ESP_LOGI(TAG, "Network IP ready; starting/reconnecting MQTT");
        mqtt_manager_reconfigure();
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

bool mqtt_manager_is_connected(void)
{
    return s_connected;
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

    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        xSemaphoreGive(s_mqtt_mutex);
        return ESP_ERR_NO_MEM;
    }
    app_config_get(cfg);

    if (!cfg->mqtt_enabled || cfg->mqtt_host[0] == '\0') {
        gateway_state_set_mqtt_status(false, cfg->mqtt_enabled ? "No broker host set" : "MQTT disabled", false);
        free(cfg);
        xSemaphoreGive(s_mqtt_mutex);
        ESP_LOGI(TAG, "MQTT disabled or host empty");
        return ESP_OK;
    }

    gw_wifi_status_t w;
    wifi_manager_get_status(&w);
    if (cfg->wifi_mode != GW_WIFI_AP_ONLY && cfg->sta_ssid[0] != '\0' && !w.sta_connected) {
        gateway_state_set_mqtt_status(false, "Waiting for Wi-Fi IP", false);
        free(cfg);
        xSemaphoreGive(s_mqtt_mutex);
        ESP_LOGI(TAG, "Deferring MQTT connect until Wi-Fi STA gets IP");
        return ESP_OK;
    }

    if (strstr(cfg->mqtt_host, "://") != NULL) {
        app_config_safe_copy(s_broker_uri, sizeof(s_broker_uri), cfg->mqtt_host);
    } else {
        snprintf(s_broker_uri, sizeof(s_broker_uri), "mqtt://%s:%u", cfg->mqtt_host, (unsigned)cfg->mqtt_port);
    }

    app_config_safe_copy(s_lwt_topic, sizeof(s_lwt_topic), cfg->topic_status);
    snprintf(s_lwt_msg, sizeof(s_lwt_msg), "{\"status\":\"offline\",\"device_id\":\"%s\"}", cfg->device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_broker_uri,
        .credentials.client_id = cfg->mqtt_client_id[0] ? cfg->mqtt_client_id : cfg->device_id,
        .credentials.username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .session.keepalive = cfg->mqtt_keepalive,
        .session.last_will.topic = s_lwt_topic[0] ? s_lwt_topic : NULL,
        .session.last_will.msg = s_lwt_topic[0] ? s_lwt_msg : NULL,
        .session.last_will.qos = cfg->mqtt_qos,
        .session.last_will.retain = true,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    free(cfg);

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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_got_event_handler, NULL, NULL));
    xTaskCreate(heartbeat_task, "mqtt_hb", 3584, NULL, 4, NULL);
    return mqtt_manager_reconfigure();
}

bool mqtt_manager_publish_status(const char *state_label)
{
    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        return false;
    }
    app_config_get(cfg);
    if (!s_connected || !s_client || cfg->topic_status[0] == '\0') {
        free(cfg);
        return false;
    }

    gw_metrics_t m;
    gateway_state_get_metrics(&m);
    gw_wifi_status_t w;
    wifi_manager_get_status(&w);
    gw_spool_status_t sp;
    spool_manager_get_status(&sp);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", state_label ? state_label : "online");
    cJSON_AddStringToObject(root, "device_id", cfg->device_id);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddStringToObject(root, "location", cfg->location);
    cJSON_AddStringToObject(root, "ip", w.sta_connected ? w.sta_ip : w.ap_ip);
    cJSON_AddNumberToObject(root, "rssi", (double)w.sta_rssi);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "total_frames", (double)m.total_frames);
    cJSON_AddNumberToObject(root, "total_rx_bytes", (double)m.total_rx_bytes);
    cJSON_AddNumberToObject(root, "mqtt_pub_ok", (double)m.mqtt_pub_ok);
    cJSON_AddNumberToObject(root, "server_ack_ok", (double)m.server_ack_ok);
    cJSON_AddNumberToObject(root, "server_nack", (double)m.server_nack_cnt);
    cJSON_AddNumberToObject(root, "spool_pending", (double)sp.pending_count);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        free(cfg);
        return false;
    }

    int msg_id = esp_mqtt_client_publish(s_client, cfg->topic_status, json, 0, cfg->mqtt_qos, 1);
    cJSON_free(json);
    free(cfg);
    return msg_id >= 0;
}

bool mqtt_manager_publish_capture(const gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len)
{
    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        return false;
    }
    app_config_get(cfg);
    if (!cfg->mqtt_enabled || !s_connected || !s_client || cfg->topic_hex[0] == '\0' || !raw_data || raw_len == 0) {
        ESP_LOGW(TAG, "Skip MQTT publish (enabled=%d, connected=%d, topic='%s', len=%zu)",
                 cfg->mqtt_enabled, s_connected, cfg->topic_hex, raw_len);
        free(cfg);
        return false;
    }

    int msg_id = -1;
    if (cfg->mqtt_format == GW_MQTT_FMT_RAW_BIN) {
        msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, (const char *)raw_data, (int)raw_len,
                                           cfg->mqtt_qos, cfg->mqtt_retain ? 1 : 0);
        if (msg_id < 0 && cfg->mqtt_qos > 0) {
            msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, (const char *)raw_data, (int)raw_len,
                                               0, cfg->mqtt_retain ? 1 : 0);
        }
        free(cfg);
        return msg_id >= 0;
    }

    static const char hex_lut[] = "0123456789ABCDEF";

    if (cfg->mqtt_format == GW_MQTT_FMT_RAW_HEX) {
        char *hex_str = (char *)malloc(raw_len * 2 + 1);
        if (!hex_str) {
            ESP_LOGE(TAG, "OOM allocating %zu-byte raw HEX buffer (free=%lu)",
                     raw_len * 2 + 1, (unsigned long)esp_get_free_heap_size());
            free(cfg);
            return false;
        }
        for (size_t i = 0; i < raw_len; i++) {
            hex_str[i * 2] = hex_lut[(raw_data[i] >> 4) & 0x0F];
            hex_str[i * 2 + 1] = hex_lut[raw_data[i] & 0x0F];
        }
        hex_str[raw_len * 2] = '\0';

        msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, hex_str, (int)(raw_len * 2),
                                           cfg->mqtt_qos, cfg->mqtt_retain ? 1 : 0);
        if (msg_id < 0 && cfg->mqtt_qos > 0) {
            ESP_LOGW(TAG, "QoS%u outbox full, retrying QoS0", (unsigned)cfg->mqtt_qos);
            msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, hex_str, (int)(raw_len * 2),
                                               0, cfg->mqtt_retain ? 1 : 0);
        }
        free(hex_str);
        if (msg_id >= 0) {
            ESP_LOGI(TAG, "Published raw HEX frame #%lu (%zu B, CRC32=%08lX) to %s",
                     (unsigned long)meta->seq, raw_len, (unsigned long)meta->crc32, cfg->topic_hex);
        } else {
            ESP_LOGE(TAG, "Failed publishing raw HEX frame #%lu", (unsigned long)meta->seq);
        }
        free(cfg);
        return msg_id >= 0;
    }

    /* Single-buffer JSON+HEX envelope with CRC32 + ACK requirement */
    char *header = (char *)malloc(640);
    if (!header) {
        free(cfg);
        return false;
    }
    int hlen = snprintf(
        header, 640,
        "{\"device_id\":\"%s\",\"device_name\":\"%s\",\"location\":\"%s\","
        "\"seq\":%lu,\"crc32\":\"%08lX\",\"ack_required\":%s,\"ack_topic\":\"%s\",\"attempt\":%u,"
        "\"timestamp_ms\":%lld,\"bytes\":%zu,\"duration_ms\":%lu,"
        "\"rate_bps\":%lu,\"esc_k_count\":%zu,\"raster_stripes\":%zu,\"reason\":\"%s\","
        "\"commands\":{\"esc_at\":%s,\"esc_k\":%s,\"cr\":%s,\"lf\":%s,\"esc_l\":%s,\"esc_1\":%s},"
        "\"hex\":\"",
        cfg->device_id, cfg->device_name, cfg->location,
        (unsigned long)meta->seq, (unsigned long)meta->crc32,
        cfg->require_server_ack ? "true" : "false", cfg->topic_ack, (unsigned)meta->attempts,
        (long long)meta->timestamp_ms, meta->byte_len,
        (unsigned long)meta->duration_ms, (unsigned long)meta->rate_bps,
        meta->esc_k_count, meta->raster_stripes, meta->end_reason,
        meta->found_esc_at ? "true" : "false",
        meta->found_esc_k ? "true" : "false",
        meta->found_cr ? "true" : "false",
        meta->found_lf ? "true" : "false",
        meta->found_esc_l ? "true" : "false",
        meta->found_esc_1 ? "true" : "false"
    );
    if (hlen <= 0 || hlen >= 640) {
        ESP_LOGE(TAG, "JSON header overflow");
        free(header);
        free(cfg);
        return false;
    }

    size_t total_len = (size_t)hlen + (raw_len * 2) + 2;
    char *payload = (char *)malloc(total_len + 1);
    if (!payload) {
        ESP_LOGE(TAG, "OOM allocating %zu-byte MQTT payload (free=%lu)",
                 total_len + 1, (unsigned long)esp_get_free_heap_size());
        free(header);
        free(cfg);
        return false;
    }

    memcpy(payload, header, (size_t)hlen);
    free(header);
    char *p = payload + hlen;
    for (size_t i = 0; i < raw_len; i++) {
        *p++ = hex_lut[(raw_data[i] >> 4) & 0x0F];
        *p++ = hex_lut[raw_data[i] & 0x0F];
    }
    *p++ = '"';
    *p++ = '}';
    *p = '\0';

    msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, payload, (int)total_len,
                                       cfg->mqtt_qos, cfg->mqtt_retain ? 1 : 0);
    if (msg_id < 0 && cfg->mqtt_qos > 0) {
        ESP_LOGW(TAG, "MQTT QoS%u publish failed (%zu bytes), retrying QoS0", (unsigned)cfg->mqtt_qos, total_len);
        msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, payload, (int)total_len,
                                           0, cfg->mqtt_retain ? 1 : 0);
    }
    free(payload);

    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published frame #%lu (%zu B, CRC32=%08lX, attempt=%u) to %s",
                 (unsigned long)meta->seq, raw_len, (unsigned long)meta->crc32,
                 (unsigned)meta->attempts, cfg->topic_hex);
    } else {
        ESP_LOGE(TAG, "Failed publishing frame #%lu (%zu B) to %s",
                 (unsigned long)meta->seq, total_len, cfg->topic_hex);
    }
    free(cfg);
    return msg_id >= 0;
}

bool mqtt_manager_publish_test(void)
{
    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        return false;
    }
    app_config_get(cfg);
    if (!s_connected || !s_client || cfg->topic_hex[0] == '\0') {
        free(cfg);
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "gateway_test");
    cJSON_AddStringToObject(root, "device_id", cfg->device_id);
    cJSON_AddStringToObject(root, "device_name", cfg->device_name);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        free(cfg);
        return false;
    }

    int msg_id = esp_mqtt_client_publish(s_client, cfg->topic_hex, payload, 0, cfg->mqtt_qos, 0);
    cJSON_free(payload);
    free(cfg);
    return msg_id >= 0;
}
