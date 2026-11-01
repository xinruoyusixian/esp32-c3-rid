/**
 * crid_led.h — 板载蓝色指示灯（运行状态心跳）
 *
 * 硬件：ESP32-C3 SuperMini 板载蓝色 LED，接 GPIO8，低电平点亮。
 *
 * 作用：
 *   1. 用闪烁节奏直观反映固件运行状态（心跳）
 *   2. 暴露 level / blinks 给 Web 接口，前端可同步显示指示灯状态，
 *      若心跳计数停止增长即说明程序跑飞/卡死
 */

#ifndef CRID_LED_H
#define CRID_LED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 板载蓝色 LED 引脚（可按实际硬件调整） */
#ifndef CRID_LED_GPIO
#define CRID_LED_GPIO        8
#endif
/* SuperMini 的 LED 为低电平点亮 */
#ifndef CRID_LED_ACTIVE_LOW
#define CRID_LED_ACTIVE_LOW  1
#endif

typedef enum {
    CRID_LED_BOOT = 0,   /* 启动中：100ms 快闪 */
    CRID_LED_RID,        /* RID 侦听正常：1Hz 慢闪（心跳） */
    CRID_LED_CAM,        /* 蓝牙键盘（拍照）模式：双击闪（短-短-停） */
    CRID_LED_RELAY,      /* 网络中继模式：1Hz 慢闪 */
    CRID_LED_ERR,        /* 异常：5Hz 急闪 */
    CRID_LED_OFF,        /* 常灭 */
} crid_led_mode_t;

/** 初始化 GPIO 并启动指示灯任务 */
esp_err_t crid_led_init(void);

/** 设置工作模式对应的闪烁节奏 */
void crid_led_set_mode(crid_led_mode_t mode);
crid_led_mode_t crid_led_get_mode(void);

/** 当前状态的可读名（英文枚举串，供 JSON 使用） */
const char *crid_led_mode_key(crid_led_mode_t mode);
/** 当前状态的中文名（供 Web 展示） */
const char *crid_led_mode_text(crid_led_mode_t mode);
/** 闪烁节奏说明 */
const char *crid_led_mode_pattern(crid_led_mode_t mode);

/** 当前电平：1=亮，0=灭 */
int crid_led_get_level(void);
/** 上电以来的闪烁（点亮）次数，用于判断任务是否还在跑 */
uint32_t crid_led_get_blinks(void);

/**
 * 业务任务心跳：RID 接收任务每轮调用一次。
 * 超过 3 秒没有调用则判定为卡死，指示灯切到急闪并上报异常。
 */
void crid_led_kick(void);

/** 业务任务是否在正常心跳（未卡死） */
bool crid_led_health_ok(void);

#ifdef __cplusplus
}
#endif

#endif // CRID_LED_H
