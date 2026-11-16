#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool sta_enabled;
    bool sta_connected;
    bool ap_enabled;
    char sta_ssid[33];
    char sta_ip[16];
    char sta_gw[16];
    char sta_mac[18];
    int8_t sta_rssi;
    char ap_ssid[33];
    char ap_ip[16];
    char ap_mac[18];
    uint8_t ap_clients;
    char last_disconnect_reason[48];
} gw_wifi_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;
} gw_wifi_ap_item_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_apply_config(void);
void wifi_manager_get_status(gw_wifi_status_t *out);
esp_err_t wifi_manager_scan(gw_wifi_ap_item_t *out, size_t max_items, size_t *out_count);

#ifdef __cplusplus
}
#endif
