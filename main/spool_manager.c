#include "spool_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_config.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_manager.h"
#include "nvs.h"

#define SPOOL_BASE_PATH   "/spool"
#define SPOOL_MAGIC       0x46535031U /* "FSP1" */
#define NVS_SEQ_NS        "gw_seq"
#define NVS_SEQ_KEY       "seq"

static const char *TAG = "spool_mgr";

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t seq;
    uint32_t crc32;
    uint32_t byte_len;
    int64_t timestamp_ms;
    uint32_t duration_ms;
    uint32_t rate_bps;
    uint16_t esc_k_count;
    uint16_t raster_stripes;
    uint8_t flags;
    uint8_t attempts;
    char end_reason[16];
} spool_file_hdr_t;

typedef enum {
    WAIT_NONE = 0,
    WAIT_PENDING = 1,
    WAIT_ACK_OK = 2,
    WAIT_ACK_NACK = 3,
} ack_wait_result_t;

static SemaphoreHandle_t s_spool_mutex = NULL;
static SemaphoreHandle_t s_ack_sem = NULL;
static TaskHandle_t s_sync_task = NULL;

static bool s_mounted = false;
static uint32_t s_next_seq = 1;
static uint32_t s_active_seq = 0;
static char s_sync_state[24] = "idle";

static volatile uint32_t s_wait_seq = 0;
static volatile uint32_t s_wait_crc = 0;
static volatile ack_wait_result_t s_wait_result = WAIT_NONE;

static uint32_t alloc_next_seq_locked(void)
{
    uint32_t seq = s_next_seq++;
    nvs_handle_t h;
    if (nvs_open(NVS_SEQ_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_SEQ_KEY, s_next_seq);
        nvs_commit(h);
        nvs_close(h);
    }
    return seq;
}

static bool parse_spool_filename(const char *name, uint32_t *seq_out)
{
    unsigned long val = 0;
    if (sscanf(name, "f_%lu.dat", &val) == 1 && val > 0) {
        *seq_out = (uint32_t)val;
        return true;
    }
    return false;
}

static void make_spool_path(uint32_t seq, char *out, size_t out_sz)
{
    snprintf(out, out_sz, SPOOL_BASE_PATH "/f_%lu.dat", (unsigned long)seq);
}

static void scan_spool_bounds_locked(size_t *count_out, size_t *bytes_out, uint32_t *min_seq_out)
{
    size_t cnt = 0;
    size_t bytes = 0;
    uint32_t min_seq = UINT32_MAX;

    if (s_mounted) {
        DIR *dir = opendir(SPOOL_BASE_PATH);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                uint32_t seq = 0;
                if (!parse_spool_filename(ent->d_name, &seq)) {
                    continue;
                }
                char path[64];
                make_spool_path(seq, path, sizeof(path));
                struct stat st;
                if (stat(path, &st) == 0) {
                    cnt++;
                    bytes += (size_t)st.st_size;
                    if (seq < min_seq) {
                        min_seq = seq;
                    }
                }
            }
            closedir(dir);
        }
    }

    if (count_out) *count_out = cnt;
    if (bytes_out) *bytes_out = bytes;
    if (min_seq_out) *min_seq_out = (cnt > 0) ? min_seq : 0;
}

static void prune_oldest_locked(size_t needed_bytes, uint32_t max_files)
{
    while (s_mounted) {
        size_t cnt = 0;
        uint32_t oldest = 0;
        scan_spool_bounds_locked(&cnt, NULL, &oldest);
        if (cnt == 0 || oldest == 0) {
            break;
        }

        size_t total = 0, used = 0;
        esp_spiffs_info("spool", &total, &used);
        size_t free_bytes = (total > used) ? (total - used) : 0;

        if (cnt < max_files && free_bytes >= needed_bytes + 16384) {
            break;
        }

        char path[64];
        make_spool_path(oldest, path, sizeof(path));
        unlink(path);
        ESP_LOGW(TAG, "Spool full; pruned oldest offline frame #%lu", (unsigned long)oldest);
    }
}

