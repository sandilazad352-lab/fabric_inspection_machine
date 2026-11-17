#include "app_config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "gateway_state.h"
#include "mqtt_manager.h"
#include "nvs_flash.h"
#include "ota_manager.h"
#include "rs232_reader.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "gw_main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gateway_state_init();
    ota_manager_init();
    ESP_ERROR_CHECK(app_config_init());

    gw_config_t cfg;
    app_config_get(&cfg);
    esp_log_level_set("*", cfg.verbose_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);

    ESP_LOGI(TAG, "Starting %s (%s)", cfg.device_name, cfg.device_id);

    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(mqtt_manager_init());
    ESP_ERROR_CHECK(rs232_reader_init());
    ESP_ERROR_CHECK(web_server_start());
    ota_manager_mark_valid();

    ESP_LOGI(TAG, "Gateway ready (AP SSID: %s, Web UI: http://192.168.4.1/)", cfg.ap_ssid);
}
