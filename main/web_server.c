#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gateway_state.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "rs232_reader.h"
#include "spool_manager.h"
#include "wifi_manager.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web_srv";

#define MAX_SESSIONS        4
#define SESSION_TTL_US      (12LL * 3600LL * 1000000LL)

typedef struct {
    bool active;
    char token[33];
    int64_t expires_us;
} session_entry_t;

static session_entry_t s_sessions[MAX_SESSIONS];

static void generate_token(char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        uint32_t r = esp_random();
        out[i * 2] = hex[(r >> 4) & 0x0F];
        out[i * 2 + 1] = hex[r & 0x0F];
    }
    out[32] = '\0';
}

static bool extract_token_from_req(httpd_req_t *req, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", out, out_sz) == ESP_OK && out[0] != '\0') {
        return true;
    }

    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0 || cookie_len > 512) {
        return false;
    }
    char *cookie_buf = (char *)malloc(cookie_len + 1);
    if (!cookie_buf) {
        return false;
    }
    bool found = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, cookie_len + 1) == ESP_OK) {
        const char *p = strstr(cookie_buf, "gw_sid=");
        if (p) {
            p += 7;
            size_t i = 0;
            while (p[i] != '\0' && p[i] != ';' && !isspace((unsigned char)p[i]) && i + 1 < out_sz) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            found = (i > 0);
        }
    }
    free(cookie_buf);
    return found;
}

static bool is_authenticated(httpd_req_t *req)
{
    char token[64];
    if (!extract_token_from_req(req, token, sizeof(token))) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
            if (now < s_sessions[i].expires_us) {
                s_sessions[i].expires_us = now + SESSION_TTL_US;
                return true;
            }
            s_sessions[i].active = false;
        }
    }
    return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t send_unauthorized(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", "Unauthorized");
    return send_json(req, "401 Unauthorized", r);
}

static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        return NULL;
    }
    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    cJSON *parsed = cJSON_Parse(buf);
    free(buf);
    return parsed;
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(450));
    esp_restart();
}

static void delayed_apply_wifi_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(350));
    wifi_manager_apply_config();
    vTaskDelete(NULL);
}

static esp_err_t handle_index(httpd_req_t *req)
{
    size_t len = (size_t)(index_html_end - index_html_start);
    if (len > 0 && index_html_start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, (ssize_t)len);
}