static uint8_t pack_flags(const gw_capture_meta_t *m)
{
    uint8_t f = 0;
    if (m->found_esc_at) f |= 0x01;
    if (m->found_esc_k)  f |= 0x02;
    if (m->found_cr)     f |= 0x04;
    if (m->found_lf)     f |= 0x08;
    if (m->found_esc_l)  f |= 0x10;
    if (m->found_esc_1)  f |= 0x20;
    return f;
}

static void unpack_flags(uint8_t f, gw_capture_meta_t *m)
{
    m->found_esc_at = (f & 0x01) != 0;
    m->found_esc_k  = (f & 0x02) != 0;
    m->found_cr     = (f & 0x04) != 0;
    m->found_lf     = (f & 0x08) != 0;
    m->found_esc_l  = (f & 0x10) != 0;
    m->found_esc_1  = (f & 0x20) != 0;
}

esp_err_t spool_manager_enqueue(gw_capture_meta_t *meta, const uint8_t *raw_data, size_t raw_len)
{
    if (!meta || !raw_data || raw_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t crc = esp_rom_crc32_le(0, raw_data, (uint32_t)raw_len);
    meta->crc32 = crc;

    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        return ESP_ERR_NO_MEM;
    }
    app_config_get(cfg);

    xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
    meta->seq = alloc_next_seq_locked();
    meta->ack_state = GW_ACK_QUEUED;
    meta->attempts = 0;
    meta->mqtt_published = false;

    esp_err_t err = ESP_OK;
    if (s_mounted && cfg->auto_publish_mqtt) {
        prune_oldest_locked(sizeof(spool_file_hdr_t) + raw_len, cfg->max_spool_files);

        char path[64];
        make_spool_path(meta->seq, path, sizeof(path));
        FILE *fp = fopen(path, "wb");
        if (fp) {
            spool_file_hdr_t hdr = {
                .magic = SPOOL_MAGIC,
                .seq = meta->seq,
                .crc32 = crc,
                .byte_len = (uint32_t)raw_len,
                .timestamp_ms = meta->timestamp_ms,
                .duration_ms = meta->duration_ms,
                .rate_bps = meta->rate_bps,
                .esc_k_count = (uint16_t)meta->esc_k_count,
                .raster_stripes = (uint16_t)meta->raster_stripes,
                .flags = pack_flags(meta),
                .attempts = 0,
            };
            app_config_safe_copy(hdr.end_reason, sizeof(hdr.end_reason), meta->end_reason);

            size_t w1 = fwrite(&hdr, 1, sizeof(hdr), fp);
            size_t w2 = fwrite(raw_data, 1, raw_len, fp);
            fclose(fp);

            if (w1 != sizeof(hdr) || w2 != raw_len) {
                unlink(path);
                err = ESP_FAIL;
                ESP_LOGE(TAG, "Spool write failed for frame #%lu", (unsigned long)meta->seq);
            } else {
                ESP_LOGI(TAG, "Spooled frame #%lu (%zu B, CRC32=%08lX) to flash",
                         (unsigned long)meta->seq, raw_len, (unsigned long)crc);
            }
        } else {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "Failed opening spool file %s", path);
        }
    }
    xSemaphoreGive(s_spool_mutex);
    free(cfg);

    gateway_state_record_capture(meta, raw_data, raw_len);

    if (s_sync_task) {
        xTaskNotifyGive(s_sync_task);
    }
    return err;
}

