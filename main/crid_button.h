/**
 * crid_button.h — 单按键手势识别
 *
 * ABROBOT ESP32-C3 板载 BOOT 按键接在 GPIO9（按下接地，内部上拉）。
 * 通过单击 / 双击 / 三击 / 长按 / 超长按 组合出足够多的操作，
 * 避免在只有一颗按键的小板上还要外接按钮。
 */

#ifndef CRID_BUTTON_H
#define CRID_BUTTON_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRID_BTN_NONE = 0,
    CRID_BTN_SINGLE,    /* 单击   */
    CRID_BTN_DOUBLE,    /* 双击   */
    CRID_BTN_TRIPLE,    /* 三击   */
    CRID_BTN_LONG,      /* 长按 ~700ms  */
    CRID_BTN_VERYLONG,  /* 超长按 ~2500ms */
} crid_btn_event_t;

/** 手势回调；在按键任务上下文中执行，请勿阻塞 */
typedef void (*crid_btn_cb_t)(crid_btn_event_t ev, void *arg);

/**
 * 启动按键检测任务。
 *
 * @param gpio 按键 GPIO（低电平有效，内部上拉）
 * @param cb   手势回调
 * @param arg  透传给回调的参数
 */
esp_err_t crid_button_init(int gpio, crid_btn_cb_t cb, void *arg);

/** 手势名称，便于日志输出 */
const char *crid_button_event_name(crid_btn_event_t ev);

#ifdef __cplusplus
}
#endif

#endif /* CRID_BUTTON_H */
