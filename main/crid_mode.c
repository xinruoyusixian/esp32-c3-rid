/**
 * crid_mode.c — RID 侦听 / BLE 拍照 双模式切换实现
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "crid_mode.h"
#include "crid_rx_types.h"
#include "crid_tracker.h"
#include "crid_sniffer.h"
#include "crid_parser.h"
#include "crid_json.h"
#include "crid_nvs.h"
#include "crid_shutter.h"
#include "crid_relay.h"
#include "crid_led.h"

static const char *TAG = "MODE";

#define MODE_NS             "ridmode"
#define KEY_MODE            "mode"
#define KEY_PENDING         "pending"
#define KEY_RETRY           "retry"

#define MODE_SWITCH_DELAY_MS  800   /* 先让 HTTP 响应送达再动手切换 */
#define MAX_BOOT_RETRY        2     /* 连续启动失败次数上限，超过则回退 */

/* ================================================================
 * 状态
 * ================================================================ */
static crid_mode_t       s_mode = CRID_MODE_RID;
static volatile bool     s_switching = false;
static volatile bool     s_rid_running = false;
static TaskHandle_t      s_rid_task = NULL;

const char *crid_mode_name(crid_mode_t mode)
{
    switch (mode) {
        case CRID_MODE_CAMERA: return "camera";
        case CRID_MODE_RELAY:  return "relay";
        default:               return "rid";
    }
}

crid_mode_t crid_mode_get(void) { return s_mode; }

bool crid_mode_is_switching(void) { return s_switching; }

/* ================================================================
 * NVS
 * ================================================================ */
static esp_err_t mode_nvs_load(crid_mode_t *mode, bool *has_pending,
                               crid_mode_t *pending, uint8_t *retry)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MODE_NS, NVS_READONLY, &h);
    uint8_t v = 0;

    *mode = CRID_MODE_RID;
    *has_pending = false;
    *pending = CRID_MODE_RID;
    *retry = 0;

    if (err != ESP_OK) return err;

    if (nvs_get_u8(h, KEY_MODE, &v) == ESP_OK && v <= 2) {
        *mode = (crid_mode_t)v;
    }
    if (nvs_get_u8(h, KEY_PENDING, &v) == ESP_OK && v <= 2) {
        *has_pending = true;
        *pending = (crid_mode_t)v;
    }
    if (nvs_get_u8(h, KEY_RETRY, &v) == ESP_OK) {
        *retry = v;
    }
    nvs_close(h);
    return ESP_OK;
}