void spool_manager_on_server_ack(uint32_t seq, bool ok, bool has_crc, uint32_t server_crc, const char *reason)
{
    if (seq == 0) {
        return;
    }

    if (s_wait_seq == seq && s_wait_result == WAIT_PENDING) {
        if (ok && has_crc && server_crc != s_wait_crc) {
            ESP_LOGW(TAG, "Server ACK CRC mismatch for #%lu (local=%08lX, server=%08lX)",
                     (unsigned long)seq, (unsigned long)s_wait_crc, (unsigned long)server_crc);
            s_wait_result = WAIT_ACK_NACK;
        } else if (ok) {
            ESP_LOGI(TAG, "Server ACK confirmed for frame #%lu (CRC32=%08lX)",
                     (unsigned long)seq, (unsigned long)s_wait_crc);
            s_wait_result = WAIT_ACK_OK;
        } else {
            ESP_LOGW(TAG, "Server NACK for frame #%lu (%s)",
                     (unsigned long)seq, reason ? reason : "error");
            s_wait_result = WAIT_ACK_NACK;
        }
        xSemaphoreGive(s_ack_sem);
    } else {
        ESP_LOGD(TAG, "Late/unmatched ACK for frame #%lu (ok=%d)", (unsigned long)seq, ok);
    }
}

void spool_manager_trigger_sync(void)
{
    if (s_sync_task) {
        xTaskNotifyGive(s_sync_task);
    }
}

esp_err_t spool_manager_purge_all(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
    DIR *dir = opendir(SPOOL_BASE_PATH);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            uint32_t seq = 0;
            if (parse_spool_filename(ent->d_name, &seq)) {
                char path[64];
                make_spool_path(seq, path, sizeof(path));
                unlink(path);
            }
        }
        closedir(dir);
    }
    s_active_seq = 0;
    snprintf(s_sync_state, sizeof(s_sync_state), "purged");
    xSemaphoreGive(s_spool_mutex);
    ESP_LOGW(TAG, "Offline spool purged");
    return ESP_OK;
}

void spool_manager_get_status(gw_spool_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mounted = s_mounted;
    if (!s_spool_mutex) {
        return;
    }

    xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
    if (s_mounted) {
        esp_spiffs_info("spool", &out->total_bytes, &out->used_bytes);
        scan_spool_bounds_locked(&out->pending_count, &out->pending_bytes, NULL);
    }
    out->active_seq = s_active_seq;
    app_config_safe_copy(out->sync_state, sizeof(out->sync_state), s_sync_state);
    xSemaphoreGive(s_spool_mutex);
}

static int cmp_spool_item(const void *a, const void *b)
{
    const gw_spool_item_t *ia = (const gw_spool_item_t *)a;
    const gw_spool_item_t *ib = (const gw_spool_item_t *)b;
    if (ia->seq < ib->seq) return -1;
    if (ia->seq > ib->seq) return 1;
    return 0;
}

size_t spool_manager_list_pending(gw_spool_item_t *out, size_t max_items)
{
    if (!out || max_items == 0 || !s_mounted || !s_spool_mutex) {
        return 0;
    }

    xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
    size_t count = 0;
    DIR *dir = opendir(SPOOL_BASE_PATH);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < max_items) {
            uint32_t seq = 0;
            if (!parse_spool_filename(ent->d_name, &seq)) {
                continue;
            }
            char path[64];
            make_spool_path(seq, path, sizeof(path));
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                continue;
            }
            spool_file_hdr_t hdr;
            if (fread(&hdr, 1, sizeof(hdr), fp) == sizeof(hdr) && hdr.magic == SPOOL_MAGIC) {
                out[count].seq = hdr.seq;
                out[count].crc32 = hdr.crc32;
                out[count].byte_len = hdr.byte_len;
                out[count].timestamp_ms = hdr.timestamp_ms;
                out[count].attempts = hdr.attempts;
                count++;
            }
            fclose(fp);
        }
        closedir(dir);
    }
    xSemaphoreGive(s_spool_mutex);

    if (count > 1) {
        qsort(out, count, sizeof(gw_spool_item_t), cmp_spool_item);
    }
    return count;
}

