/**
 * crid_button.c — 单按键手势识别实现
 *
 * 采用 10ms 周期轮询 + 消抖，状态机识别：
 *   单击 / 双击 / 三击 / 长按(700ms) / 超长按(2500ms)
 * 长按在按住到达阈值时立即触发，无需等松手，手感更跟手。
 */

#include "crid_button.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BTN";

/* ---- 时序参数（毫秒） ---- */
#define DEBOUNCE_TICKS   3                   /* 3 x 10ms = 30ms 消抖 */
#define LONG_MS          700                 /* 长按阈值       */
#define VERYLONG_MS      2500                /* 超长按阈值     */
#define MULTI_GAP_MS     280                 /* 多击间隔窗口   */
#define POLL_MS          10

typedef enum {
    ST_IDLE = 0,
    ST_DOWN_1,
    ST_UP_1,
    ST_DOWN_2,
    ST_UP_2,
    ST_DOWN_3,
} btn_state_t;

static btn_state_t s_state   = ST_IDLE;
static int         s_gpio    = 9;
static uint32_t    s_t_down  = 0;
static uint32_t    s_t_up    = 0;
static uint8_t     s_deb     = 0;
static bool        s_raw_prev = false;
static bool        s_pressed = false;
static bool        s_long_fired  = false;
static bool        s_vlong_fired = false;
static crid_btn_cb_t s_cb    = NULL;
static void       *s_arg     = NULL;

const char *crid_button_event_name(crid_btn_event_t ev)
{
    switch (ev) {
        case CRID_BTN_SINGLE:   return "SINGLE";
        case CRID_BTN_DOUBLE:   return "DOUBLE";
        case CRID_BTN_TRIPLE:   return "TRIPLE";
        case CRID_BTN_LONG:     return "LONG";
        case CRID_BTN_VERYLONG: return "VERYLONG";
        default:                return "NONE";
    }
}

static void emit(crid_btn_event_t ev)
{
    ESP_LOGI(TAG, "gesture: %s", crid_button_event_name(ev));
    if (s_cb) s_cb(ev, s_arg);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void btn_task(void *pv)
{
    (void)pv;
    bool edge_down = false, edge_up = false;

    while (1) {
        bool raw = (gpio_get_level((gpio_num_t)s_gpio) == 0);  /* 低有效 */
        edge_down = edge_up = false;

        if (raw != s_raw_prev) {
            s_raw_prev = raw;
            s_deb = 0;
        } else if (s_deb < DEBOUNCE_TICKS) {
            s_deb++;
            if (s_deb == DEBOUNCE_TICKS && raw != s_pressed) {
                s_pressed = raw;
                if (raw) edge_down = true; else edge_up = true;
            }
        }

        uint32_t t = now_ms();

        switch (s_state) {
            case ST_IDLE:
                if (edge_down) {
                    s_t_down = t;
                    s_long_fired = false;
                    s_vlong_fired = false;
                    s_state = ST_DOWN_1;
                }
                break;

            case ST_DOWN_1:
                if (!s_vlong_fired && (t - s_t_down) >= VERYLONG_MS) {
                    s_vlong_fired = true;
                    emit(CRID_BTN_VERYLONG);
                } else if (!s_long_fired && !s_vlong_fired &&
                           (t - s_t_down) >= LONG_MS) {
                    s_long_fired = true;
                    emit(CRID_BTN_LONG);
                }
                if (edge_up) {
                    if (s_long_fired || s_vlong_fired) {
                        s_state = ST_IDLE;          /* 长按已触发，忽略收尾 */
                    } else {
                        s_t_up = t;
                        s_state = ST_UP_1;
                    }
                }
                break;

            case ST_UP_1:
                if (edge_down) {
                    s_t_down = t;
                    s_long_fired = false;
                    s_vlong_fired = false;
                    s_state = ST_DOWN_2;
                } else if ((t - s_t_up) > MULTI_GAP_MS) {
                    emit(CRID_BTN_SINGLE);
                    s_state = ST_IDLE;
                }
                break;

            case ST_DOWN_2:
                if (!s_long_fired && (t - s_t_down) >= LONG_MS) {
                    s_long_fired = true;
                    emit(CRID_BTN_LONG);
                }
                if (edge_up) {
                    if (s_long_fired) {
                        s_state = ST_IDLE;
                    } else {
                        s_t_up = t;
                        s_state = ST_UP_2;
                    }
                }
                break;

            case ST_UP_2:
                if (edge_down) {
                    s_t_down = t;
                    s_state = ST_DOWN_3;
                } else if ((t - s_t_up) > MULTI_GAP_MS) {
                    emit(CRID_BTN_DOUBLE);
                    s_state = ST_IDLE;
                }
                break;

            case ST_DOWN_3:
                if (edge_up) {
                    emit(CRID_BTN_TRIPLE);
                    s_state = ST_IDLE;
                }
                break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t crid_button_init(int gpio, crid_btn_cb_t cb, void *arg)
{
    s_gpio = gpio;
    s_cb   = cb;
    s_arg  = arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (gpio_num_t)gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) return e;

    s_raw_prev = (gpio_get_level((gpio_num_t)gpio) == 0);
    s_pressed  = s_raw_prev;
    s_deb      = 0;

    BaseType_t ok = xTaskCreate(btn_task, "crid_btn", 2048, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "button on GPIO%d (active low)", gpio);
    return ESP_OK;
}