static void mode_nvs_save(crid_mode_t mode, bool clear_pending, uint8_t retry,
                          bool set_retry)
{
    nvs_handle_t h;
    if (nvs_open(MODE_NS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_u8(h, KEY_MODE, (uint8_t)mode);
    if (clear_pending) nvs_erase_key(h, KEY_PENDING);
    if (set_retry) nvs_set_u8(h, KEY_RETRY, retry);
    else nvs_erase_key(h, KEY_RETRY);
    nvs_commit(h);
    nvs_close(h);
}

static void mode_nvs_set_pending(crid_mode_t target)
{
    nvs_handle_t h;
    if (nvs_open(MODE_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_PENDING, (uint8_t)target);
    nvs_commit(h);
    nvs_close(h);
}

/* ================================================================
 * RID 接收任务（原 app_main.c 中的 sniffer_task）
 * ================================================================ */
static void rid_rx_task(void *pv)
{
    (void)pv;

    sniffer_msg_t msg;
    uint32_t last_cleanup = 0;

    while (s_rid_running) {
        QueueHandle_t q = crid_sniffer_get_queue();
        if (q == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (xQueueReceive(q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            crid_led_kick();          /* 空闲也算存活心跳 */
            continue;
        }
        crid_led_kick();
        if (msg.msg_type != MSG_TYPE_RID) continue;

        SemaphoreHandle_t mtx = crid_tracker_get_mutex();
        if (mtx == NULL || !crid_tracker_ready()) continue;
        if (xSemaphoreTake(mtx, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        uav_track_t *uav = crid_tracker_find_or_create(msg.src_mac);
        if (!uav) {
            uint32_t now = esp_log_timestamp();
            if (now - last_cleanup >= 10000) {
                crid_tracker_cleanup(UAV_TIMEOUT_MS);
                last_cleanup = now;
                uav = crid_tracker_find_or_create(msg.src_mac);
            }
            if (!uav) { xSemaphoreGive(mtx); continue; }
        }

        bool was_new = (uav->msg_count == 0);
        uav->last_seen_ms = esp_log_timestamp();
        uav->msg_count++;
        uav->last_rssi = msg.rssi;
        uav->last_channel = msg.channel;
        memcpy(uav->oui, msg.oui, 3);
        uav->oui_type = msg.oui_type;
        uav->transport = (uint8_t)GET_RID_TRANSPORT(msg.oui[0], msg.oui[1], msg.oui[2]);

        rid_protocol_t proto = crid_parser_decode(uav, msg.data, msg.data_len);
        if (proto != RID_PROTOCOL_UNKNOWN) uav->protocol = (uint8_t)proto;

        crid_parser_extract_layered(uav);
        xSemaphoreGive(mtx);

        if (was_new && uav->basic_id.valid) {
            json_uav_discovery(uav);
            crid_nvs_save_uav(uav);
        }
        if (uav->basic_id.valid) {
            json_uav_update(uav);
        }
    }

    /* RID 模式常驻运行，模式切换一律通过重启完成，任务不会主动退出 */
    vTaskDelete(NULL);
}

/* ================================================================
 * 各模式启停
 * ================================================================ */
static esp_err_t rid_start(void)
{
    esp_err_t err = crid_tracker_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tracker init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = crid_sniffer_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sniffer init failed: %s", esp_err_to_name(err));
        crid_tracker_deinit();
        return err;
    }

    s_rid_running = true;
    if (s_rid_task == NULL) {
        if (xTaskCreate(rid_rx_task, "sniffer", 4096, NULL, 5, &s_rid_task) != pdPASS) {
            ESP_LOGE(TAG, "create rid rx task failed");
            s_rid_running = false;
            crid_sniffer_deinit();
            crid_tracker_deinit();
            return ESP_ERR_NO_MEM;
        }
    }

    crid_led_kick();
    crid_led_set_mode(CRID_LED_RID);

    ESP_LOGI(TAG, "RID mode on, slots=%d heap=%u",
             crid_tracker_get_capacity(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t camera_start(void)
{
    esp_err_t err = crid_shutter_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shutter init failed: %s", esp_err_to_name(err));
        return err;
    }
    crid_led_set_mode(CRID_LED_CAM);

    ESP_LOGI(TAG, "CAMERA mode on, heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

/* 中继：不启 tracker / NimBLE，内存最宽裕；上游未配置时返回错误交由 boot 回退 */
static esp_err_t relay_start(void)
{
    esp_err_t err = crid_relay_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "relay start failed: %s", esp_err_to_name(err));
        return err;
    }
    crid_led_set_mode(CRID_LED_RELAY);
    ESP_LOGI(TAG, "RELAY mode on, heap=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return ESP_OK;
}

static esp_err_t mode_start(crid_mode_t mode)
{
    switch (mode) {
        case CRID_MODE_CAMERA: return camera_start();
        case CRID_MODE_RELAY:  return relay_start();
        default:               return rid_start();
    }
}

/* ================================================================
 * 切换任务
 * ================================================================ */
static void mode_switch_task(void *arg)
{
    crid_mode_t target = (crid_mode_t)(intptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(MODE_SWITCH_DELAY_MS));   /* 先让 HTTP 响应送达 */

    ESP_LOGW(TAG, "switch %s -> %s by reboot", crid_mode_name(s_mode),
             crid_mode_name(target));

    /* 目标模式落盘为 pending：重启后由 crid_mode_boot() 直接以干净状态启动 */
    mode_nvs_set_pending(target);

    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

esp_err_t crid_mode_request(crid_mode_t target)
{
    if (target != CRID_MODE_RID && target != CRID_MODE_CAMERA &&
        target != CRID_MODE_RELAY) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_switching) return ESP_ERR_INVALID_STATE;
    if (target == s_mode) return ESP_OK;

    s_switching = true;
    if (xTaskCreate(mode_switch_task, "mode_switch", 4096,
                    (void *)(intptr_t)target, 6, NULL) != pdPASS) {
        s_switching = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ================================================================
 * 开机
 * ================================================================ */
esp_err_t crid_mode_boot(void)
{
    crid_mode_t saved = CRID_MODE_RID;
    crid_mode_t pending = CRID_MODE_RID;
    bool has_pending = false;
    uint8_t retry = 0;

    mode_nvs_load(&saved, &has_pending, &pending, &retry);

    crid_mode_t target = has_pending ? pending : saved;

    if (has_pending) {
        retry++;
        if (retry > MAX_BOOT_RETRY) {
            crid_mode_t fallback = (target == CRID_MODE_RID) ? CRID_MODE_CAMERA
                                                              : CRID_MODE_RID;
            ESP_LOGE(TAG, "%s failed %d times at boot, fallback to %s",
                     crid_mode_name(target), retry - 1, crid_mode_name(fallback));
            target = fallback;
            retry = 0;
        } else {
            ESP_LOGW(TAG, "resume pending %s (retry %d)",
                     crid_mode_name(target), retry);
            mode_nvs_save(saved, false, retry, true);   /* 保留 pending，记下重试次数 */
        }
    }

    ESP_LOGI(TAG, "booting in %s mode, heap=%u",
             crid_mode_name(target),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    esp_err_t err = mode_start(target);

    if (err != ESP_OK) {
        crid_mode_t fallback = (target == CRID_MODE_RID) ? CRID_MODE_CAMERA
                                                          : CRID_MODE_RID;
        ESP_LOGE(TAG, "%s start failed (%s), try %s",
                 crid_mode_name(target), esp_err_to_name(err),
                 crid_mode_name(fallback));
        err = mode_start(fallback);
        if (err == ESP_OK) {
            target = fallback;
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no mode could start: %s", esp_err_to_name(err));
        crid_led_set_mode(CRID_LED_ERR);   /* 两种模式都起不来：急闪告警 */
        return err;
    }

    s_mode = target;
    mode_nvs_save(target, true, 0, false);
    ESP_LOGI(TAG, "boot mode = %s", crid_mode_name(target));
    return ESP_OK;
}
