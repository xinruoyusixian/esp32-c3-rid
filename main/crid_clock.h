/**
 * crid_clock.h — 设备时间基准（由浏览器下发）
 *
 * 背景：ESP32-C3 没有 RTC 后备电池，掉电后时间归零；而 Remote ID 记录里的
 * first_seen / last_seen 是"上电以来的毫秒数"（uptime），重启后基线归零，
 * 历史时间戳就失去意义。
 *
 * 方案：网页每次打开时，把浏览器的 UTC 毫秒时间戳 + 时区偏移下发到
 * /api/time 接口。固件记下 (unix_ms - esp_timer_us) 这个偏移量，
 * 之后任意时刻都能用单调时钟换算出真实挂钟时间。
 *
 * 这样：
 *   - 无人机记录的发现/更新时间可以渲染成 "10-29 10:52:44"
 *   - 重启后只要网页再打开一次，基线自动重建
 *   - 不依赖 SNTP（设备可能处在无外网的热点环境）
 */

#ifndef CRID_CLOCK_H
#define CRID_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 用浏览器时间校准本机时间基准
 * @param unix_ms       UTC 毫秒时间戳（Date.now()）
 * @param tz_offset_min 本地时区偏移，单位分钟（东八区 = 480，即 -new Date().getTimezoneOffset()）
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t crid_clock_sync(int64_t unix_ms, int tz_offset_min);

/** 时间基准是否已建立（网页至少同步过一次） */
bool crid_clock_ready(void);

/** 当前 UTC 毫秒时间戳；未同步返回 0 */
int64_t crid_clock_unix_ms(void);

/** 本地时区偏移（分钟） */
int crid_clock_tz_minutes(void);

/**
 * @brief 把 UTC 毫秒格式化为本地可读串 "MM-DD HH:MM:SS"
 * @return true 成功 / false 未同步或参数非法
 */
bool crid_clock_format(int64_t unix_ms, char *out, size_t out_n);

/**
 * @brief 把 uptime 毫秒（设备内部相对时间）换算成本地可读时间
 *
 * 注意：若 uptime_ms 大于当前 uptime，说明这条记录来自上一次开机
 * （NVS 里的历史缓存），此时基准已变，换算出来的时间不可信，返回 false。
 *
 * @return true 成功写入 out / false 不可信（调用方应显示"历史记录"）
 */
bool crid_clock_from_uptime(uint32_t uptime_ms, char *out, size_t out_n);

/** 供日志/接口展示：当前本地时间字符串，未同步时写入 "--" */
void crid_clock_now_str(char *out, size_t out_n);

/**
 * @brief 短格式本地时间 "MM-DD HH:MM"（不带秒），未同步时写入 "--"
 *
 * OLED 只有 72 px 宽、每行 12 个字符，带秒的 "MM-DD HH:MM:SS" 会被截断，
 * 故屏幕底部状态行用这个短格式。
 */
void crid_clock_now_hm(char *out, size_t out_n);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif // CRID_CLOCK_H