static void sync_task(void *arg)
{
    (void)arg;
    gw_config_t *cfg = (gw_config_t *)malloc(sizeof(gw_config_t));
    if (!cfg) {
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

        while (1) {
            app_config_get(cfg);
            if (!s_mounted || !cfg->mqtt_enabled || !mqtt_manager_is_connected()) {
                xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
                size_t pending = 0;
                scan_spool_bounds_locked(&pending, NULL, NULL);
                s_active_seq = 0;
                snprintf(s_sync_state, sizeof(s_sync_state),
                         pending > 0 ? "offline_queued" : "offline");
                xSemaphoreGive(s_spool_mutex);
                break;
            }

            uint32_t next_seq = 0;
            xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
            scan_spool_bounds_locked(NULL, NULL, &next_seq);
            if (next_seq == 0) {
                s_active_seq = 0;
                snprintf(s_sync_state, sizeof(s_sync_state), "synced");
                xSemaphoreGive(s_spool_mutex);
                break;
            }
            s_active_seq = next_seq;
            snprintf(s_sync_state, sizeof(s_sync_state), "syncing");

            char path[64];
            make_spool_path(next_seq, path, sizeof(path));
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                xSemaphoreGive(s_spool_mutex);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            }

            spool_file_hdr_t hdr;
            size_t r_hdr = fread(&hdr, 1, sizeof(hdr), fp);
            if (r_hdr != sizeof(hdr) || hdr.magic != SPOOL_MAGIC || hdr.byte_len == 0 || hdr.byte_len > GW_LATEST_RAW_MAX_BYTES) {
                fclose(fp);
                unlink(path);
                xSemaphoreGive(s_spool_mutex);
                gateway_state_inc_crc_error();
                ESP_LOGE(TAG, "Corrupt spool header for #%lu; removed", (unsigned long)next_seq);
                continue;
            }

            uint8_t *raw = (uint8_t *)malloc(hdr.byte_len);
            if (!raw) {
                fclose(fp);
                xSemaphoreGive(s_spool_mutex);
                ESP_LOGE(TAG, "OOM reading #%lu (%lu B)", (unsigned long)next_seq, (unsigned long)hdr.byte_len);
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;
            }

            size_t r_data = fread(raw, 1, hdr.byte_len, fp);
            fclose(fp);

            uint32_t calc_crc = (r_data == hdr.byte_len) ? esp_rom_crc32_le(0, raw, hdr.byte_len) : 0;
            if (r_data != hdr.byte_len || calc_crc != hdr.crc32) {
                free(raw);
                unlink(path);
                xSemaphoreGive(s_spool_mutex);
                gateway_state_inc_crc_error();
                ESP_LOGE(TAG, "Flash CRC mismatch on #%lu (expected=%08lX, got=%08lX); dropped",
                         (unsigned long)next_seq, (unsigned long)hdr.crc32, (unsigned long)calc_crc);
                continue;
            }

            if (hdr.attempts < 250) {
                hdr.attempts++;
            }
            fp = fopen(path, "r+b");
            if (fp) {
                fwrite(&hdr, 1, sizeof(hdr), fp);
                fclose(fp);
            }
            xSemaphoreGive(s_spool_mutex);

            gw_capture_meta_t meta = {
                .seq = hdr.seq,
                .crc32 = hdr.crc32,
                .timestamp_ms = hdr.timestamp_ms,
                .byte_len = hdr.byte_len,
                .duration_ms = hdr.duration_ms,
                .rate_bps = hdr.rate_bps,
                .esc_k_count = hdr.esc_k_count,
                .raster_stripes = hdr.raster_stripes,
                .mqtt_published = false,
                .ack_state = GW_ACK_QUEUED,
                .attempts = hdr.attempts,
            };
            unpack_flags(hdr.flags, &meta);
            app_config_safe_copy(meta.end_reason, sizeof(meta.end_reason), hdr.end_reason);

            /* Arm server ACK waiter before publishing to avoid race on fast servers */
            while (xSemaphoreTake(s_ack_sem, 0) == pdTRUE) {
            }
            s_wait_seq = hdr.seq;
            s_wait_crc = hdr.crc32;
            s_wait_result = cfg->require_server_ack ? WAIT_PENDING : WAIT_NONE;

            bool pub_ok = mqtt_manager_publish_capture(&meta, raw, hdr.byte_len);
            free(raw);

            if (!pub_ok) {
                s_wait_result = WAIT_NONE;
                s_wait_seq = 0;
                gateway_state_update_delivery(hdr.seq, false, GW_ACK_QUEUED, hdr.attempts);
                xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
                snprintf(s_sync_state, sizeof(s_sync_state), "retry_wait");
                xSemaphoreGive(s_spool_mutex);
                vTaskDelay(pdMS_TO_TICKS(3000));
                break;
            }

            if (!cfg->require_server_ack) {
                s_wait_result = WAIT_NONE;
                s_wait_seq = 0;
                gateway_state_update_delivery(hdr.seq, true, GW_ACK_BROKER_ONLY, hdr.attempts);
                xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
                unlink(path);
                xSemaphoreGive(s_spool_mutex);
                continue;
            }

            gateway_state_update_delivery(hdr.seq, true, GW_ACK_WAIT_SERVER, hdr.attempts);
            xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
            snprintf(s_sync_state, sizeof(s_sync_state), "wait_ack");
            xSemaphoreGive(s_spool_mutex);

            BaseType_t got_ack = xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(cfg->ack_timeout_ms));
            ack_wait_result_t res = s_wait_result;
            s_wait_result = WAIT_NONE;
            s_wait_seq = 0;

            if (got_ack == pdTRUE && res == WAIT_ACK_OK) {
                gateway_state_inc_server_ack(true);
                gateway_state_update_delivery(hdr.seq, true, GW_ACK_OK, hdr.attempts);
                xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
                unlink(path);
                xSemaphoreGive(s_spool_mutex);
                continue;
            }

            if (got_ack == pdTRUE && res == WAIT_ACK_NACK) {
                gateway_state_inc_server_ack(false);
                gateway_state_update_delivery(hdr.seq, true, GW_ACK_NACK, hdr.attempts);
                xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
                snprintf(s_sync_state, sizeof(s_sync_state), "nack_retry");
                xSemaphoreGive(s_spool_mutex);
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }

            ESP_LOGW(TAG, "Server ACK timeout (%lums) for frame #%lu; retrying",
                     (unsigned long)cfg->ack_timeout_ms, (unsigned long)hdr.seq);
            gateway_state_update_delivery(hdr.seq, true, GW_ACK_QUEUED, hdr.attempts);
            xSemaphoreTake(s_spool_mutex, portMAX_DELAY);
            snprintf(s_sync_state, sizeof(s_sync_state), "ack_timeout");
            xSemaphoreGive(s_spool_mutex);
            vTaskDelay(pdMS_TO_TICKS(3000));
            break;
        }
    }
}

