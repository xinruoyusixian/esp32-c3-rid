/**
 * crid_tracker.c — 无人机追踪表（线程安全，堆上动态分配）
 */

#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "crid_tracker.h"
#include "crid_json.h"

static const char *TAG = "TRACKER";

/* ---- 模块内部状态 ---- */

static SemaphoreHandle_t g_tracker_mutex = NULL;
static uav_track_t      *g_uavs = NULL;      /* 堆上分配，切换模式时释放 */
static int               g_capacity = 0;     /* 实际可用条目数 */
static uint32_t          g_start_time_ms = 0;

/* 内存不足时的逐级降级容量 */
static const int s_cap_ladder[] = { MAX_TRACKED_UAVS, 60, 40 };

/* ---- 新设备事件（环形队列，供外部 REST 接口增量拉取/推送） ---- */
static crid_new_ev_t g_new_ev[CRID_NEW_EV_MAX];
static int           g_ev_head  = 0;   /* 下一个写入位置 */
static int           g_ev_count = 0;   /* 队列内有效事件数 */
static uint32_t      g_new_seq  = 0;   /* 全局新设备序号，永不清零 */

/* 弱符号默认实现：未接推送模块时为空操作 */
__attribute__((weak)) void crid_tracker_on_new_uav(const uint8_t *mac, uint32_t seq) {
    (void)mac;
    (void)seq;
}

/* ---- MAC 比较 ---- */

static inline bool mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

/* ---- 内部辅助 ---- */
static const uav_track_t *crid_tracker_find_by_mac_locked(const uint8_t *mac);

/* ---- 公开接口 ---- */