static esp_err_t handle_auth_status(httpd_req_t *req)
{
    gw_config_t cfg;
    app_config_get(&cfg);
    cJSON *r = cJSON_CreateObject();
    bool authed = is_authenticated(req);
    cJSON_AddBoolToObject(r, "authenticated", authed);
    if (authed) {
        cJSON_AddStringToObject(r, "user", cfg.admin_user);
    }
    cJSON_AddStringToObject(r, "device_name", cfg.device_name);
    cJSON_AddStringToObject(r, "device_id", cfg.device_id);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_login(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        return send_json(req, "400 Bad Request", err);
    }

    const cJSON *u = cJSON_GetObjectItem(body, "username");
    const cJSON *p = cJSON_GetObjectItem(body, "password");
    gw_config_t cfg;
    app_config_get(&cfg);

    bool ok = cJSON_IsString(u) && cJSON_IsString(p) &&
              strcmp(u->valuestring, cfg.admin_user) == 0 &&
              strcmp(p->valuestring, cfg.admin_pass) == 0;
    cJSON_Delete(body);

    if (!ok) {
        ESP_LOGW(TAG, "Failed admin login attempt");
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "Invalid username or password");
        return send_json(req, "401 Unauthorized", err);
    }

    int slot = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!s_sessions[i].active) {
            slot = i;
            break;
        }
        if (s_sessions[i].expires_us < oldest) {
            oldest = s_sessions[i].expires_us;
            slot = i;
        }
    }

    s_sessions[slot].active = true;
    generate_token(s_sessions[slot].token);
    s_sessions[slot].expires_us = esp_timer_get_time() + SESSION_TTL_US;

    char set_cookie[96];
    snprintf(set_cookie, sizeof(set_cookie), "gw_sid=%s; Path=/; HttpOnly; SameSite=Lax", s_sessions[slot].token);
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);

    ESP_LOGI(TAG, "Admin '%s' logged in", cfg.admin_user);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "token", s_sessions[slot].token);
    cJSON_AddStringToObject(r, "user", cfg.admin_user);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_logout(httpd_req_t *req)
{
    char token[64];
    if (extract_token_from_req(req, token, sizeof(token))) {
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].active && strcmp(s_sessions[i].token, token) == 0) {
                s_sessions[i].active = false;
            }
        }
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "gw_sid=; Path=/; Max-Age=0; HttpOnly");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_config_t cfg;
    app_config_get(&cfg);
    gw_wifi_status_t w;
    wifi_manager_get_status(&w);
    gw_metrics_t m;
    gateway_state_get_metrics(&m);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(r, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "min_heap", (double)esp_get_minimum_free_heap_size());

    cJSON *dev = cJSON_AddObjectToObject(r, "device");
    cJSON_AddStringToObject(dev, "name", cfg.device_name);
    cJSON_AddStringToObject(dev, "id", cfg.device_id);
    cJSON_AddStringToObject(dev, "location", cfg.location);

    cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
    cJSON_AddNumberToObject(wifi, "mode", cfg.wifi_mode);
    cJSON_AddBoolToObject(wifi, "sta_enabled", w.sta_enabled);
    cJSON_AddBoolToObject(wifi, "sta_connected", w.sta_connected);
    cJSON_AddStringToObject(wifi, "sta_ssid", w.sta_ssid);
    cJSON_AddStringToObject(wifi, "sta_ip", w.sta_ip);
    cJSON_AddStringToObject(wifi, "sta_gw", w.sta_gw);
    cJSON_AddStringToObject(wifi, "sta_mac", w.sta_mac);
    cJSON_AddNumberToObject(wifi, "sta_rssi", w.sta_rssi);
    cJSON_AddBoolToObject(wifi, "ap_enabled", w.ap_enabled);
    cJSON_AddStringToObject(wifi, "ap_ssid", w.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_ip", w.ap_ip);
    cJSON_AddStringToObject(wifi, "ap_mac", w.ap_mac);
    cJSON_AddNumberToObject(wifi, "ap_clients", w.ap_clients);
    cJSON_AddStringToObject(wifi, "last_err", w.last_disconnect_reason);

    cJSON *mqtt = cJSON_AddObjectToObject(r, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", cfg.mqtt_enabled);
    cJSON_AddBoolToObject(mqtt, "connected", m.mqtt_connected);
    cJSON_AddStringToObject(mqtt, "host", cfg.mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port", cfg.mqtt_port);
    cJSON_AddNumberToObject(mqtt, "pub_ok", (double)m.mqtt_pub_ok);
    cJSON_AddNumberToObject(mqtt, "pub_err", (double)m.mqtt_pub_err);
    cJSON_AddNumberToObject(mqtt, "reconnects", (double)m.mqtt_reconnects);
    cJSON_AddNumberToObject(mqtt, "server_ack_ok", (double)m.server_ack_ok);
    cJSON_AddNumberToObject(mqtt, "server_nack", (double)m.server_nack_cnt);
    cJSON_AddNumberToObject(mqtt, "crc_errors", (double)m.crc_error_cnt);
    cJSON_AddStringToObject(mqtt, "last_err", m.mqtt_last_err);

    gw_spool_status_t sp;
    spool_manager_get_status(&sp);
    cJSON *spool = cJSON_AddObjectToObject(r, "spool");
    cJSON_AddBoolToObject(spool, "mounted", sp.mounted);
    cJSON_AddNumberToObject(spool, "total_bytes", (double)sp.total_bytes);
    cJSON_AddNumberToObject(spool, "used_bytes", (double)sp.used_bytes);
    cJSON_AddNumberToObject(spool, "pending_count", (double)sp.pending_count);
    cJSON_AddNumberToObject(spool, "pending_bytes", (double)sp.pending_bytes);
    cJSON_AddNumberToObject(spool, "active_seq", (double)sp.active_seq);
    cJSON_AddStringToObject(spool, "sync_state", sp.sync_state);
    cJSON_AddBoolToObject(spool, "require_server_ack", cfg.require_server_ack);

    cJSON *uart = cJSON_AddObjectToObject(r, "uart");
    cJSON_AddNumberToObject(uart, "baud", cfg.uart_baud);
    cJSON_AddNumberToObject(uart, "tx_pin", cfg.uart_tx_pin);
    cJSON_AddNumberToObject(uart, "rx_pin", cfg.uart_rx_pin);
    cJSON_AddNumberToObject(uart, "data_bits", cfg.uart_data_bits);
    cJSON_AddNumberToObject(uart, "parity", cfg.uart_parity);
    cJSON_AddNumberToObject(uart, "stop_bits", cfg.uart_stop_bits);
    cJSON_AddNumberToObject(uart, "idle_timeout_ms", cfg.idle_timeout_ms);
    cJSON_AddNumberToObject(uart, "total_frames", (double)m.total_frames);
    cJSON_AddNumberToObject(uart, "total_rx_bytes", (double)m.total_rx_bytes);
    cJSON_AddNumberToObject(uart, "uart_errors", (double)m.uart_errors);

    return send_json(req, "200 OK", r);
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_config_t cfg;
    app_config_get(&cfg);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "device_name", cfg.device_name);
    cJSON_AddStringToObject(r, "device_id", cfg.device_id);
    cJSON_AddStringToObject(r, "location", cfg.location);

    cJSON_AddStringToObject(r, "admin_user", cfg.admin_user);

    cJSON_AddNumberToObject(r, "wifi_mode", cfg.wifi_mode);
    cJSON_AddStringToObject(r, "sta_ssid", cfg.sta_ssid);
    cJSON_AddBoolToObject(r, "has_sta_pass", cfg.sta_pass[0] != '\0');
    cJSON_AddStringToObject(r, "ap_ssid", cfg.ap_ssid);
    cJSON_AddBoolToObject(r, "has_ap_pass", cfg.ap_pass[0] != '\0');

    cJSON_AddNumberToObject(r, "uart_baud", cfg.uart_baud);
    cJSON_AddNumberToObject(r, "uart_tx_pin", cfg.uart_tx_pin);
    cJSON_AddNumberToObject(r, "uart_rx_pin", cfg.uart_rx_pin);
    cJSON_AddNumberToObject(r, "uart_data_bits", cfg.uart_data_bits);
    cJSON_AddNumberToObject(r, "uart_parity", cfg.uart_parity);
    cJSON_AddNumberToObject(r, "uart_stop_bits", cfg.uart_stop_bits);
    cJSON_AddNumberToObject(r, "idle_timeout_ms", cfg.idle_timeout_ms);
    cJSON_AddNumberToObject(r, "max_capture_bytes", cfg.max_capture_bytes);
    cJSON_AddBoolToObject(r, "auto_publish_mqtt", cfg.auto_publish_mqtt);
    cJSON_AddBoolToObject(r, "serial_echo_hex", cfg.serial_echo_hex);
    cJSON_AddBoolToObject(r, "verbose_debug", cfg.verbose_debug);

    cJSON_AddBoolToObject(r, "mqtt_enabled", cfg.mqtt_enabled);
    cJSON_AddStringToObject(r, "mqtt_host", cfg.mqtt_host);
    cJSON_AddNumberToObject(r, "mqtt_port", cfg.mqtt_port);
    cJSON_AddStringToObject(r, "mqtt_client_id", cfg.mqtt_client_id);
    cJSON_AddStringToObject(r, "mqtt_user", cfg.mqtt_user);
    cJSON_AddBoolToObject(r, "has_mqtt_pass", cfg.mqtt_pass[0] != '\0');
    cJSON_AddNumberToObject(r, "mqtt_keepalive", cfg.mqtt_keepalive);
    cJSON_AddNumberToObject(r, "mqtt_qos", cfg.mqtt_qos);
    cJSON_AddBoolToObject(r, "mqtt_retain", cfg.mqtt_retain);
    cJSON_AddNumberToObject(r, "mqtt_format", cfg.mqtt_format);
    cJSON_AddBoolToObject(r, "mqtt_debug_stream", cfg.mqtt_debug_stream);
    cJSON_AddStringToObject(r, "topic_hex", cfg.topic_hex);
    cJSON_AddStringToObject(r, "topic_status", cfg.topic_status);
    cJSON_AddStringToObject(r, "topic_cmd", cfg.topic_cmd);
    cJSON_AddStringToObject(r, "topic_debug", cfg.topic_debug);
    cJSON_AddStringToObject(r, "topic_ack", cfg.topic_ack);
    cJSON_AddBoolToObject(r, "require_server_ack", cfg.require_server_ack);
    cJSON_AddNumberToObject(r, "ack_timeout_ms", cfg.ack_timeout_ms);
    cJSON_AddNumberToObject(r, "max_spool_files", cfg.max_spool_files);

    return send_json(req, "200 OK", r);
}

static void replace_substr_once(char *buf, size_t buf_sz, const char *old_s, const char *new_s)
{
    if (!buf || buf_sz == 0 || !old_s || old_s[0] == '\0' || !new_s) {
        return;
    }
    char *pos = strstr(buf, old_s);
    if (!pos) {
        return;
    }
    char tmp[128];
    size_t prefix_len = (size_t)(pos - buf);
    size_t old_len = strlen(old_s);
    snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)prefix_len, buf, new_s, pos + old_len);
    app_config_safe_copy(buf, buf_sz, tmp);
}

