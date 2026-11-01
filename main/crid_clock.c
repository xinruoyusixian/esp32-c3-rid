/**
 * crid_clock.c — 时间基准实现
 *
 * 用 esp_timer（上电单调、微秒精度）作载体，浏览器下发的时间戳只换算成一个
 * 偏移量：offset_us = unix_us - esp_timer_us。之后任意时刻：
 *     unix_now = esp_timer_now + offset
 *
 * 这样避开 set timeofday / RTC / SNTP，也避开跨时区转换：固件内部一律按
 * UTC 毫秒 + 时区偏移分钟来格式化。
 */

#include "crid_clock.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "CRID_CLOCK";

/** unix_us - esp_timer_us，建立基准后不再漂移 */
static int64_t s_offset_us = 0;
/** 本地时区偏移（分钟），东八区 = 480 */
static int s_tz_min = 0;
/** 是否已同步 */
static bool s_ready = false;
/** 同步次数，便于排障 */
static uint32_t s_sync_count = 0;

esp_err_t crid_clock_sync(int64_t unix_ms, int tz_offset_min)
{
    if (unix_ms <= 0) return ESP_ERR_INVALID_ARG;

    int64_t now_us = esp_timer_get_time();
    if (now_us < 0) now_us = 0;

    s_offset_us = (unix_ms * 1000LL) - now_us;

    /* 时区偏移做合理性收窄：UTC-12 ~ UTC+14 */
    if (tz_offset_min < -720) tz_offset_min = -720;
    if (tz_offset_min >  840) tz_offset_min =  840;
    s_tz_min = tz_offset_min;

    s_ready = true;
    s_sync_count++;

    ESP_LOGI(TAG, "时间基准已同步 unix_ms=%lld tz=%dmin (第 %lu 次)",
             (long long)unix_ms, s_tz_min, (unsigned long)s_sync_count);
    return ESP_OK;
}

bool crid_clock_ready(void)
{
    return s_ready;
}

int64_t crid_clock_unix_ms(void)
{
    if (!s_ready) return 0;
    return (esp_timer_get_time() + s_offset_us) / 1000LL;
}

int crid_clock_tz_minutes(void)
{
    return s_ready ? s_tz_min : 0;
}

bool crid_clock_format(int64_t unix_ms, char *out, size_t out_n)
{
    if (!s_ready || unix_ms <= 0 || out == NULL || out_n < 16) {
        if (out && out_n > 0) out[0] = 0;
        return false;
    }

    /* 先换到当地时间（毫秒），再取整到秒 */
    int64_t local_ms = unix_ms + ((int64_t)s_tz_min * 60000LL);
    time_t sec = (time_t)(local_ms / 1000LL);
    if (sec < 0) sec = 0;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (gmtime_r(&sec, &tmv) == NULL) {
        out[0] = 0;
        return false;
    }

    snprintf(out, out_n, "%02d-%02d %02d:%02d:%02d",
             tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return true;
}

bool crid_clock_from_uptime(uint32_t uptime_ms, char *out, size_t out_n)
{
    if (out == NULL || out_n < 16) return false;
    out[0] = 0;
    if (!s_ready || uptime_ms == 0) return false;

    /* 当前已经运行的毫秒数 */
    int64_t now_us = esp_timer_get_time();
    if (now_us < 0) now_us = 0;
    uint32_t now_up_ms = (uint32_t)(now_us / 1000LL);

    /*
     * uptime_ms > now_up_ms：这条记录不是本次开机产生的
     * （多半来自 NVS 的历史缓存），换算出来的时间不可信。
     */
    if (uptime_ms > now_up_ms) return false;

    /* 开机时刻的 UTC 毫秒 + 该记录的 uptime */
    int64_t boot_unix_ms = ((now_us + s_offset_us) / 1000LL) - (int64_t)now_up_ms;
    int64_t target_ms = boot_unix_ms + (int64_t)uptime_ms;

    return crid_clock_format(target_ms, out, out_n);
}

void crid_clock_now_str(char *out, size_t out_n)
{
    if (out == NULL || out_n == 0) return;
    if (!s_ready) {
        snprintf(out, out_n, "--");
        return;
    }
    if (!crid_clock_format(crid_clock_unix_ms(), out, out_n)) {
        snprintf(out, out_n, "--");
    }
}

void crid_clock_now_hm(char *out, size_t out_n)
{
    char full[24];
    crid_clock_now_str(full, sizeof(full));

    if (out == NULL || out_n == 0) return;
    if (strcmp(full, "--") == 0) {
        snprintf(out, out_n, "--");
        return;
    }
    /* full = "MM-DD HH:MM:SS"，截掉最后的秒，留出 12 字符宽度 */
    size_t len = strlen(full);
    if (len > 3) {
        full[len - 3] = 0;
    }
    snprintf(out, out_n, "%s", full);
}
