#include "ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota_mgr";

static SemaphoreHandle_t s_ota_mutex = NULL;
static gw_ota_state_t s_state = GW_OTA_IDLE;
static size_t s_bytes_written = 0;
static size_t s_total_bytes = 0;
static char s_last_error[96] = {0};

static esp_ota_handle_t s_upload_handle = 0;
static const esp_partition_t *s_upload_part = NULL;
static bool s_upload_active = false;
static bool s_first_chunk_checked = false;

static void set_state_locked(gw_ota_state_t st, size_t written, size_t total, const char *err)
{
    s_state = st;
    s_bytes_written = written;
    s_total_bytes = total;
    if (err) {
        app_config_safe_copy(s_last_error, sizeof(s_last_error), err);
    } else if (st != GW_OTA_ERROR) {
        s_last_error[0] = '\0';
    }
}

static void delayed_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

void ota_manager_init(void)
{
    if (!s_ota_mutex) {
        s_ota_mutex = xSemaphoreCreateMutex();
    }
}

void ota_manager_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Confirming newly booted OTA partition '%s'", running->label);
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
}

void ota_manager_get_info(gw_ota_info_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    app_config_safe_copy(out->running_partition, sizeof(out->running_partition),
                         running ? running->label : "unknown");
    app_config_safe_copy(out->next_partition, sizeof(out->next_partition),
                         next ? next->label : "none");
    if (desc) {
        app_config_safe_copy(out->app_version, sizeof(out->app_version), desc->version);
        app_config_safe_copy(out->compile_date, sizeof(out->compile_date), desc->date);
        app_config_safe_copy(out->compile_time, sizeof(out->compile_time), desc->time);
    }

    esp_ota_img_states_t img_state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        out->pending_verify = (img_state == ESP_OTA_IMG_PENDING_VERIFY);
    }

    if (s_ota_mutex) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        out->state = s_state;
        out->bytes_written = s_bytes_written;
        out->total_bytes = s_total_bytes;
        app_config_safe_copy(out->last_error, sizeof(out->last_error), s_last_error);
        xSemaphoreGive(s_ota_mutex);
    }
}

static void ota_url_task(void *arg)
{
    char *url = (char *)arg;
    ESP_LOGI(TAG, "Starting URL OTA from %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "OTA begin failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, 0, msg);
        xSemaphoreGive(s_ota_mutex);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    int total = esp_https_ota_get_image_size(handle);
    if (total < 0) {
        total = 0;
    }

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        int written = esp_https_ota_get_image_len_read(handle);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_DOWNLOADING, written > 0 ? (size_t)written : 0, (size_t)total, NULL);
        xSemaphoreGive(s_ota_mutex);
    }

    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Download failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        esp_https_ota_abort(handle);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, (size_t)total, msg);
        xSemaphoreGive(s_ota_mutex);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    set_state_locked(GW_OTA_VERIFYING, (size_t)esp_https_ota_get_image_len_read(handle), (size_t)total, NULL);
    xSemaphoreGive(s_ota_mutex);

    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, (size_t)total, "Incomplete image received");
        xSemaphoreGive(s_ota_mutex);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Verify/finish failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, (size_t)total, msg);
        xSemaphoreGive(s_ota_mutex);
        free(url);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "URL OTA succeeded; rebooting...");
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    set_state_locked(GW_OTA_REBOOTING, (size_t)total, (size_t)total, NULL);
    xSemaphoreGive(s_ota_mutex);

    free(url);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

