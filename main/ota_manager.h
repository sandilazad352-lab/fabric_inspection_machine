#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_OTA_IDLE = 0,
    GW_OTA_DOWNLOADING = 1,
    GW_OTA_UPLOADING = 2,
    GW_OTA_VERIFYING = 3,
    GW_OTA_REBOOTING = 4,
    GW_OTA_ERROR = 5,
} gw_ota_state_t;

typedef struct {
    gw_ota_state_t state;
    char running_partition[17];
    char next_partition[17];
    char app_version[32];
    char compile_date[16];
    char compile_time[16];
    bool pending_verify;
    size_t bytes_written;
    size_t total_bytes;
    char last_error[96];
} gw_ota_info_t;

void ota_manager_init(void);
void ota_manager_mark_valid(void);
void ota_manager_get_info(gw_ota_info_t *out);

esp_err_t ota_manager_start_url(const char *url);

esp_err_t ota_manager_upload_begin(size_t total_bytes);
esp_err_t ota_manager_upload_write(const void *data, size_t len);
esp_err_t ota_manager_upload_finish(void);
void ota_manager_upload_abort(const char *err_msg);

#ifdef __cplusplus
}
#endif