static void json_copy_str(const cJSON *obj, const char *key, char *dst, size_t dst_sz, bool skip_if_empty)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item)) {
        if (skip_if_empty && item->valuestring[0] == '\0') {
            return;
        }
        app_config_safe_copy(dst, dst_sz, item->valuestring);
    }
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    cJSON *body = read_json_body(req);
    if (!body) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        return send_json(req, "400 Bad Request", err);
    }

    gw_config_t prev;
    app_config_get(&prev);
    gw_config_t next = prev;

    json_copy_str(body, "device_name", next.device_name, sizeof(next.device_name), false);
    json_copy_str(body, "device_id", next.device_id, sizeof(next.device_id), true);
    json_copy_str(body, "location", next.location, sizeof(next.location), false);

    json_copy_str(body, "admin_user", next.admin_user, sizeof(next.admin_user), true);
    json_copy_str(body, "admin_pass", next.admin_pass, sizeof(next.admin_pass), true);

    const cJSON *it;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "wifi_mode")))) next.wifi_mode = (uint8_t)it->valueint;
    json_copy_str(body, "sta_ssid", next.sta_ssid, sizeof(next.sta_ssid), false);
    json_copy_str(body, "sta_pass", next.sta_pass, sizeof(next.sta_pass), true);
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_sta_pass"))) next.sta_pass[0] = '\0';
    json_copy_str(body, "ap_ssid", next.ap_ssid, sizeof(next.ap_ssid), true);
    json_copy_str(body, "ap_pass", next.ap_pass, sizeof(next.ap_pass), true);
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_ap_pass"))) next.ap_pass[0] = '\0';

    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_baud")))) next.uart_baud = it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_tx_pin")))) next.uart_tx_pin = it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_rx_pin")))) next.uart_rx_pin = it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_data_bits")))) next.uart_data_bits = (uint8_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_parity")))) next.uart_parity = (uint8_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "uart_stop_bits")))) next.uart_stop_bits = (uint8_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "idle_timeout_ms")))) next.idle_timeout_ms = (uint32_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "max_capture_bytes")))) next.max_capture_bytes = (uint32_t)it->valueint;
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "auto_publish_mqtt")))) next.auto_publish_mqtt = cJSON_IsTrue(it);
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "serial_echo_hex")))) next.serial_echo_hex = cJSON_IsTrue(it);
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "verbose_debug")))) next.verbose_debug = cJSON_IsTrue(it);

    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "mqtt_enabled")))) next.mqtt_enabled = cJSON_IsTrue(it);
    json_copy_str(body, "mqtt_host", next.mqtt_host, sizeof(next.mqtt_host), false);
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "mqtt_port")))) next.mqtt_port = (uint16_t)it->valueint;
    json_copy_str(body, "mqtt_client_id", next.mqtt_client_id, sizeof(next.mqtt_client_id), false);
    json_copy_str(body, "mqtt_user", next.mqtt_user, sizeof(next.mqtt_user), false);
    json_copy_str(body, "mqtt_pass", next.mqtt_pass, sizeof(next.mqtt_pass), true);
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_mqtt_pass"))) next.mqtt_pass[0] = '\0';
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "mqtt_keepalive")))) next.mqtt_keepalive = (uint16_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "mqtt_qos")))) next.mqtt_qos = (uint8_t)it->valueint;
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "mqtt_retain")))) next.mqtt_retain = cJSON_IsTrue(it);
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "mqtt_format")))) next.mqtt_format = (uint8_t)it->valueint;
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "mqtt_debug_stream")))) next.mqtt_debug_stream = cJSON_IsTrue(it);
    json_copy_str(body, "topic_hex", next.topic_hex, sizeof(next.topic_hex), false);
    json_copy_str(body, "topic_status", next.topic_status, sizeof(next.topic_status), false);
    json_copy_str(body, "topic_cmd", next.topic_cmd, sizeof(next.topic_cmd), false);
    json_copy_str(body, "topic_debug", next.topic_debug, sizeof(next.topic_debug), false);
    json_copy_str(body, "topic_ack", next.topic_ack, sizeof(next.topic_ack), false);
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "require_server_ack")))) next.require_server_ack = cJSON_IsTrue(it);
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "ack_timeout_ms")))) next.ack_timeout_ms = (uint32_t)it->valueint;
    if (cJSON_IsNumber((it = cJSON_GetObjectItem(body, "max_spool_files")))) next.max_spool_files = (uint32_t)it->valueint;

    bool sync_topics = true;
    if (cJSON_IsBool((it = cJSON_GetObjectItem(body, "sync_device_topics")))) {
        sync_topics = cJSON_IsTrue(it);
    }
    if (sync_topics && strcmp(prev.device_id, next.device_id) != 0) {
        if (!cJSON_GetObjectItem(body, "mqtt_client_id") &&
            (strcmp(next.mqtt_client_id, prev.device_id) == 0 || next.mqtt_client_id[0] == '\0')) {
            app_config_safe_copy(next.mqtt_client_id, sizeof(next.mqtt_client_id), next.device_id);
        }
        if (!cJSON_GetObjectItem(body, "topic_hex")) replace_substr_once(next.topic_hex, sizeof(next.topic_hex), prev.device_id, next.device_id);
        if (!cJSON_GetObjectItem(body, "topic_status")) replace_substr_once(next.topic_status, sizeof(next.topic_status), prev.device_id, next.device_id);
        if (!cJSON_GetObjectItem(body, "topic_cmd")) replace_substr_once(next.topic_cmd, sizeof(next.topic_cmd), prev.device_id, next.device_id);
        if (!cJSON_GetObjectItem(body, "topic_debug")) replace_substr_once(next.topic_debug, sizeof(next.topic_debug), prev.device_id, next.device_id);
        if (!cJSON_GetObjectItem(body, "topic_ack")) replace_substr_once(next.topic_ack, sizeof(next.topic_ack), prev.device_id, next.device_id);
    }

    cJSON_Delete(body);

    esp_err_t err = app_config_set(&next);
    if (err != ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", "Failed to save NVS");
        return send_json(req, "500 Internal Server Error", r);
    }

    esp_log_level_set("*", next.verbose_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);

    bool uart_changed = (prev.uart_baud != next.uart_baud || prev.uart_tx_pin != next.uart_tx_pin ||
                         prev.uart_rx_pin != next.uart_rx_pin || prev.uart_data_bits != next.uart_data_bits ||
                         prev.uart_parity != next.uart_parity || prev.uart_stop_bits != next.uart_stop_bits);
    if (uart_changed) {
        rs232_reader_reconfigure();
    }

    bool mqtt_changed = (prev.mqtt_enabled != next.mqtt_enabled || strcmp(prev.mqtt_host, next.mqtt_host) != 0 ||
                         prev.mqtt_port != next.mqtt_port || strcmp(prev.mqtt_client_id, next.mqtt_client_id) != 0 ||
                         strcmp(prev.mqtt_user, next.mqtt_user) != 0 || strcmp(prev.mqtt_pass, next.mqtt_pass) != 0 ||
                         prev.mqtt_keepalive != next.mqtt_keepalive || strcmp(prev.device_id, next.device_id) != 0 ||
                         strcmp(prev.topic_hex, next.topic_hex) != 0 || strcmp(prev.topic_status, next.topic_status) != 0 ||
                         strcmp(prev.topic_cmd, next.topic_cmd) != 0 || strcmp(prev.topic_ack, next.topic_ack) != 0);
    if (mqtt_changed) {
        mqtt_manager_reconfigure();
    }

    bool wifi_changed = (prev.wifi_mode != next.wifi_mode || strcmp(prev.sta_ssid, next.sta_ssid) != 0 ||
                         strcmp(prev.sta_pass, next.sta_pass) != 0 || strcmp(prev.ap_ssid, next.ap_ssid) != 0 ||
                         strcmp(prev.ap_pass, next.ap_pass) != 0);
    if (wifi_changed) {
        xTaskCreate(delayed_apply_wifi_task, "wifi_apply", 4096, NULL, 5, NULL);
    }

    ESP_LOGI(TAG, "Configuration updated (wifi=%d, uart=%d, mqtt=%d)", wifi_changed, uart_changed, mqtt_changed);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "wifi_restarting", wifi_changed);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_wifi_ap_item_t items[20];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(items, 20, &count);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    cJSON *arr = cJSON_AddArrayToObject(r, "aps");
    for (size_t i = 0; i < count; i++) {
        if (items[i].ssid[0] == '\0') {
            continue;
        }
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", items[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", items[i].channel);
        cJSON_AddNumberToObject(ap, "auth", items[i].authmode);
        cJSON_AddItemToArray(arr, ap);
    }
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_captures(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_capture_meta_t hist[GW_MAX_CAPTURES_HISTORY];
    size_t count = gateway_state_get_history(hist, GW_MAX_CAPTURES_HISTORY);

    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "history");
    for (size_t i = 0; i < count; i++) {
        cJSON *it = cJSON_CreateObject();
        char crc_str[10];
        snprintf(crc_str, sizeof(crc_str), "%08lX", (unsigned long)hist[i].crc32);
        const char *ack_str = "queued";
        switch (hist[i].ack_state) {
        case GW_ACK_WAIT_SERVER: ack_str = "wait_ack"; break;
        case GW_ACK_OK: ack_str = "acked"; break;
        case GW_ACK_NACK: ack_str = "nack"; break;
        case GW_ACK_BROKER_ONLY: ack_str = "sent"; break;
        default: break;
        }
        cJSON_AddNumberToObject(it, "seq", (double)hist[i].seq);
        cJSON_AddStringToObject(it, "crc32", crc_str);
        cJSON_AddStringToObject(it, "ack_state", ack_str);
        cJSON_AddNumberToObject(it, "attempts", hist[i].attempts);
        cJSON_AddNumberToObject(it, "timestamp_ms", (double)hist[i].timestamp_ms);
        cJSON_AddNumberToObject(it, "bytes", (double)hist[i].byte_len);
        cJSON_AddNumberToObject(it, "duration_ms", (double)hist[i].duration_ms);
        cJSON_AddNumberToObject(it, "rate_bps", (double)hist[i].rate_bps);
        cJSON_AddNumberToObject(it, "esc_k_count", (double)hist[i].esc_k_count);
        cJSON_AddNumberToObject(it, "raster_stripes", (double)hist[i].raster_stripes);
        cJSON_AddStringToObject(it, "reason", hist[i].end_reason);
        cJSON_AddBoolToObject(it, "mqtt_published", hist[i].mqtt_published);

        cJSON *cmds = cJSON_AddObjectToObject(it, "commands");
        cJSON_AddBoolToObject(cmds, "esc_at", hist[i].found_esc_at);
        cJSON_AddBoolToObject(cmds, "esc_k", hist[i].found_esc_k);
        cJSON_AddBoolToObject(cmds, "cr", hist[i].found_cr);
        cJSON_AddBoolToObject(cmds, "lf", hist[i].found_lf);
        cJSON_AddBoolToObject(cmds, "esc_l", hist[i].found_esc_l);
        cJSON_AddBoolToObject(cmds, "esc_1", hist[i].found_esc_1);
        cJSON_AddItemToArray(arr, it);
    }
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_latest_hex(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_capture_meta_t meta;
    size_t raw_len = 0;
    if (!gateway_state_get_latest(&meta, NULL, 0, &raw_len) || raw_len == 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "");
    }

    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    if (!gateway_state_get_latest(&meta, raw, raw_len, &raw_len) || raw_len == 0) {
        free(raw);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "dl=1") != NULL) {
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"HMI_CAPTURE_latest.hex\"");
    }

    static const char hex_lut[] = "0123456789ABCDEF";
    char chunk[1024];
    size_t pos = 0;
    size_t col = 0;

    for (size_t i = 0; i < raw_len; i++) {
        chunk[pos++] = hex_lut[(raw[i] >> 4) & 0x0F];
        chunk[pos++] = hex_lut[raw[i] & 0x0F];
        col += 2;
        if (col >= 80) {
            chunk[pos++] = '\n';
            col = 0;
        }
        if (pos >= sizeof(chunk) - 4) {
            httpd_resp_send_chunk(req, chunk, (ssize_t)pos);
            pos = 0;
        }
    }
    if (pos > 0) {
        httpd_resp_send_chunk(req, chunk, (ssize_t)pos);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    free(raw);
    return ESP_OK;
}

