#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gateway_state.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_manager_init(void);
esp_err_t mqtt_manager_reconfigure(void);
bool mqtt_manager_publish_capture(const gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len);
bool mqtt_manager_publish_status(const char *state_label);
bool mqtt_manager_publish_test(void);

#ifdef __cplusplus
}
#endif
