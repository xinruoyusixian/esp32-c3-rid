/**
 * crid_screen.h — 板载 OLED 屏幕编排
 *
 * 把三种工作模式的信息分别排版到 72x40 的屏幕上，并接管 BOOT 按键：
 *   单击   模式内翻页
 *   双击   模式内的快捷动作
 *   长按   切换到下一个工作模式（会重启生效）
 *   超长按 屏幕开关
 */

#ifndef CRID_SCREEN_H
#define CRID_SCREEN_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 屏幕 I2C 引脚（ABROBOT ESP32-C3 板载 OLED） */
#ifndef CRID_SCREEN_SDA_GPIO
#define CRID_SCREEN_SDA_GPIO 5
#endif
#ifndef CRID_SCREEN_SCL_GPIO
#define CRID_SCREEN_SCL_GPIO 6
#endif
#ifndef CRID_SCREEN_I2C_ADDR
#define CRID_SCREEN_I2C_ADDR 0x3C
#endif

/** BOOT 按键 GPIO */
#ifndef CRID_SCREEN_BTN_GPIO
#define CRID_SCREEN_BTN_GPIO 9
#endif

/**
 * 初始化屏幕与按键，并启动刷新任务。
 * 屏幕不存在时不会失败，仅关闭显示输出。
 */
esp_err_t crid_screen_init(void);

/** 当前显示到第几页（0 起） */
int crid_screen_get_page(void);
void crid_screen_set_page(int page);

/** 当前模式下有多少页 */
int crid_screen_get_page_count(void);

/** 当前 UI 层：0=实时页 1=主菜单 2=子菜单 3=详情 4=切换确认（调试/远程查看用） */
int  crid_screen_get_ui(void);
void crid_screen_set_ui(int ui);

/** 屏幕背光/显示开关状态 */
bool crid_screen_is_on(void);
void crid_screen_set_on(bool on);

#ifdef __cplusplus
}
#endif

#endif /* CRID_SCREEN_H */