esp_err_t crid_tracker_init(void) {
    if (g_uavs != NULL) return ESP_OK;   /* 已就绪，幂等 */

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    for (size_t i = 0; i < sizeof(s_cap_ladder) / sizeof(s_cap_ladder[0]); i++) {
        int cap = s_cap_ladder[i];
        if (cap <= 0) continue;

        uav_track_t *tbl = heap_caps_calloc((size_t)cap, sizeof(uav_track_t), MALLOC_CAP_8BIT);
        if (tbl == NULL) {
            ESP_LOGW(TAG, "alloc %d slots (%u B) failed, try smaller (free=%u)",
                     cap, (unsigned)(cap * sizeof(uav_track_t)),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }

        g_uavs = tbl;
        g_capacity = cap;
        g_start_time_ms = esp_log_timestamp();

        if (g_tracker_mutex == NULL) {
            g_tracker_mutex = xSemaphoreCreateMutex();
            if (g_tracker_mutex == NULL) {
                json_error("RID_TRACK", "Failed to create tracker mutex!");
            }
        }

        ESP_LOGI(TAG, "tracker ready: %d slots, %u B, heap %u -> %u",
                 g_capacity, (unsigned)(cap * sizeof(uav_track_t)),
                 (unsigned)free_before,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "tracker init failed: not enough internal RAM");
    return ESP_ERR_NO_MEM;
}

void crid_tracker_deinit(void) {
    if (g_uavs != NULL) {
        heap_caps_free(g_uavs);
        g_uavs = NULL;
        ESP_LOGI(TAG, "tracker released, heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
    g_capacity = 0;
    /* 互斥锁保留，避免与其它任务的取锁竞争；切回 RID 时继续复用 */
}

bool crid_tracker_ready(void) {
    return g_uavs != NULL;
}

int crid_tracker_get_capacity(void) {
    return g_capacity;
}

SemaphoreHandle_t crid_tracker_get_mutex(void) {
    return g_tracker_mutex;
}

uav_track_t *crid_tracker_find_or_create(const uint8_t *mac) {
    if (g_uavs == NULL || g_capacity <= 0) return NULL;

    uav_track_t *free_slot = NULL;

    for (int i = 0; i < g_capacity; i++) {
        if (g_uavs[i].active) {
            if (mac_equal(g_uavs[i].mac, mac)) {
                return &g_uavs[i];
            }
        } else if (free_slot == NULL) {
            free_slot = &g_uavs[i];
        }
    }

    if (free_slot) {
        memset(free_slot, 0, sizeof(uav_track_t));
        memcpy(free_slot->mac, mac, 6);
        free_slot->active = true;
        free_slot->first_seen_ms = esp_log_timestamp();
        odid_initUasData(&free_slot->uas_data);

        /* 记录"新设备"事件：seq 单调递增，外部据此做增量拉取 */
        uint32_t seq = ++g_new_seq;
        g_new_ev[g_ev_head].seq = seq;
        memcpy(g_new_ev[g_ev_head].mac, mac, 6);
        g_new_ev[g_ev_head].ts_ms = free_slot->first_seen_ms;
        g_ev_head = (g_ev_head + 1) % CRID_NEW_EV_MAX;
        if (g_ev_count < CRID_NEW_EV_MAX) g_ev_count++;

        /* 推送到外部服务：仅入队，不在此处做阻塞 IO */
        crid_tracker_on_new_uav(mac, seq);
        return free_slot;
    }

    return NULL;  // 追踪表已满
}

int crid_tracker_get_active_count(void) {
    if (g_uavs == NULL) return 0;
    int count = 0;
    for (int i = 0; i < g_capacity; i++) {
        if (g_uavs[i].active) count++;
    }
    return count;
}

uav_track_t *crid_tracker_get_table(void) {
    return g_uavs;
}

void crid_tracker_cleanup(uint32_t timeout_ms) {
    if (g_uavs == NULL) return;
    uint32_t now = esp_log_timestamp();
    for (int i = 0; i < g_capacity; i++) {
        if (!g_uavs[i].active) continue;
        uint32_t age_ms = now - g_uavs[i].last_seen_ms;
        if (age_ms > timeout_ms) {
            json_uav_timeout(g_uavs[i].mac);
            g_uavs[i].active = false;
        }
    }
}

/* ================================================================
 * 新设备事件 / 外部接口支撑
 * ================================================================ */

uint32_t crid_tracker_get_new_seq(void) {
    return g_new_seq;
}

int crid_tracker_snapshot_new(uint32_t since_seq, uav_track_t *out, int max_count,
                              uint32_t *out_seq, int *out_miss) {
    int filled = 0;
    uint32_t cursor = since_seq;

    if (out_seq) *out_seq = since_seq;
    if (out_miss) *out_miss = 0;
    if (g_uavs == NULL || g_capacity <= 0) return 0;

    /* 队列内增量总数（含本次装不下的） */
    int avail = 0;
    int start = (g_ev_count > 0) ? (g_ev_head - g_ev_count + CRID_NEW_EV_MAX) % CRID_NEW_EV_MAX : 0;
    for (int i = 0; i < g_ev_count; i++) {
        if (g_new_ev[(start + i) % CRID_NEW_EV_MAX].seq > since_seq) avail++;
    }
    if (out_miss) {
        uint32_t total_new = g_new_seq - since_seq;      /* 期间产生的新设备数 */
        *out_miss = (total_new > (uint32_t)avail) ? (int)(total_new - (uint32_t)avail) : 0;
    }

    if (out == NULL || max_count <= 0) return 0;

    for (int i = 0; i < g_ev_count && filled < max_count; i++) {
        crid_new_ev_t *ev = &g_new_ev[(start + i) % CRID_NEW_EV_MAX];
        if (ev->seq <= since_seq) continue;

        const uav_track_t *src = crid_tracker_find_by_mac_locked(ev->mac);
        if (src == NULL) continue;                        /* 已被清空/超时回收 */

        memcpy(&out[filled], src, sizeof(uav_track_t));
        filled++;
        cursor = ev->seq;
    }

    if (out_seq) *out_seq = cursor;
    return filled;
}

/* 内部：调用方已持有 tracker 互斥锁 */
static const uav_track_t *crid_tracker_find_by_mac_locked(const uint8_t *mac) {
    if (g_uavs == NULL) return NULL;
    for (int i = 0; i < g_capacity; i++) {
        if (g_uavs[i].active && mac_equal(g_uavs[i].mac, mac)) return &g_uavs[i];
    }
    return NULL;
}

bool crid_tracker_copy_by_mac(const uint8_t *mac, uav_track_t *out) {
    if (out == NULL) return false;
    const uav_track_t *src = crid_tracker_find_by_mac_locked(mac);
    if (src == NULL) return false;
    memcpy(out, src, sizeof(uav_track_t));
    return true;
}

int crid_tracker_clear_all(void) {
    if (g_uavs == NULL) return 0;
    int n = 0;
    for (int i = 0; i < g_capacity; i++) {
        if (g_uavs[i].active) n++;
        memset(&g_uavs[i], 0, sizeof(uav_track_t));
    }
    /* 事件队列一并清空，seq 不回退（外部游标继续单调有效） */
    g_ev_head = 0;
    g_ev_count = 0;
    return n;
}