esp_err_t ota_manager_start_url(const char *url)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (s_upload_active || s_state == GW_OTA_DOWNLOADING || s_state == GW_OTA_UPLOADING || s_state == GW_OTA_VERIFYING) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    set_state_locked(GW_OTA_DOWNLOADING, 0, 0, NULL);
    xSemaphoreGive(s_ota_mutex);

    char *url_copy = strdup(url);
    if (!url_copy) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, 0, "Out of memory");
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ota_url_task, "ota_url", 8192, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        set_state_locked(GW_OTA_ERROR, 0, 0, "Failed to start OTA task");
        xSemaphoreGive(s_ota_mutex);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ota_manager_upload_begin(size_t total_bytes)
{
    if (total_bytes < 4096) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (s_upload_active || s_state == GW_OTA_DOWNLOADING || s_state == GW_OTA_VERIFYING) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        set_state_locked(GW_OTA_ERROR, 0, total_bytes, "No OTA partition available");
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (total_bytes > part->size) {
        set_state_locked(GW_OTA_ERROR, 0, total_bytes, "Firmware larger than OTA slot");
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_state_locked(GW_OTA_ERROR, 0, total_bytes, msg);
        xSemaphoreGive(s_ota_mutex);
        return err;
    }

    s_upload_part = part;
    s_upload_handle = handle;
    s_upload_active = true;
    s_first_chunk_checked = false;
    set_state_locked(GW_OTA_UPLOADING, 0, total_bytes, NULL);
    xSemaphoreGive(s_ota_mutex);

    ESP_LOGI(TAG, "Upload OTA started -> partition '%s' (%zu bytes)", part->label, total_bytes);
    return ESP_OK;
}

esp_err_t ota_manager_upload_write(const void *data, size_t len)
{
    if (!data || len == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (!s_upload_active) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_first_chunk_checked) {
        s_first_chunk_checked = true;
        const uint8_t *bytes = (const uint8_t *)data;
        if (bytes[0] != 0xE9) {
            esp_ota_abort(s_upload_handle);
            s_upload_active = false;
            s_upload_handle = 0;
            set_state_locked(GW_OTA_ERROR, 0, s_total_bytes, "Invalid ESP32 .bin magic byte");
            xSemaphoreGive(s_ota_mutex);
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = esp_ota_write(s_upload_handle, data, len);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "esp_ota_write failed: %s", esp_err_to_name(err));
        esp_ota_abort(s_upload_handle);
        s_upload_active = false;
        s_upload_handle = 0;
        set_state_locked(GW_OTA_ERROR, s_bytes_written, s_total_bytes, msg);
        xSemaphoreGive(s_ota_mutex);
        return err;
    }

    s_bytes_written += len;
    xSemaphoreGive(s_ota_mutex);
    return ESP_OK;
}

esp_err_t ota_manager_upload_finish(void)
{
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (!s_upload_active) {
        xSemaphoreGive(s_ota_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_state = GW_OTA_VERIFYING;
    esp_err_t err = esp_ota_end(s_upload_handle);
    s_upload_handle = 0;
    s_upload_active = false;

    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "OTA image validation failed: %s", esp_err_to_name(err));
        set_state_locked(GW_OTA_ERROR, s_bytes_written, s_total_bytes, msg);
        xSemaphoreGive(s_ota_mutex);
        return err;
    }

    err = esp_ota_set_boot_partition(s_upload_part);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Set boot partition failed: %s", esp_err_to_name(err));
        set_state_locked(GW_OTA_ERROR, s_bytes_written, s_total_bytes, msg);
        xSemaphoreGive(s_ota_mutex);
        return err;
    }

    ESP_LOGI(TAG, "Upload OTA verified (%zu bytes) on '%s'; rebooting...",
             s_bytes_written, s_upload_part->label);
    set_state_locked(GW_OTA_REBOOTING, s_bytes_written, s_total_bytes, NULL);
    xSemaphoreGive(s_ota_mutex);

    xTaskCreate(delayed_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

void ota_manager_upload_abort(const char *err_msg)
{
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (s_upload_active) {
        esp_ota_abort(s_upload_handle);
        s_upload_handle = 0;
        s_upload_active = false;
    }
    set_state_locked(GW_OTA_ERROR, s_bytes_written, s_total_bytes,
                     err_msg ? err_msg : "Upload aborted");
    xSemaphoreGive(s_ota_mutex);
}
