/**
 * crid_led.c — 板载蓝色指示灯（运行状态心跳）实现
 *
 * 独立低优先级任务驱动，20ms 节拍，用一个状态机产生不同的闪烁节奏；
 * 同时把电平 / 闪烁计数 / 健康状态暴露给 Web 接口，实现"网页指示灯
 * 与板载灯同步"，程序跑飞时心跳计数不再增长，前端可立即发现。
 */

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "crid_led.h"

static const char *TAG = "LED";

#define LED_TICK_MS          20      /* 状态机节拍 */
#define HEALTH_TIMEOUT_MS    3000    /* 业务任务心跳超时 */

/* ---- 内部状态 ---- */

static volatile crid_led_mode_t s_mode = CRID_LED_BOOT;
static volatile int             s_level = 0;
static volatile uint32_t        s_blinks = 0;
static volatile uint32_t        s_last_kick = 0;
static volatile bool            s_started = false;

static inline uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

static void led_write(int on)
{
    int level = CRID_LED_ACTIVE_LOW ? !on : !!on;
    gpio_set_level((gpio_num_t)CRID_LED_GPIO, level);
    s_level = on ? 1 : 0;
    if (on) s_blinks++;
}

/* ---- 闪烁节奏 ---- */

typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
    uint16_t gap_ms;    /* 一次闪烁结束后的额外停顿（0 表示连续闪） */
} led_pattern_t;

static led_pattern_t pattern_of(crid_led_mode_t m)
{
    switch (m) {
        case CRID_LED_RID:  return (led_pattern_t){ 450, 450, 0 };    /* 1Hz 心跳 */
        case CRID_LED_CAM:  return (led_pattern_t){  80, 120, 1200 }; /* 双击 + 长停 */
        case CRID_LED_RELAY:return (led_pattern_t){ 450, 450, 0 };    /* 1Hz 心跳 */
        case CRID_LED_ERR:  return (led_pattern_t){ 100, 100, 0 };    /* 5Hz 急闪 */
        case CRID_LED_BOOT: return (led_pattern_t){  60, 60, 0 };     /* 启动快闪 */
        case CRID_LED_OFF:
        default:            return (led_pattern_t){   0,   0, 0 };
    }
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t phase_start = now_ms();
    uint32_t gap_until = 0;
    int step = 0;   /* 0=灭 1=亮 */

    for (;;) {
        uint32_t now = now_ms();
        crid_led_mode_t want = s_mode;

        /* 业务任务卡死 → 覆盖为急闪异常态（BOOT/OFF 不动） */
        if ((want == CRID_LED_RID) && !crid_led_health_ok()) {
            want = CRID_LED_ERR;
        }

        if (want != s_mode) {
            s_mode = want;
            step = 0;
            gap_until = 0;
            phase_start = now;
            led_write(0);
        }

        led_pattern_t p = pattern_of(want);

        if (p.on_ms == 0 && p.off_ms == 0) {
            /* 常灭 */
            if (s_level) led_write(0);
            vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
            continue;
        }

        if (gap_until && now < gap_until) {
            vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
            continue;
        }
        gap_until = 0;

        uint32_t elapsed = now - phase_start;
        if (step == 0) {
            if (elapsed >= p.off_ms) { led_write(1); step = 1; phase_start = now; }
        } else {
            if (elapsed >= p.on_ms) {
                led_write(0);
                step = 0;
                phase_start = now;
                if (p.gap_ms) gap_until = now + p.gap_ms;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

/* ---- 公开接口 ---- */

esp_err_t crid_led_init(void)
{
    if (s_started) return ESP_OK;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CRID_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    led_write(0);
    s_last_kick = now_ms();
    s_mode = CRID_LED_BOOT;

    if (xTaskCreate(led_task, "led", 2048, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create led task failed");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "blue LED ready on GPIO%d (active %s)",
             CRID_LED_GPIO, CRID_LED_ACTIVE_LOW ? "low" : "high");
    return ESP_OK;
}

void crid_led_set_mode(crid_led_mode_t mode)
{
    if (s_mode == mode) return;
    s_mode = mode;
    ESP_LOGI(TAG, "pattern -> %s (%s)", crid_led_mode_key(mode), crid_led_mode_pattern(mode));
}

crid_led_mode_t crid_led_get_mode(void) { return s_mode; }

const char *crid_led_mode_key(crid_led_mode_t mode)
{
    switch (mode) {
        case CRID_LED_RID:  return "rid";
        case CRID_LED_CAM:  return "camera";
        case CRID_LED_RELAY:return "relay";
        case CRID_LED_ERR:  return "error";
        case CRID_LED_BOOT: return "boot";
        case CRID_LED_OFF:  return "off";
        default:            return "unknown";
    }
}

const char *crid_led_mode_text(crid_led_mode_t mode)
{
    switch (mode) {
        case CRID_LED_RID:  return "运行中";
        case CRID_LED_CAM:  return "蓝牙键盘模式";
        case CRID_LED_RELAY:return "中继模式";
        case CRID_LED_ERR:  return "异常";
        case CRID_LED_BOOT: return "启动中";
        case CRID_LED_OFF:  return "已关闭";
        default:            return "未知";
    }
}

const char *crid_led_mode_pattern(crid_led_mode_t mode)
{
    switch (mode) {
        case CRID_LED_RID:  return "1Hz 慢闪";
        case CRID_LED_CAM:  return "双击闪";
        case CRID_LED_RELAY:return "1Hz 慢闪";
        case CRID_LED_ERR:  return "5Hz 急闪";
        case CRID_LED_BOOT: return "快闪";
        case CRID_LED_OFF:  return "常灭";
        default:            return "未知";
    }
}

int crid_led_get_level(void) { return s_level; }
uint32_t crid_led_get_blinks(void) { return s_blinks; }

void crid_led_kick(void)
{
    s_last_kick = now_ms();
}

bool crid_led_health_ok(void)
{
    if (!s_started) return false;
    return (now_ms() - s_last_kick) < HEALTH_TIMEOUT_MS;
}
