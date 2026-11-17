#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gateway_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;
    size_t total_bytes;
    size_t used_bytes;
    size_t pending_count;
    size_t pending_bytes;
    uint32_t active_seq;
    char sync_state[24];
} gw_spool_status_t;

typedef struct {
    uint32_t seq;
    uint32_t crc32;
    size_t byte_len;
    int64_t timestamp_ms;
    uint8_t attempts;
} gw_spool_item_t;

esp_err_t spool_manager_init(void);
esp_err_t spool_manager_enqueue(gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len);
void spool_manager_on_server_ack(uint32_t seq, bool ok, bool has_crc, uint32_t server_crc, const char *reason);
void spool_manager_trigger_sync(void);
esp_err_t spool_manager_purge_all(void);
void spool_manager_get_status(gw_spool_status_t *out);
size_t spool_manager_list_pending(gw_spool_item_t *out, size_t max_items);

#ifdef __cplusplus
}
#endif