static esp_err_t handle_logs(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    uint32_t since = 0;
    char q[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(q, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    gw_log_entry_t *entries = (gw_log_entry_t *)calloc(GW_LOG_RING_LINES, sizeof(gw_log_entry_t));
    if (!entries) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t n = gateway_state_get_logs(since, entries, GW_LOG_RING_LINES);
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "logs");
    uint32_t last_seq = since;
    for (size_t i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "seq", (double)entries[i].seq);
        cJSON_AddNumberToObject(it, "ts", (double)entries[i].timestamp_ms);
        cJSON_AddStringToObject(it, "text", entries[i].text);
        cJSON_AddItemToArray(arr, it);
        last_seq = entries[i].seq;
    }
    cJSON_AddNumberToObject(r, "last_seq", (double)last_seq);
    free(entries);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_logs_clear(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    gateway_state_clear_logs();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_test_mqtt(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    bool ok = mqtt_manager_publish_test();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_test_uart(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    rs232_reader_inject_test_frame();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_reboot(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    ESP_LOGW(TAG, "System reboot requested via Web UI");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t err = send_json(req, "200 OK", r);
    xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    return err;
}

static esp_err_t handle_ota_status(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_ota_info_t info;
    ota_manager_get_info(&info);

    const char *state_str = "idle";
    switch (info.state) {
    case GW_OTA_DOWNLOADING: state_str = "downloading"; break;
    case GW_OTA_UPLOADING: state_str = "uploading"; break;
    case GW_OTA_VERIFYING: state_str = "verifying"; break;
    case GW_OTA_REBOOTING: state_str = "rebooting"; break;
    case GW_OTA_ERROR: state_str = "error"; break;
    default: break;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", state_str);
    cJSON_AddStringToObject(r, "running_partition", info.running_partition);
    cJSON_AddStringToObject(r, "next_partition", info.next_partition);
    cJSON_AddStringToObject(r, "app_version", info.app_version);
    cJSON_AddStringToObject(r, "compile_date", info.compile_date);
    cJSON_AddStringToObject(r, "compile_time", info.compile_time);
    cJSON_AddBoolToObject(r, "pending_verify", info.pending_verify);
    cJSON_AddNumberToObject(r, "bytes_written", (double)info.bytes_written);
    cJSON_AddNumberToObject(r, "total_bytes", (double)info.total_bytes);
    cJSON_AddStringToObject(r, "last_error", info.last_error);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_ota_url(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    cJSON *body = read_json_body(req);
    if (!body) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "Invalid JSON");
        return send_json(req, "400 Bad Request", err);
    }

    const cJSON *url = cJSON_GetObjectItem(body, "url");
    esp_err_t err_code = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(url)) {
        err_code = ota_manager_start_url(url->valuestring);
    }
    cJSON_Delete(body);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err_code == ESP_OK);
    if (err_code != ESP_OK) {
        cJSON_AddStringToObject(r, "error", esp_err_to_name(err_code));
        return send_json(req, "400 Bad Request", r);
    }
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_ota_upload(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    if (req->content_len <= 0) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddBoolToObject(err, "ok", false);
        cJSON_AddStringToObject(err, "error", "Empty firmware file");
        return send_json(req, "400 Bad Request", err);
    }

    esp_err_t err = ota_manager_upload_begin((size_t)req->content_len);
    if (err != ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
        return send_json(req, "400 Bad Request", r);
    }

    char *buf = (char *)malloc(2048);
    if (!buf) {
        ota_manager_upload_abort("Out of memory");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > 2048 ? 2048 : remaining;
        int rlen = httpd_req_recv(req, buf, to_read);
        if (rlen <= 0) {
            if (rlen == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            ota_manager_upload_abort("Socket receive error");
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", false);
            cJSON_AddStringToObject(r, "error", "Upload interrupted");
            return send_json(req, "400 Bad Request", r);
        }
        err = ota_manager_upload_write(buf, (size_t)rlen);
        if (err != ESP_OK) {
            free(buf);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", false);
            cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
            return send_json(req, "400 Bad Request", r);
        }
        remaining -= rlen;
    }
    free(buf);

    err = ota_manager_upload_finish();
    if (err != ESP_OK) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
        return send_json(req, "400 Bad Request", r);
    }

    gw_ota_info_t info;
    ota_manager_get_info(&info);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "target_partition", info.next_partition);
    cJSON_AddNumberToObject(r, "bytes", (double)req->content_len);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_spool_list(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }

    gw_spool_status_t sp;
    spool_manager_get_status(&sp);

    gw_spool_item_t *items = (gw_spool_item_t *)calloc(32, sizeof(gw_spool_item_t));
    size_t n = items ? spool_manager_list_pending(items, 32) : 0;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "mounted", sp.mounted);
    cJSON_AddNumberToObject(r, "total_bytes", (double)sp.total_bytes);
    cJSON_AddNumberToObject(r, "used_bytes", (double)sp.used_bytes);
    cJSON_AddNumberToObject(r, "pending_count", (double)sp.pending_count);
    cJSON_AddNumberToObject(r, "pending_bytes", (double)sp.pending_bytes);
    cJSON_AddNumberToObject(r, "active_seq", (double)sp.active_seq);
    cJSON_AddStringToObject(r, "sync_state", sp.sync_state);

    cJSON *arr = cJSON_AddArrayToObject(r, "items");
    for (size_t i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        char crc_str[10];
        snprintf(crc_str, sizeof(crc_str), "%08lX", (unsigned long)items[i].crc32);
        cJSON_AddNumberToObject(it, "seq", (double)items[i].seq);
        cJSON_AddStringToObject(it, "crc32", crc_str);
        cJSON_AddNumberToObject(it, "bytes", (double)items[i].byte_len);
        cJSON_AddNumberToObject(it, "timestamp_ms", (double)items[i].timestamp_ms);
        cJSON_AddNumberToObject(it, "attempts", items[i].attempts);
        cJSON_AddItemToArray(arr, it);
    }
    free(items);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_sync_now(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    spool_manager_trigger_sync();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_purge_spool(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    esp_err_t err = spool_manager_purge_all();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    return send_json(req, "200 OK", r);
}

static esp_err_t handle_factory_reset(httpd_req_t *req)
{
    if (!is_authenticated(req)) {
        return send_unauthorized(req);
    }
    ESP_LOGW(TAG, "Factory reset requested via Web UI");
    app_config_factory_reset();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t err = send_json(req, "200 OK", r);
    xTaskCreate(reboot_task, "reset_reboot", 2048, NULL, 5, NULL);
    return err;
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handle_index},
        {.uri = "/api/auth/status", .method = HTTP_GET, .handler = handle_auth_status},
        {.uri = "/api/login", .method = HTTP_POST, .handler = handle_login},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = handle_logout},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handle_status},
        {.uri = "/api/config", .method = HTTP_GET, .handler = handle_get_config},
        {.uri = "/api/config", .method = HTTP_POST, .handler = handle_post_config},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = handle_wifi_scan},
        {.uri = "/api/captures", .method = HTTP_GET, .handler = handle_captures},
        {.uri = "/api/captures/latest.hex", .method = HTTP_GET, .handler = handle_latest_hex},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = handle_logs},
        {.uri = "/api/logs/clear", .method = HTTP_POST, .handler = handle_logs_clear},
        {.uri = "/api/actions/test-mqtt", .method = HTTP_POST, .handler = handle_test_mqtt},
        {.uri = "/api/actions/test-uart", .method = HTTP_POST, .handler = handle_test_uart},
        {.uri = "/api/actions/reboot", .method = HTTP_POST, .handler = handle_reboot},
        {.uri = "/api/actions/factory-reset", .method = HTTP_POST, .handler = handle_factory_reset},
        {.uri = "/api/ota/status", .method = HTTP_GET, .handler = handle_ota_status},
        {.uri = "/api/ota/url", .method = HTTP_POST, .handler = handle_ota_url},
        {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = handle_ota_upload},
        {.uri = "/api/spool", .method = HTTP_GET, .handler = handle_spool_list},
        {.uri = "/api/actions/sync-now", .method = HTTP_POST, .handler = handle_sync_now},
        {.uri = "/api/actions/purge-spool", .method = HTTP_POST, .handler = handle_purge_spool},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "HTTP management server started on port %u", (unsigned)config.server_port);
    return ESP_OK;
}
