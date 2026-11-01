/**
 * crid_oled.h — 0.42" OLED 驱动（SSD1306，有效分辨率 72x40）
 *
 * ABROBOT ESP32-C3 板载屏幕特性：
 *   - 控制器 SSD1306，I2C 地址 0x3C，SDA=GPIO5，SCL=GPIO6
 *   - 内部 GDDRAM 仍按 128x64 组织，实际面板只有 72x40
 *   - 可见窗口起始偏移可在运行时调整（默认 30,12，实测批次多为 28,24）；
 *     驱动按 GDDRAM 全 8 页分配缓冲，绘制时 y 自动加 y 偏移，
 *     列/页偏移交给 SSD1306 的窗口地址命令，偏移一改即刻生效。
 */

#ifndef CRID_OLED_H
#define CRID_OLED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* 逻辑分辨率：业务代码只面对 72x40 坐标系 */
#define CRID_OLED_W      72
#define CRID_OLED_HEIGHT 40

/* 帧缓冲页数：行 12..51 覆盖第 1..6 页（行 8..55） */
/* 帧缓冲覆盖 GDDRAM 全部 8 页（行 0..63）。
   这样 y 偏移可以任意调整：可见窗口落在哪几页，就刷哪几页，
   不会像早期版本那样把页范围写死导致底部从未被写入（花屏）。 */
#define CRID_OLED_PAGES  8
#define CRID_OLED_BUFSZ  (CRID_OLED_W * CRID_OLED_PAGES)

/* 可见窗口偏移，可用 menuconfig / 编译宏覆盖 */
/* 实测值：ABROBOT 批次面板顶部对齐于 (28, 24) —— 资料给的 (30,12) 偏上 */
#ifndef CRID_OLED_X_OFFSET
#define CRID_OLED_X_OFFSET 28
#endif
#ifndef CRID_OLED_Y_OFFSET
#define CRID_OLED_Y_OFFSET 24
#endif

/**
 * 初始化 I2C 与 SSD1306。重复调用安全。
 */
esp_err_t crid_oled_init(int sda_gpio, int scl_gpio, uint8_t i2c_addr);

/**
 * @brief 显示自检 / 校准图案，并锁住屏幕刷新任务（防止被正常界面冲掉）
 *
 * mode: 0=退出校准（解锁并清屏） 1=填满整个帧缓冲 2=填满逻辑 72x40
 *       3=边框+四角块+中心十字（校准用） 4=边框+当前偏移数值
 */
void crid_oled_test_pattern(int mode);

/** 是否正处于校准图案锁定状态（屏幕刷新任务据此让行） */
bool crid_oled_test_locked(void);

/**
 * @brief 帧缓冲互斥，包住"清空 → 绘制 → flush"整个序列
 *
 * 屏幕刷新任务和 HTTP 校准接口会并发改帧缓冲，不加锁会出现
 * 边框画上去了、中间却残留上一帧文字的撕裂帧。
 */
void crid_oled_lock(void);
void crid_oled_unlock(void);

/** 屏幕是否可用（I2C 上探测到设备） */
bool crid_oled_ready(void);

/**
 * @brief 把当前显存按逻辑坐标导成 ASCII 图（排障用，无需看物理屏）
 *
 * 每个像素一个字符：点亮为 '#'，熄灭为 '.'，每行结尾换行。
 * 输出尺寸为 CRID_OLED_W x CRID_OLED_HEIGHT，另加每行换行符与结尾 NUL；
 * 缓冲区不足时返回 ESP_ERR_NO_MEM。
 */
esp_err_t crid_oled_dump(char *out, size_t n);

/** 读取逻辑坐标处的像素值（0/1），越界返回 0 */
int crid_oled_get_pixel(int x, int y);

/** 运行时调整可见窗口偏移（用于校准不同批次的面板） */
void crid_oled_set_offset(int xoff, int yoff);
int  crid_oled_get_x_offset(void);
int  crid_oled_get_y_offset(void);

/* ---- 绘制 API（全部操作内部缓冲，需调用 flush 才会显示） ---- */
void crid_oled_clear(void);
void crid_oled_pixel(int x, int y, int on);
void crid_oled_text(int x, int y, const char *s, int inverse);
void crid_oled_hline(int x, int y, int w, int on);
void crid_oled_rect(int x, int y, int w, int h, int on);
void crid_oled_fill(int x, int y, int w, int h, int on);

/** 将缓冲刷到屏幕 */
void crid_oled_flush(void);

/** 对比度 0..255 */
void crid_oled_contrast(uint8_t v);

/** 屏幕开关（关闭时进入休眠，保留显存） */
void crid_oled_display_on(bool on);

/**
 * 自检图案：
 *   0 清除
 *   1 整个帧缓冲全亮（含窗口外的部分，用来看面板实际能显示到哪）
 *   2 仅逻辑区域 72x40 全亮（用来验证偏移是否对准）
 */
void crid_oled_test_pattern(int mode);

#endif /* CRID_OLED_H */
