#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static SemaphoreHandle_t s_wifi_mutex = NULL;
static gw_wifi_status_t s_status = {0};
static bool s_started = false;
static uint32_t s_sta_retry_count = 0;
static esp_timer_handle_t s_retry_timer = NULL;

static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    default: return "DISCONNECTED";
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    gw_config_t cfg;
    app_config_get(&cfg);
    if (cfg.wifi_mode != GW_WIFI_AP_ONLY && cfg.sta_ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            gw_config_t cfg;
            app_config_get(&cfg);
            if (cfg.sta_ssid[0] != '\0') {
                ESP_LOGI(TAG, "Connecting STA to SSID '%s'...", cfg.sta_ssid);
                esp_wifi_connect();
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            s_status.sta_connected = false;
            s_status.sta_ip[0] = '\0';
            s_status.sta_gw[0] = '\0';
            s_status.sta_rssi = 0;
            snprintf(s_status.last_disconnect_reason, sizeof(s_status.last_disconnect_reason),
                     "%s (%u)", reason_str(ev->reason), (unsigned)ev->reason);
            xSemaphoreGive(s_wifi_mutex);

            s_sta_retry_count++;
            uint64_t delay_us = (s_sta_retry_count <= 3) ? 2000000ULL : 10000000ULL;
            ESP_LOGW(TAG, "STA disconnected (%s), retry #%lu in %llums",
                     reason_str(ev->reason), (unsigned long)s_sta_retry_count, delay_us / 1000ULL);
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, delay_us);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            s_status.ap_clients++;
            xSemaphoreGive(s_wifi_mutex);
            ESP_LOGI(TAG, "AP client joined (" MACSTR ")", MAC2STR(ev->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
            if (s_status.ap_clients > 0) {
                s_status.ap_clients--;
            }
            xSemaphoreGive(s_wifi_mutex);
            ESP_LOGI(TAG, "AP client left (" MACSTR ")", MAC2STR(ev->mac));
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        s_sta_retry_count = 0;

        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
        s_status.sta_connected = true;
        snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(s_status.sta_gw, sizeof(s_status.sta_gw), IPSTR, IP2STR(&ev->ip_info.gw));
        s_status.last_disconnect_reason[0] = '\0';
        xSemaphoreGive(s_wifi_mutex);

        ESP_LOGI(TAG, "STA connected, IP=%s", s_status.sta_ip);
    }
}

esp_err_t wifi_manager_apply_config(void)
{
    gw_config_t cfg;
    app_config_get(&cfg);

    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    if (s_started) {
        esp_wifi_stop();
        s_started = false;
    }

    bool use_sta = (cfg.wifi_mode != GW_WIFI_AP_ONLY) && (cfg.sta_ssid[0] != '\0');
    bool use_ap = (cfg.wifi_mode != GW_WIFI_STA_ONLY) || !use_sta;

    wifi_mode_t mode = WIFI_MODE_AP;
    if (use_sta && use_ap) {
        mode = WIFI_MODE_APSTA;
    } else if (use_sta) {
        mode = WIFI_MODE_STA;
    } else {
        mode = WIFI_MODE_AP;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (use_sta) {
        wifi_config_t sta_cfg = {0};
        app_config_safe_copy((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg.sta_ssid);
        app_config_safe_copy((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg.sta_pass);
        sta_cfg.sta.threshold.authmode = (cfg.sta_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    if (use_ap) {
        wifi_config_t ap_cfg = {0};
        app_config_safe_copy((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), cfg.ap_ssid);
        ap_cfg.ap.ssid_len = (uint8_t)strlen(cfg.ap_ssid);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        if (strlen(cfg.ap_pass) >= 8) {
            app_config_safe_copy((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), cfg.ap_pass);
            ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    }

    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    s_status.sta_enabled = use_sta;
    s_status.ap_enabled = use_ap;
    s_status.sta_connected = false;
    s_status.sta_ip[0] = '\0';
    s_status.sta_gw[0] = '\0';
    s_status.sta_rssi = 0;
    s_status.ap_clients = 0;
    app_config_safe_copy(s_status.sta_ssid, sizeof(s_status.sta_ssid), cfg.sta_ssid);
    app_config_safe_copy(s_status.ap_ssid, sizeof(s_status.ap_ssid), cfg.ap_ssid);
    if (use_ap && s_netif_ap) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(s_netif_ap, &ip_info) == ESP_OK) {
            snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip_info.ip));
        }
    } else {
        s_status.ap_ip[0] = '\0';
    }
    xSemaphoreGive(s_wifi_mutex);

    s_sta_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;

    ESP_LOGI(TAG, "Wi-Fi started (STA=%d '%s', AP=%d '%s' IP=%s)",
             use_sta, cfg.sta_ssid, use_ap, cfg.ap_ssid, s_status.ap_ip);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (!s_wifi_mutex) {
        s_wifi_mutex = xSemaphoreCreateMutex();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_status.sta_mac, sizeof(s_status.sta_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_status.ap_mac, sizeof(s_status.ap_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    return wifi_manager_apply_config();
}

void wifi_manager_get_status(gw_wifi_status_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    if (s_status.sta_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_status.sta_rssi = ap_info.rssi;
        }
    }
    *out = s_status;
    xSemaphoreGive(s_wifi_mutex);
}

esp_err_t wifi_manager_scan(gw_wifi_ap_item_t *out, size_t max_items, size_t *out_count)
{
    if (!out || max_items == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur_mode);
    if (cur_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }

    uint16_t fetch = ap_num > (uint16_t)max_items ? (uint16_t)max_items : ap_num;
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(fetch, sizeof(wifi_ap_record_t));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch, recs);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < fetch; i++) {
            app_config_safe_copy(out[i].ssid, sizeof(out[i].ssid), (const char *)recs[i].ssid);
            out[i].rssi = recs[i].rssi;
            out[i].channel = recs[i].primary;
            out[i].authmode = (uint8_t)recs[i].authmode;
        }
        *out_count = fetch;
    }
    free(recs);
    return err;
}