esp_err_t spool_manager_init(void)
{
    if (!s_spool_mutex) {
        s_spool_mutex = xSemaphoreCreateMutex();
    }
    if (!s_ack_sem) {
        s_ack_sem = xSemaphoreCreateBinary();
    }

    nvs_handle_t h;
    if (nvs_open(NVS_SEQ_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t saved = 1;
        if (nvs_get_u32(h, NVS_SEQ_KEY, &saved) == ESP_OK && saved > 0) {
            s_next_seq = saved;
        }
        nvs_close(h);
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = SPOOL_BASE_PATH,
        .partition_label = "spool",
        .max_files = 6,
        .format_if_mount_failed = true,
    };

    ESP_LOGI(TAG, "Mounting SPIFFS spool partition...");
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS spool (%s)", esp_err_to_name(err));
        s_mounted = false;
    } else {
        s_mounted = true;
        size_t total = 0, used = 0, pending = 0;
        esp_spiffs_info("spool", &total, &used);
        scan_spool_bounds_locked(&pending, NULL, NULL);
        ESP_LOGI(TAG, "Spool mounted (%zu/%zu KB used, %zu pending frames, next_seq=%lu)",
                 used / 1024, total / 1024, pending, (unsigned long)s_next_seq);
    }

    xTaskCreate(sync_task, "spool_sync", 6144, NULL, 6, &s_sync_task);
    return ESP_OK;
}
