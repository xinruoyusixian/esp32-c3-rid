#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 快门模拟按键 */
typedef enum {
    SHUTTER_KEY_VOL_UP = 0,
    SHUTTER_KEY_VOL_DOWN = 1,
} shutter_key_t;

typedef struct {
    bool     connected;   /* BLE 主机已连接 */
    bool     ready;       /* 协议栈就绪 */
    bool     continuous;  /* 连拍中（运行态） */
    bool     paused;      /* 连拍已暂停（运行态但暂停出片） */
    uint32_t interval;    /* 连拍间隔（秒） */
    uint8_t  key;         /* shutter_key_t */
    uint32_t shots;       /* 已触发拍照次数 */
    uint32_t passkey;     /* 当前配对码（0 表示尚未生成） */
    bool     pairing;     /* 正处于"等待手机输入配对码"状态（屏幕据此显示配对码） */
    char     name[32];    /* 广播名称 */
    uint32_t gap;         /* 拍照间隔档位（秒），0 = 手动（单击拍照） */
    uint32_t next_in;     /* 定时连拍倒计时（秒），0 = 未运行 */
} shutter_status_t;

/* 屏幕可选间隔档位（秒）；GAPS[0] = 0 表示手动 */
#define SHUTTER_GAP_MANUAL 0
extern const uint32_t crid_shutter_gaps[8];
#define SHUTTER_GAP_N 8

/* 初始化 BLE HID 快门（内部自建 NimBLE host + 广播） */
esp_err_t crid_shutter_init(void);

/* 反初始化：停止广播、断开连接、关闭 NimBLE host 与 BLE 控制器并回收内存
 * 成功返回 ESP_OK；返回失败表示协议栈状态不可恢复，调用方应重启设备 */
esp_err_t crid_shutter_deinit(void);

/* BLE 协议栈是否就绪 */
bool crid_shutter_is_ready(void);

int crid_shutter_clear_bonds(void);

void crid_shutter_get_status(shutter_status_t *out);

/** 是否正在等待手机输入配对码（屏幕据此把 6 位配对码放大显示） */
bool crid_shutter_is_pairing(void);

/* 触发一次快门（阻塞约 80ms），成功返回 true */
bool crid_shutter_take_photo(void);

bool crid_shutter_set_continuous(bool on);

/** 暂停 / 恢复定时连拍（仅 running 态有意义） */
void crid_shutter_pause(void);
void crid_shutter_resume(void);

/** 停止定时连拍（运行态与暂停态一并清除） */
void crid_shutter_stop(void);

/** 当前是否处于暂停态 */
bool crid_shutter_is_paused(void);

void crid_shutter_set_interval(uint32_t seconds);

/**
 * @brief 设置拍照间隔档位（屏幕用）
 *
 * @param gap 0 = 手动（按键才拍，不自动连拍）；>0 = 每 gap 秒拍一张
 * @note 设为手动时会自动停止正在进行的定时连拍；设为 >0 时同步 interval。
 */
void crid_shutter_set_gap(uint32_t gap);

/** 当前间隔档位（0 = 手动） */
uint32_t crid_shutter_get_gap(void);

/** 切到下一个档位（手动 -> 3 -> 5 -> 10 -> 15 -> 20 -> 30 -> 60 -> 手动），返回新档位 */
uint32_t crid_shutter_gap_next(void);
bool crid_shutter_set_key(uint8_t key);
esp_err_t crid_shutter_set_name(const char *name);

/* ---------------- 远程键盘（HID Report ID 2，标准 6KRO） ----------------
 * mod 位定义：bit0 Ctrl / bit1 Shift / bit2 Alt / bit3 GUI
 * key 为标准 HID Keyboard/Keypad 用法码（如 0x04=A，0x28=Enter，0x29=Esc）
 * 均已做连接/就绪检查，未连接返回 false/-1。 */

/** 按下或抬起单个按键（先用 hold(true) 再 hold(false) 可实现长按） */
bool crid_shutter_key_hold(uint8_t mod, uint8_t key, bool down);

/** 点击一次按键（按下 + 抬起，阻塞约 40ms） */
bool crid_shutter_key_tap(uint8_t mod, uint8_t key);

/** 逐字符输入 ASCII 文本（最大 96 字节），返回成功字符数；-1 未连接 */
int crid_shutter_type_text(const char *utf8);

/* BLE/HID 诊断快照（供串口 ble 命令打印） */
void crid_shutter_ble_diag(char *out, size_t len);

/* 保存/加载配置到 NVS */
void crid_shutter_save_config(void);

#ifdef __cplusplus
}
#endif
