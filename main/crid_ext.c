/**
 * crid_ext.c — 对外服务接口实现
 *
 * 提供 6 组 REST 接口（每组同时支持 GET / POST）：
 *   /api/ext/sta          STA 配置与状态（ssid/password/enable/forget/scan）
 *   /api/ext/scan         扫描周边 WiFi 列表（供配置页选择 SSID）
 *   /api/ext/uavs         增量拉取新设备（游标 since）
 *   /api/ext/clear        清空设备 / 缓存，释放内存
 *   /api/ext/push_config  配置主动推送目标（URL + GET/POST）
 *   /api/ext/test_push    立即全量推送一次（联调用）
 *   /api/ext/status       综合状态（STA / 推送统计 / 设备数 / 内存）
 */

#include "crid_ext.h"
#include "crid_tracker.h"
#include "crid_nvs.h"
#include "crid_rx_types.h"
#include "crid_web.h"
#include "crid_sniffer.h"
#include "crid_relay.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

static const char *TAG = "CRID_EXT";
static const char *NVS_NS = "crid_ext";

/* ================================================================
 * 全局状态
 * ================================================================ */

static crid_ext_cfg_t g_cfg;

/* STA 运行状态 */
static volatile bool g_sta_connected = false;
static char          g_sta_ip[16] = "0.0.0.0";
static int8_t        g_sta_rssi = 0;
static uint8_t       g_sta_channel = 0;
static uint32_t      g_sta_last_try_ms = 0;
static uint32_t      g_sta_connects = 0;
static uint32_t      g_sta_discs = 0;
static bool          g_wifi_started = false;
static bool          g_sta_netif_ready = false;
static bool          g_push_task_ready = false;
static bool          g_sta_mon_ready = false;

static void sta_monitor_task(void *pv);
static void push_task(void *pv);

/* 推送统计 */
static QueueHandle_t g_push_q = NULL;
static uint32_t      g_push_ok = 0;
static uint32_t      g_push_fail = 0;
static uint32_t      g_push_drop = 0;
static int           g_push_last_code = 0;
static uint32_t      g_push_last_ms = 0;
static char          g_push_last_err[64] = "";

#define PUSH_QUEUE_LEN      16
#define PUSH_SETTLE_MS      1200   /* 新设备出现后等待报文补齐再取快照 */
#define PUSH_TIMEOUT_MS     8000
#define PUSH_BODY_MAX       1024
#define ARG_BUF             512

const crid_ext_cfg_t *crid_ext_get_cfg(void) { return &g_cfg; }

/* ================================================================
 * 小工具
 * ================================================================ */

static void mac_to_str(const uint8_t *mac, char *buf, size_t sz) {
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *protocol_name(uint8_t proto) {
    switch (proto) {
        case 1: return "GB 42590";
        case 2: return "GB 46750";
        case 3: return "ASTM F3411";
        default: return "Unknown";
    }
}

static const char *status_text(uint8_t status) {
    switch (status) {
        case 0: return "Undeclared";
        case 1: return "Ground";
        case 2: return "Airborne";
        case 3: return "Emergency";
        case 4: return "Failure";
        default: return "Unknown";
    }
}

static const char *id_type_text(uint8_t id_type) {
    switch (id_type) {
        case 0: return "None";
        case 1: return "Serial Number";
        case 2: return "CAA Registration";
        case 3: return "UTM ID";
        case 4: return "Specific Session";
        default: return "Unknown";
    }
}

/* URL 参数简单编码（仅保留非保留字符） */
static void url_encode(const char *in, char *out, size_t out_sz) {
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] != 0 && o + 4 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = 0;
}

/* ================================================================
 * NVS 配置
 * ================================================================ */

static void cfg_set_defaults(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.sta_enable = false;
    g_cfg.push_enable = false;
    g_cfg.push_method = CRID_PUSH_METHOD_POST;
}

static void cfg_load(void) {
    cfg_set_defaults();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "NVS(%s) 无历史配置，使用默认值", NVS_NS);
        return;
    }
    uint8_t u8 = 0;
    size_t len = 0;

    if (nvs_get_u8(h, "sta_en", &u8) == ESP_OK) g_cfg.sta_enable = (u8 != 0);
    len = sizeof(g_cfg.sta_ssid);
    if (nvs_get_str(h, "sta_ssid", g_cfg.sta_ssid, &len) != ESP_OK) g_cfg.sta_ssid[0] = 0;
    len = sizeof(g_cfg.sta_pass);
    if (nvs_get_str(h, "sta_pwd", g_cfg.sta_pass, &len) != ESP_OK) g_cfg.sta_pass[0] = 0;

    if (nvs_get_u8(h, "push_en", &u8) == ESP_OK) g_cfg.push_enable = (u8 != 0);
    len = sizeof(g_cfg.push_url);
    if (nvs_get_str(h, "push_url", g_cfg.push_url, &len) != ESP_OK) g_cfg.push_url[0] = 0;
    if (nvs_get_u8(h, "push_m", &u8) == ESP_OK) {
        g_cfg.push_method = (u8 == CRID_PUSH_METHOD_GET) ? CRID_PUSH_METHOD_GET : CRID_PUSH_METHOD_POST;
    }
    len = sizeof(g_cfg.api_token);
    if (nvs_get_str(h, "api_tok", g_cfg.api_token, &len) != ESP_OK) g_cfg.api_token[0] = 0;

    nvs_close(h);
    ESP_LOGI(TAG, "配置载入: sta=%d ssid=%s push=%d url=%s method=%s",
             g_cfg.sta_enable, g_cfg.sta_ssid, g_cfg.push_enable, g_cfg.push_url,
             g_cfg.push_method == CRID_PUSH_METHOD_GET ? "GET" : "POST");
}

static void cfg_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u8(h, "sta_en", g_cfg.sta_enable ? 1 : 0);
    nvs_set_str(h, "sta_ssid", g_cfg.sta_ssid);
    nvs_set_str(h, "sta_pwd", g_cfg.sta_pass);
    nvs_set_u8(h, "push_en", g_cfg.push_enable ? 1 : 0);
    nvs_set_str(h, "push_url", g_cfg.push_url);
    nvs_set_u8(h, "push_m", g_cfg.push_method);
    nvs_set_str(h, "api_tok", g_cfg.api_token);
    nvs_commit(h);
    nvs_close(h);
}

/* ================================================================
 * STA 模式
 * ================================================================ */

static void sta_task_start(void);

/* 中继模式：常连优先，跳过"扫到才连"的节能逻辑，失败后 20 秒重试 */
static bool g_relay_mode = false;

bool crid_ext_is_relay(void) { return g_relay_mode; }

void crid_ext_set_relay(bool on)
{
    g_relay_mode = on;
    if (on) {
        g_cfg.sta_enable = true;
        sta_task_start();
    }
}

/* ----------------------------------------------------------------
 * STA 状态机（v2）
 *   开机：先扫描 → 命中保存的 SSID 才发起连接（无覆盖时不空转）
 *   一轮最多 3 次尝试，全失败 → 回 AP 待机（STA 保持使能以支持扫描）
 *   AP 待机：每 5 分钟扫描一次，命中才连；连续未命中放宽到 10 分钟
 *   运行中掉线：先快速重连（3 次），失败再回待机节奏，避免重连风暴
 * ---------------------------------------------------------------- */
#define STA_TRY_MAX             3        /* 每轮最大尝试次数 */
#define STA_CONNECT_TIMEOUT_MS  15000    /* 单次连接超时 */
#define STA_RETRY_GAP_MS        3000     /* 两次尝试间隔 */
#define STA_RELAY_BACKOFF_MIN_MS 3000    /* 中继重连退避下限 */
#define STA_RELAY_BACKOFF_MAX_MS 20000   /* 中继重连退避上限 */
#define STA_IP_TIMEOUT_MS       25000    /* 已关联但迟迟拿不到 IP 的超时 */
#define STA_SCAN_INTERVAL_MS    300000   /* AP 待机扫描周期：5 分钟 */
#define STA_SCAN_LONG_MS        600000   /* 连续未命中后的扫描周期：10 分钟 */
#define STA_SCAN_MISS_LIMIT     6        /* 连续未命中多少次后放宽（约 30 分钟） */
#define STA_BOOT_DELAY_MS       4000     /* 开机后等待系统稳定再扫描 */
#define STA_SCAN_MAX_AP         16

typedef enum {
    STA_ST_AP_IDLE = 0,   /* AP 待机，等下一次扫描 */
    STA_ST_CONNECTING,    /* 正在尝试连接 */
    STA_ST_CONNECTED,     /* 已关联 */
} sta_state_t;

static volatile sta_state_t g_sta_state = STA_ST_AP_IDLE;
static int      g_sta_try = 0;             /* 本轮已尝试次数 */
static uint32_t g_sta_deadline_ms = 0;     /* 当前尝试的判定时刻 */
static uint32_t g_sta_next_scan_ms = 0;    /* 下次扫描时刻 */
static uint32_t g_sta_conn_ms = 0;         /* 关联成功时刻 */
static int      g_sta_scan_miss = 0;       /* 连续扫描未命中次数 */
static uint32_t g_sta_scan_total = 0;      /* 累计扫描次数 */
static volatile bool g_sta_scan_req = false;  /* 接口请求立即扫描 */
static uint32_t g_sta_backoff_ms = 0;       /* 中继模式重连退避（成功后清零） */

static void sta_task_start(void);
static void sta_enter_idle(uint32_t delay_ms);
static void sta_try_connect(void);

static uint32_t now_ms(void) { return esp_log_timestamp(); }

/* 真实开机毫秒数（esp_log_timestamp 在系统时间源下不随重启归零，不可用于 uptime） */
static uint32_t uptime_ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *sta_state_text(void) {
    if (g_sta_connected) return "connected";
    switch (g_sta_state) {
        case STA_ST_CONNECTING: return "connecting";
        case STA_ST_AP_IDLE:    return "ap_idle";
        default:                return "ap_idle";
    }
}

/* 按需把 WiFi 切到 APSTA 并创建 STA 网口（扫描同样需要 STA 使能） */
static bool sta_netif_prepare(void) {
    if (!g_sta_netif_ready) {
        esp_netif_t *nif = esp_netif_create_default_wifi_sta();
        if (nif == NULL) {
            ESP_LOGE(TAG, "创建 STA 网口失败（内存不足）");
            return false;
        }
        g_sta_netif_ready = true;
    }
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) == ESP_OK && m != WIFI_MODE_APSTA && m != WIFI_MODE_STA) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "切换 APSTA 失败: %s", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

/* 仅 AP：未配置 STA 或用户清除凭据时调用，省内存 */
static void sta_ap_only(void) {
    if (g_relay_mode) return;  /* 中继模式禁止退回纯 AP：AP 必须与 STA 并存才能转发 */
    esp_wifi_disconnect();
    g_sta_connected = false;
    g_sta_state = STA_ST_AP_IDLE;
    g_sta_try = 0;
    snprintf(g_sta_ip, sizeof(g_sta_ip), "0.0.0.0");
    g_sta_rssi = 0;
    g_sta_channel = 0;
    if (g_sta_netif_ready) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
}

static bool sta_set_config(void) {
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, g_cfg.sta_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, g_cfg.sta_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA 配置失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* 回 AP 待机（STA 仍使能，便于定时扫描） */
static void sta_enter_idle(uint32_t delay_ms) {
    if (g_relay_mode) {
        /* 中继：上游需常连，失败后指数退避 3→6→12→20 秒，避免空转刷日志与抢占射频 */
        uint32_t bo = g_sta_backoff_ms ? g_sta_backoff_ms * 2 : STA_RELAY_BACKOFF_MIN_MS;
        if (bo > STA_RELAY_BACKOFF_MAX_MS) bo = STA_RELAY_BACKOFF_MAX_MS;
        g_sta_backoff_ms = bo;
        delay_ms = bo;
    }
    if (g_sta_netif_ready) esp_wifi_disconnect();
    g_sta_connected = false;
    snprintf(g_sta_ip, sizeof(g_sta_ip), "0.0.0.0");
    g_sta_rssi = 0;
    g_sta_state = STA_ST_AP_IDLE;
    g_sta_try = 0;
    g_sta_next_scan_ms = now_ms() + delay_ms;
    if (g_relay_mode)
        ESP_LOGW(TAG, "中继上游未连上，%lu 秒后重连（退避）", (unsigned long)(delay_ms / 1000));
    else
        ESP_LOGI(TAG, "STA 转 AP 待机，%lu 秒后扫描", (unsigned long)(delay_ms / 1000));
}

/* 发起一次连接尝试 */
static void sta_try_connect(void) {
    if (!g_cfg.sta_enable || g_cfg.sta_ssid[0] == 0) return;
    if (!sta_netif_prepare() || !sta_set_config()) {
        sta_enter_idle(STA_SCAN_INTERVAL_MS);
        return;
    }
    if (g_sta_try < STA_TRY_MAX) g_sta_try++;
    g_sta_state = STA_ST_CONNECTING;
    g_sta_last_try_ms = now_ms();
    g_sta_deadline_ms = g_sta_last_try_ms + STA_CONNECT_TIMEOUT_MS;
    esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "STA 第 %d/%d 次连接 SSID=%s (%s)", g_sta_try, STA_TRY_MAX,
             g_cfg.sta_ssid, esp_err_to_name(err));
    if (err != ESP_OK) {
        g_sta_deadline_ms = now_ms() + STA_RETRY_GAP_MS;
    }
}

/* 开始新一轮（重置计数并立即尝试） */
static void sta_start_round(void) {
    g_sta_try = 0;
    g_sta_state = STA_ST_CONNECTING;
    sta_try_connect();
}

static void sta_refresh_info(void) {
    if (!g_sta_connected) return;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        g_sta_rssi = ap.rssi;
        g_sta_channel = ap.primary;
    }
}

void crid_ext_relay_connect_now(void)
{
    if (!g_relay_mode) return;
    g_sta_scan_miss = 0;
    g_sta_try = 0;
    sta_start_round();
}

void crid_ext_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                g_wifi_started = true;
                ESP_LOGI(TAG, "STA 协议栈就绪");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                g_sta_connects++;
                g_sta_connected = true;
                g_sta_state = STA_ST_CONNECTED;
                g_sta_try = 0;
                g_sta_backoff_ms = 0;
                g_sta_scan_miss = 0;
                g_sta_conn_ms = now_ms();
                sta_refresh_info();
                ESP_LOGI(TAG, "STA 已关联 SSID=%s CH=%u，等待 DHCP", g_cfg.sta_ssid, g_sta_channel);
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
                g_sta_discs++;
                g_sta_connected = false;
                snprintf(g_sta_ip, sizeof(g_sta_ip), "0.0.0.0");
                g_sta_rssi = 0;
                ESP_LOGW(TAG, "STA 断开, reason=%d", ev ? ev->reason : -1);
                if (g_relay_mode) crid_relay_on_sta_lost();
                /* 掉线后交给守护任务：3 次快速重连，仍失败则转 AP 待机 */
                if (g_sta_state == STA_ST_CONNECTED) {
                    g_sta_state = STA_ST_CONNECTING;
                    g_sta_try = 0;
                    g_sta_deadline_ms = now_ms() + STA_RETRY_GAP_MS;
                } else if (g_sta_state == STA_ST_CONNECTING) {
                    g_sta_deadline_ms = now_ms() + STA_RETRY_GAP_MS;
                }
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        if (ev) {
            snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
            g_sta_connected = true;
            g_sta_state = STA_ST_CONNECTED;
            sta_refresh_info();
            ESP_LOGI(TAG, "STA 获取 IP: %s -> 外部服务可访问 http://%s/api/ext/...", g_sta_ip, g_sta_ip);
            if (g_relay_mode) {
                esp_netif_ip_info_t ipinfo;
                esp_netif_ip_info_t *pinfo = NULL;
                esp_netif_t *sta_nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (sta_nif && esp_netif_get_ip_info(sta_nif, &ipinfo) == ESP_OK) pinfo = &ipinfo;
                crid_relay_on_got_ip(pinfo ? &pinfo->ip : NULL, pinfo ? &pinfo->gw : NULL);
            }
        }
    }
}

/* 扫描一次，返回周边 AP 数量（<0 表示失败）；扫描期间暂停抓包，结束后恢复信道 */
static int sta_scan_ap_list(wifi_ap_record_t *out, int max) {
    if (out == NULL || max <= 0) return -1;
    if (!sta_netif_prepare()) return -2;

    uint8_t pri = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&pri, &sec);

    bool sniff = crid_sniffer_is_running();
    if (sniff) crid_sniffer_pause();

    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.ssid = NULL;
    sc.bssid = NULL;
    sc.channel = 0;
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 80;
    sc.scan_time.active.max = 160;

    int n = -3;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > (uint16_t)max) num = (uint16_t)max;
        if (num == 0) {
            n = 0;
        } else if (esp_wifi_scan_get_ap_records(&num, out) == ESP_OK) {
            n = (int)num;
        } else {
            n = -4;
        }
        esp_wifi_clear_ap_list();
    } else {
        ESP_LOGW(TAG, "WiFi 扫描失败: %s", esp_err_to_name(err));
    }

    if (pri > 0) esp_wifi_set_channel(pri, sec);
    if (sniff) {
        crid_sniffer_resume();
        crid_sniffer_start_channel_hold();
    }
    g_sta_scan_total++;
    return n;
}

/* 扫描并匹配目标 SSID */
static bool sta_scan_target(void) {
    if (g_cfg.sta_ssid[0] == 0) return false;

    wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * STA_SCAN_MAX_AP);
    if (recs == NULL) {
        ESP_LOGW(TAG, "扫描缓冲不足，跳过本轮");
        return false;
    }
    int n = sta_scan_ap_list(recs, STA_SCAN_MAX_AP);
    bool hit = false;
    int8_t rssi = 0;
    for (int i = 0; i < n; i++) {
        if (recs[i].ssid[0] == 0) continue;
        if (strcasecmp((const char *)recs[i].ssid, g_cfg.sta_ssid) == 0) {
            hit = true;
            rssi = recs[i].rssi;
            break;
        }
    }
    free(recs);
    if (hit) ESP_LOGI(TAG, "扫描命中 SSID=%s RSSI=%d", g_cfg.sta_ssid, rssi);
    return hit;
}

/* 未命中：按周期排下一次扫描（连续未命中逐级放宽到 10 分钟） */
static void sta_schedule_next_scan(void) {
    g_sta_scan_miss++;
    uint32_t interval = (g_sta_scan_miss > STA_SCAN_MISS_LIMIT) ? STA_SCAN_LONG_MS : STA_SCAN_INTERVAL_MS;
    g_sta_next_scan_ms = now_ms() + interval;
    ESP_LOGI(TAG, "未发现 SSID=%s（连续 %d 次），%lu 秒后再扫",
             g_cfg.sta_ssid, g_sta_scan_miss, (unsigned long)(interval / 1000));
}

/* STA 守护：1s tick，负责超时判定、重试、AP 待机扫描 */
static void sta_monitor_task(void *pv) {
    (void)pv;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

        if (!g_cfg.sta_enable || g_cfg.sta_ssid[0] == 0) {
            if (g_sta_state != STA_ST_AP_IDLE || g_sta_connected) sta_ap_only();
            g_sta_scan_req = false;
            continue;
        }

        uint32_t now = now_ms();
        switch (g_sta_state) {
            case STA_ST_CONNECTED:
                /* 已关联但长时间拿不到 IP：断开重来 */
                if (!g_sta_connected) break;
                if (strcmp(g_sta_ip, "0.0.0.0") == 0 && (now - g_sta_conn_ms) > STA_IP_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "STA 已关联但未获取 IP，重新尝试");
                    sta_enter_idle(STA_RETRY_GAP_MS);
                }
                break;

            case STA_ST_CONNECTING:
                if (now < g_sta_deadline_ms) break;
                if (g_sta_try >= STA_TRY_MAX) {
                    ESP_LOGW(TAG, "STA 连续 %d 次连接失败，%s", g_sta_try,
                             g_relay_mode ? "中继等待退避重连" : "转 AP 待机");
                    g_sta_scan_miss = 0;
                    sta_enter_idle(STA_SCAN_INTERVAL_MS);
                } else {
                    sta_try_connect();
                }
                break;

            case STA_ST_AP_IDLE:
            default:
                if (g_relay_mode) {
                    /* 中继：跳过扫描，直接重连（上游固定且需尽快恢复） */
                    if (g_sta_scan_req || now >= g_sta_next_scan_ms) {
                        g_sta_scan_req = false;
                        sta_start_round();
                    }
                    break;
                }
                if (g_sta_scan_req || now >= g_sta_next_scan_ms) {
                    g_sta_scan_req = false;
                    if (sta_scan_target()) {
                        g_sta_scan_miss = 0;
                        sta_start_round();
                    } else {
                        sta_schedule_next_scan();
                    }
                }
                break;
        }
    }
}

static void sta_task_start(void) {
    if (g_sta_mon_ready) return;
    if (xTaskCreate(sta_monitor_task, "sta_mon", 3072, NULL, 3, NULL) == pdPASS) {
        g_sta_mon_ready = true;
    } else {
        ESP_LOGW(TAG, "STA 守护任务创建失败");
    }
}


/* ================================================================
 * HTTP 客户端（主动推送）
 * ================================================================ */

static int http_send(const char *url, bool post, const char *body) {
    esp_http_client_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.url = url;
    cfg.method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = PUSH_TIMEOUT_MS;
    cfg.disable_auto_redirect = false;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        snprintf(g_push_last_err, sizeof(g_push_last_err), "client init failed");
        return -1;
    }
    if (post && body != NULL) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(cli);
    int code = -1;
    if (err == ESP_OK) {
        code = esp_http_client_get_status_code(cli);
        g_push_last_err[0] = 0;
    } else {
        snprintf(g_push_last_err, sizeof(g_push_last_err), "%s", esp_err_to_name(err));
        ESP_LOGW(TAG, "推送失败: %s (%s)", esp_err_to_name(err), url);
    }
    esp_http_client_cleanup(cli);
    return code;
}

/* ================================================================
 * 设备 -> JSON
 * ================================================================ */

static cJSON *uav_to_json(const uav_track_t *u, uint32_t seq) {
    cJSON *item = cJSON_CreateObject();
    if (item == NULL) return NULL;

    char mac_str[20];
    mac_to_str(u->mac, mac_str, sizeof(mac_str));
    cJSON_AddStringToObject(item, "mac", mac_str);
    if (seq != 0) cJSON_AddNumberToObject(item, "seq", seq);
    cJSON_AddBoolToObject(item, "online", u->active);
    cJSON_AddNumberToObject(item, "rssi", u->last_rssi);
    cJSON_AddNumberToObject(item, "channel", u->last_channel);

    if (u->basic_id.valid && u->basic_id.ua_type != ODID_UATYPE_NONE) {
        cJSON_AddStringToObject(item, "id", u->basic_id.uas_id);
        cJSON_AddStringToObject(item, "id_type", id_type_text(u->basic_id.id_type));
        cJSON_AddNumberToObject(item, "ua_type", u->basic_id.ua_type);
    }
    if (u->location.valid) {
        cJSON_AddNumberToObject(item, "latitude", u->location.latitude);
        cJSON_AddNumberToObject(item, "longitude", u->location.longitude);
        cJSON_AddNumberToObject(item, "altitude_msl", u->location.altitude_geo);
        cJSON_AddNumberToObject(item, "altitude_agl", u->location.height);
        cJSON_AddNumberToObject(item, "speed_h", u->location.speed_horizontal);
        cJSON_AddNumberToObject(item, "speed_v", u->location.speed_vertical);
        cJSON_AddNumberToObject(item, "heading", u->location.direction);
        cJSON_AddStringToObject(item, "status_text", status_text(u->location.status));
        cJSON_AddNumberToObject(item, "status", u->location.status);
    }
    if (u->operator_id.valid) {
        cJSON_AddStringToObject(item, "operator_id", u->operator_id.id);
    }
    if (u->system.operator_latitude != 0.0f || u->system.operator_longitude != 0.0f) {
        cJSON_AddNumberToObject(item, "operator_lat", u->system.operator_latitude);
        cJSON_AddNumberToObject(item, "operator_lon", u->system.operator_longitude);
    }
    cJSON_AddStringToObject(item, "protocol_name", protocol_name(u->protocol));
    cJSON_AddNumberToObject(item, "first_seen", u->first_seen_ms);
    cJSON_AddNumberToObject(item, "last_seen", u->last_seen_ms);
    return item;
}

/* 组装 GET 查询串形式的设备参数 */
static void uav_to_query(const uav_track_t *u, uint32_t seq, const char *event,
                         char *buf, size_t sz) {
    char mac_str[20], id_enc[96] = "", op_enc[96] = "";
    mac_to_str(u->mac, mac_str, sizeof(mac_str));
    if (u->basic_id.valid) url_encode(u->basic_id.uas_id, id_enc, sizeof(id_enc));
    if (u->operator_id.valid) url_encode(u->operator_id.id, op_enc, sizeof(op_enc));

    snprintf(buf, sz,
             "event=%s&seq=%lu&mac=%s&id=%s&lat=%.7f&lon=%.7f&alt_msl=%.1f&alt_agl=%.1f"
             "&speed_h=%.1f&speed_v=%.1f&heading=%.1f&rssi=%d&status=%u&op_id=%s&first_seen=%lu&last_seen=%lu",
             event, (unsigned long)seq, mac_str, id_enc,
             u->location.valid ? u->location.latitude : 0.0,
             u->location.valid ? u->location.longitude : 0.0,
             u->location.valid ? u->location.altitude_geo : 0.0f,
             u->location.valid ? u->location.height : 0.0f,
             u->location.valid ? u->location.speed_horizontal : 0.0f,
             u->location.valid ? u->location.speed_vertical : 0.0f,
             u->location.valid ? u->location.direction : 0.0f,
             u->last_rssi, u->location.valid ? u->location.status : 0, op_enc,
             (unsigned long)u->first_seen_ms, (unsigned long)u->last_seen_ms);
}

/* ================================================================
 * 推送
 * ================================================================ */

static void push_stat_update(int code) {
    g_push_last_ms = esp_log_timestamp();
    g_push_last_code = code;
    if (code >= 200 && code < 300) g_push_ok++;
    else g_push_fail++;
}

/* 推送单台设备：POST 时发 JSON body，GET 时拼 query */
static int push_device(const uav_track_t *u, uint32_t seq, const char *event, bool batch_json) {
    if (!g_cfg.push_enable || g_cfg.push_url[0] == 0) return -1;

    if (!batch_json && g_cfg.push_method == CRID_PUSH_METHOD_GET) {
        char qs[ARG_BUF];
        char url[800];
        uav_to_query(u, seq, event, qs, sizeof(qs));
        strlcpy(url, g_cfg.push_url, sizeof(url));
        strlcat(url, strchr(g_cfg.push_url, '?') ? "&" : "?", sizeof(url));
        strlcat(url, qs, sizeof(url));
        int code = http_send(url, false, NULL);
        push_stat_update(code);
        return code;
    }

    /* POST / 批量 JSON */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return -1;
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddNumberToObject(root, "seq", (double)seq);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms_now());
    cJSON_AddNumberToObject(root, "free_heap_kb", (double)(esp_get_free_heap_size() / 1024));
    cJSON_AddItemToObject(root, "device", uav_to_json(u, seq));

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) return -1;

    int code = http_send(g_cfg.push_url, true, body);
    free(body);
    push_stat_update(code);
    return code;
}

/* tracker 弱符号回调：仅入队 */
void crid_tracker_on_new_uav(const uint8_t *mac, uint32_t seq) {
    if (g_push_q == NULL) return;
    if (!g_cfg.push_enable || g_cfg.push_url[0] == 0) return;

    crid_new_ev_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.seq = seq;
    memcpy(ev.mac, mac, 6);
    ev.ts_ms = esp_log_timestamp();

    if (xQueueSend(g_push_q, &ev, 0) != pdTRUE) {
        g_push_drop++;
        ESP_LOGW(TAG, "推送队列已满，丢弃 seq=%lu", (unsigned long)seq);
    }
}

static void push_task(void *pv) {
    (void)pv;
    crid_new_ev_t ev;
    for (;;) {
        if (xQueueReceive(g_push_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (!g_cfg.push_enable || g_cfg.push_url[0] == 0) continue;

        /* 等待该设备的 BasicID / Location 报文补齐，避免推空数据 */
        vTaskDelay(pdMS_TO_TICKS(PUSH_SETTLE_MS));

        uav_track_t snap;
        bool ok = false;
        SemaphoreHandle_t mtx = crid_tracker_get_mutex();
        if (mtx != NULL && xSemaphoreTake(mtx, pdMS_TO_TICKS(300)) == pdTRUE) {
            ok = crid_tracker_copy_by_mac(ev.mac, &snap);
            xSemaphoreGive(mtx);
        }
        if (!ok) {
            ESP_LOGW(TAG, "设备已消失，跳过推送 seq=%lu", (unsigned long)ev.seq);
            continue;
        }
        int code = push_device(&snap, ev.seq, "new_uav", false);
        ESP_LOGI(TAG, "新设备推送 seq=%lu -> HTTP %d", (unsigned long)ev.seq, code);
    }
}

/* ================================================================
 * HTTP 请求参数解析（query 与 JSON body 双通道）
 * ================================================================ */

typedef struct {
    char   query[ARG_BUF];
    char   body[PUSH_BODY_MAX];
    int    body_len;
    cJSON *json;
} req_ctx_t;

static void ctx_load(httpd_req_t *req, req_ctx_t *c) {
    memset(c, 0, sizeof(*c));

    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen > 1 && qlen <= sizeof(c->query)) {
        if (httpd_req_get_url_query_str(req, c->query, qlen) != ESP_OK) c->query[0] = 0;
    }

    int total = req->content_len;
    if (total <= 0) return;
    if (total >= (int)sizeof(c->body)) {
        /* 超长 body 直接丢弃，避免阻塞连接 */
        char tmp[64];
        int left = total;
        while (left > 0) {
            int r = httpd_req_recv(req, tmp, left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left);
            if (r <= 0) break;
            left -= r;
        }
        return;
    }
    int off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, c->body + off, total - off);
        if (r <= 0) break;
        off += r;
    }
    c->body[off] = 0;
    c->body_len = off;
    if (off > 0) {
        const char *p = c->body;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '{' || *p == '[') c->json = cJSON_Parse(p);
    }
}

static void ctx_free(req_ctx_t *c) {
    if (c->json) { cJSON_Delete(c->json); c->json = NULL; }
}

/* httpd_query_key_value 不做百分号解码，这里补上（GET 传 URL 等含编码字符时必须） */
static void url_decode_inplace(char *s) {
    char *d = s;
    for (char *p = s; *p; ) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            *d++ = (char)strtol(h, NULL, 16);
            p += 3;
        } else if (*p == '+') {
            *d++ = ' ';
            p++;
        } else {
            *d++ = *p++;
        }
    }
    *d = 0;
}

static bool get_str(req_ctx_t *c, const char *key, char *out, size_t sz) {
    if (c->json) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(c->json, key);
        if (it && cJSON_IsString(it) && it->valuestring) {
            snprintf(out, sz, "%s", it->valuestring);
            return true;
        }
    }
    if (c->query[0] && httpd_query_key_value(c->query, key, out, sz) == ESP_OK) {
        url_decode_inplace(out);
        return true;
    }
    return false;
}

static bool get_long(req_ctx_t *c, const char *key, long *out) {
    if (c->json) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(c->json, key);
        if (it && cJSON_IsNumber(it)) { *out = (long)it->valuedouble; return true; }
        if (it && cJSON_IsString(it) && it->valuestring) { *out = atol(it->valuestring); return true; }
    }
    char buf[24];
    if (c->query[0] && httpd_query_key_value(c->query, key, buf, sizeof(buf)) == ESP_OK) {
        *out = atol(buf);
        return true;
    }
    return false;
}

static bool get_bool(req_ctx_t *c, const char *key, bool *out) {
    if (c->json) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(c->json, key);
        if (it) {
            if (cJSON_IsBool(it)) { *out = cJSON_IsTrue(it); return true; }
            if (cJSON_IsNumber(it)) { *out = (it->valuedouble != 0); return true; }
            if (cJSON_IsString(it) && it->valuestring) {
                *out = (strcmp(it->valuestring, "1") == 0 ||
                        strcasecmp(it->valuestring, "true") == 0);
                return true;
            }
        }
    }
    char buf[8];
    if (c->query[0] && httpd_query_key_value(c->query, key, buf, sizeof(buf)) == ESP_OK) {
        *out = (strcmp(buf, "1") == 0 || strcasecmp(buf, "true") == 0);
        return true;
    }
    return false;
}

/* ================================================================
 * 响应辅助
 * ================================================================ */

static void add_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-API-Token");
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    add_cors(req);
    httpd_resp_set_type(req, "application/json");
    char *s = cJSON_PrintUnformatted(root);
    if (s == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t send_err(httpd_req_t *req, int code, const char *msg) {
    add_cors(req);
    httpd_resp_set_status(req, code == 401 ? "401 Unauthorized" :
                               code == 400 ? "400 Bad Request" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "err", msg ? msg : "error");
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static bool auth_ok(httpd_req_t *req, req_ctx_t *c) {
    if (g_cfg.api_token[0] == 0) return true;
    char tok[40] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-API-Token", tok, sizeof(tok)) == ESP_OK &&
        strcmp(tok, g_cfg.api_token) == 0) {
        return true;
    }
    if (c != NULL && get_str(c, "token", tok, sizeof(tok)) &&
        strcmp(tok, g_cfg.api_token) == 0) {
        return true;
    }
    return false;
}

/* 取一次跟踪表的当前快照（受互斥锁保护） */
static int snapshot_all(uav_track_t *out, int max) {
    int n = 0;
    SemaphoreHandle_t mtx = crid_tracker_get_mutex();
    uav_track_t *table = crid_tracker_get_table();
    if (mtx == NULL || table == NULL) return 0;
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(300)) != pdTRUE) return 0;
    int cap = crid_tracker_get_capacity();
    for (int i = 0; i < cap && n < max; i++) {
        if (!table[i].active) continue;
        memcpy(&out[n], &table[i], sizeof(uav_track_t));
        n++;
    }
    xSemaphoreGive(mtx);
    return n;
}

/* ================================================================
 * API: /api/ext/sta
 * ================================================================ */

static void sta_state_to_json(cJSON *root) {
    cJSON_AddBoolToObject(root, "enable", g_cfg.sta_enable);
    cJSON_AddStringToObject(root, "ssid", g_cfg.sta_ssid);
    cJSON_AddStringToObject(root, "state", sta_state_text());
    cJSON_AddBoolToObject(root, "connected", g_sta_connected);
    cJSON_AddStringToObject(root, "ip", g_sta_ip);
    cJSON_AddNumberToObject(root, "rssi", g_sta_rssi);
    cJSON_AddNumberToObject(root, "channel", g_sta_channel);
    cJSON_AddNumberToObject(root, "tries", g_sta_try);
    cJSON_AddNumberToObject(root, "try_max", STA_TRY_MAX);
    cJSON_AddNumberToObject(root, "connects", g_sta_connects);
    cJSON_AddNumberToObject(root, "disconnects", g_sta_discs);
    cJSON_AddNumberToObject(root, "scan_count", g_sta_scan_total);
    cJSON_AddNumberToObject(root, "scan_miss", g_sta_scan_miss);
    int32_t left = (int32_t)(g_sta_next_scan_ms - now_ms());
    if (left < 0) left = 0;
    cJSON_AddNumberToObject(root, "next_scan_s", left / 1000);
    cJSON_AddNumberToObject(root, "scan_interval_s", STA_SCAN_INTERVAL_MS / 1000);
    cJSON_AddStringToObject(root, "ap_ssid", "rid");
    cJSON_AddStringToObject(root, "ap_ip", "192.168.4.1");
}

/* GET/POST /api/ext/sta
 *   查询：无参数
 *   配置：ssid=&password=&enable=1   （只给 ssid 时默认 enable=1，并立即开始连接）
 *   清除：forget=1                  （回到仅 AP 模式，不再自动扫描）
 *   立即扫描：scan=1                （由守护任务 1 秒内执行，结果看 state/日志）
 */
static esp_err_t api_ext_sta(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }

    char ssid[33] = {0};
    bool enable = false, forget = false, scan_now = false;
    bool has_enable = get_bool(&ctx, "enable", &enable);
    bool has_ssid = get_str(&ctx, "ssid", ssid, sizeof(ssid));
    bool has_forget = get_bool(&ctx, "forget", &forget);
    bool has_scan = get_bool(&ctx, "scan", &scan_now);

    if (has_forget && forget) {
        /* 清除保存的 WiFi：回仅 AP 模式 */
        g_cfg.sta_enable = false;
        g_cfg.sta_ssid[0] = 0;
        g_cfg.sta_pass[0] = 0;
        cfg_save();
        sta_ap_only();
        ESP_LOGI(TAG, "已清除 STA 凭据，切换到仅 AP 模式");
    } else if (has_ssid || has_enable) {
        if (has_ssid) {
            snprintf(g_cfg.sta_ssid, sizeof(g_cfg.sta_ssid), "%s", ssid);
            char pwd[65] = {0};
            if (get_str(&ctx, "password", pwd, sizeof(pwd)) ||
                get_str(&ctx, "pwd", pwd, sizeof(pwd))) {
                snprintf(g_cfg.sta_pass, sizeof(g_cfg.sta_pass), "%s", pwd);
            }
            if (!has_enable) g_cfg.sta_enable = true;   /* 只给 SSID 时默认开启 */
        }
        if (has_enable) g_cfg.sta_enable = enable;

        char tok[33] = {0};
        if (get_str(&ctx, "api_token", tok, sizeof(tok))) {
            strlcpy(g_cfg.api_token, tok, sizeof(g_cfg.api_token));
        }
        cfg_save();

        if (g_cfg.sta_enable && g_cfg.sta_ssid[0] != 0) {
            sta_task_start();
            if (sta_netif_prepare()) {
                sta_start_round();       /* 主动配置：立即尝试，不必等扫描 */
            } else {
                ESP_LOGW(TAG, "STA 网口准备失败（内存不足），稍后由守护任务重试");
            }
        } else {
            sta_ap_only();
        }
        ESP_LOGI(TAG, "STA 配置更新: enable=%d ssid=%s", g_cfg.sta_enable, g_cfg.sta_ssid);
    } else if (has_scan && scan_now) {
        sta_task_start();
        g_sta_scan_req = true;
        ESP_LOGI(TAG, "收到立即扫描请求");
    }

    sta_refresh_info();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    sta_state_to_json(root);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    ctx_free(&ctx);
    return err;
}

/* ================================================================
 * API: /api/ext/scan  扫描周边 WiFi（供网页选择 SSID）
 * ================================================================ */

static esp_err_t api_ext_scan(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }
    ctx_free(&ctx);

    wifi_ap_record_t *recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * STA_SCAN_MAX_AP);
    if (recs == NULL) return send_err(req, 500, "no mem for scan");

    int n = sta_scan_ap_list(recs, STA_SCAN_MAX_AP);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (recs[i].ssid[0] == 0) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "ch", recs[i].primary);
        cJSON_AddNumberToObject(o, "auth", recs[i].authmode);
        cJSON_AddBoolToObject(o, "saved",
                              g_cfg.sta_ssid[0] != 0 &&
                              strcasecmp((const char *)recs[i].ssid, g_cfg.sta_ssid) == 0);
        cJSON_AddItemToArray(arr, o);
        cnt++;
    }
    free(recs);

    cJSON_AddBoolToObject(root, "ok", n >= 0);
    cJSON_AddNumberToObject(root, "count", cnt);
    cJSON_AddItemToObject(root, "list", arr);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

/* ================================================================
 * API: /api/ext/uavs  增量拉取
 * ================================================================ */

static esp_err_t api_ext_uavs(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }

    long since_l = 0, limit_l = 16;
    bool all = false;
    get_long(&ctx, "since", &since_l);
    get_long(&ctx, "limit", &limit_l);
    get_bool(&ctx, "all", &all);
    if (since_l < 0) since_l = 0;
    if (limit_l <= 0) limit_l = 16;
    if (limit_l > 16) limit_l = 16;      /* ESP32-C3 单条快照约 1.5KB，限制批量防 OOM */
    ctx_free(&ctx);

    /* 快照缓冲走堆，且按小批量复用：避免静态大数组长期占住内部 RAM */
    const int batch = 4;
    uav_track_t *buf = (uav_track_t *)malloc(sizeof(uav_track_t) * batch);
    if (buf == NULL) return send_err(req, 500, "no mem for snapshot");

    /* 追踪表未就绪（未开扫描/仿真）时直接返回空列表，避免半截 chunk 响应 */
    if (crid_tracker_get_mutex() == NULL || crid_tracker_get_table() == NULL) {
        free(buf);
        cJSON *r = cJSON_CreateObject();
        cJSON *l = cJSON_CreateArray();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddItemToObject(r, "list", l);
        cJSON_AddNumberToObject(r, "seq", 0);
        cJSON_AddNumberToObject(r, "latest_seq", 0);
        cJSON_AddNumberToObject(r, "count", 0);
        cJSON_AddNumberToObject(r, "missed", 0);
        cJSON_AddStringToObject(r, "hint", "tracker not ready (scan/sim off)");
        esp_err_t e = send_json(req, r);
        cJSON_Delete(r);
        return e;
    }

    httpd_resp_set_type(req, "application/json");
    add_cors(req);
    httpd_resp_sendstr_chunk(req, "{\"ok\":true,\"list\":[");

    int total = 0, miss = 0;
    uint32_t cursor = (uint32_t)since_l;
    char hdr[96];

    if (all) {
        int cap = crid_tracker_get_capacity();
        int start_i = 0;
        SemaphoreHandle_t mtx = crid_tracker_get_mutex();
        for (;;) {
            uav_track_t *table = crid_tracker_get_table();
            if (mtx == NULL || table == NULL || cap <= 0) break;
            int n = 0;
            if (xSemaphoreTake(mtx, pdMS_TO_TICKS(300)) != pdTRUE) break;
            while (start_i < cap && n < batch) {
                if (table[start_i].active) buf[n++] = table[start_i];
                start_i++;
            }
            xSemaphoreGive(mtx);
            if (n == 0) break;
            for (int i = 0; i < n && total < (int)limit_l; i++) {
                cJSON *item = uav_to_json(&buf[i], 0);
                char *js = item ? cJSON_PrintUnformatted(item) : NULL;
                cJSON_Delete(item);
                if (js == NULL) continue;
                size_t need = strlen(js) + 2;
                char *piece = (char *)malloc(need);
                if (piece) {
                    snprintf(piece, need, "%s%s", total ? "," : "", js);
                    httpd_resp_sendstr_chunk(req, piece);
                    free(piece);
                    total++;
                }
                cJSON_free(js);
            }
            if (total >= (int)limit_l) { cursor = crid_tracker_get_new_seq(); break; }
            if (n < batch) { cursor = crid_tracker_get_new_seq(); break; }
        }
    } else {
        /* 增量：snapshot_new 自带游标，分批取直到取空 */
        for (;;) {
            SemaphoreHandle_t mtx = crid_tracker_get_mutex();
            int n = 0;
            if (mtx != NULL && xSemaphoreTake(mtx, pdMS_TO_TICKS(300)) == pdTRUE) {
                n = crid_tracker_snapshot_new(cursor, buf, batch, &cursor, &miss);
                xSemaphoreGive(mtx);
            } else {
                /* 锁忙：补全响应结构，避免客户端拿到半截 JSON */
                free(buf);
                httpd_resp_sendstr_chunk(req, "],\"seq\":0,\"latest_seq\":0,\"count\":0,\"missed\":0,\"err\":\"tracker busy\"}");
                httpd_resp_sendstr_chunk(req, NULL);
                return ESP_OK;
            }
            if (n <= 0) break;
            for (int i = 0; i < n && total < (int)limit_l; i++) {
                cJSON *item = uav_to_json(&buf[i], 0);
                char *js = item ? cJSON_PrintUnformatted(item) : NULL;
                cJSON_Delete(item);
                if (js == NULL) continue;
                size_t need = strlen(js) + 2;
                char *piece = (char *)malloc(need);
                if (piece) {
                    snprintf(piece, need, "%s%s", total ? "," : "", js);
                    httpd_resp_sendstr_chunk(req, piece);
                    free(piece);
                    total++;
                }
                cJSON_free(js);
            }
            if (total >= (int)limit_l) break;
            if (n < batch) break;
        }
    }

    free(buf);

    snprintf(hdr, sizeof(hdr), "],\"seq\":%lu,\"latest_seq\":%lu,\"count\":%d,\"missed\":%d}",
             (unsigned long)cursor, (unsigned long)crid_tracker_get_new_seq(), total, miss);
    httpd_resp_sendstr_chunk(req, hdr);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ================================================================
 * API: /api/ext/clear  清空设备 / 释放内存
 * ================================================================ */

static esp_err_t api_ext_clear(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }

    bool deep = false;
    get_bool(&ctx, "deep", &deep);

    uint32_t heap_before = esp_get_free_heap_size();
    int cleared = 0;

    SemaphoreHandle_t mtx = crid_tracker_get_mutex();
    if (mtx != NULL && xSemaphoreTake(mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
        cleared = crid_tracker_clear_all();
        xSemaphoreGive(mtx);
    }
    crid_nvs_clear_all();
    if (g_push_q) xQueueReset(g_push_q);

    if (deep) {
        crid_web_release_trajectory();   /* 释放轨迹数组（内存大头） */
    }

    uint32_t heap_after = esp_get_free_heap_size();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "cleared", cleared);
    cJSON_AddNumberToObject(root, "heap_before_kb", (double)(heap_before / 1024));
    cJSON_AddNumberToObject(root, "heap_after_kb", (double)(heap_after / 1024));
    cJSON_AddNumberToObject(root, "freed_kb", (double)((int)heap_after - (int)heap_before) / 1024.0);
    cJSON_AddNumberToObject(root, "online", crid_tracker_get_active_count());
    cJSON_AddNumberToObject(root, "cached", crid_nvs_get_total_count());
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    ctx_free(&ctx);

    ESP_LOGI(TAG, "清空设备: %d 条, heap %lu -> %lu", cleared,
             (unsigned long)heap_before, (unsigned long)heap_after);
    return err;
}

/* ================================================================
 * API: /api/ext/push_config  主动推送配置
 * ================================================================ */

static void push_stat_to_json(cJSON *root) {
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "ok", g_push_ok);
    cJSON_AddNumberToObject(st, "fail", g_push_fail);
    cJSON_AddNumberToObject(st, "drop", g_push_drop);
    cJSON_AddNumberToObject(st, "last_code", g_push_last_code);
    cJSON_AddNumberToObject(st, "last_ms", g_push_last_ms);
    cJSON_AddStringToObject(st, "last_err", g_push_last_err);
    cJSON_AddItemToObject(root, "stat", st);
}

static esp_err_t api_ext_push_config(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }

    char url[192] = {0};
    char method[8] = {0};
    bool enable;
    bool has_url = get_str(&ctx, "url", url, sizeof(url));
    bool has_method = get_str(&ctx, "method", method, sizeof(method));
    bool has_enable = get_bool(&ctx, "enable", &enable);

    if (has_url || has_method || has_enable) {
        if (has_url) {
            if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
                ctx_free(&ctx);
                return send_err(req, 400, "url must start with http:// or https://");
            }
            snprintf(g_cfg.push_url, sizeof(g_cfg.push_url), "%s", url);
        }
        if (has_method) {
            g_cfg.push_method = (strcasecmp(method, "GET") == 0) ? CRID_PUSH_METHOD_GET
                                                                : CRID_PUSH_METHOD_POST;
        }
        if (has_enable) g_cfg.push_enable = enable;
        cfg_save();
        if (g_cfg.push_enable && g_cfg.push_url[0] != 0 && !g_push_task_ready) {
            if (xTaskCreate(push_task, "ext_push", 6144, NULL, 4, NULL) == pdPASS) {
                g_push_task_ready = true;
                ESP_LOGI(TAG, "推送任务已启动");
            } else {
                ESP_LOGW(TAG, "推送任务创建失败");
            }
        }
        ESP_LOGI(TAG, "推送配置更新: enable=%d method=%s url=%s",
                 g_cfg.push_enable,
                 g_cfg.push_method == CRID_PUSH_METHOD_GET ? "GET" : "POST",
                 g_cfg.push_url);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enable", g_cfg.push_enable);
    cJSON_AddStringToObject(root, "url", g_cfg.push_url);
    cJSON_AddStringToObject(root, "method",
                            g_cfg.push_method == CRID_PUSH_METHOD_GET ? "GET" : "POST");
    cJSON_AddBoolToObject(root, "token_enabled", g_cfg.api_token[0] != 0);
    push_stat_to_json(root);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    ctx_free(&ctx);
    return err;
}

/* ================================================================
 * API: /api/ext/test_push  立即推送一次当前在线设备
 * ================================================================ */

static esp_err_t api_ext_test_push(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }
    ctx_free(&ctx);

    if (g_cfg.push_url[0] == 0) return send_err(req, 400, "push url not configured");

    const int batch = 4;
    uav_track_t *snaps = (uav_track_t *)malloc(sizeof(uav_track_t) * batch);
    if (snaps == NULL) return send_err(req, 500, "no mem for snapshot");
    int n = snapshot_all(snaps, batch);
    int sent = 0;
    int last_code = 0;
    bool get_mode = (g_cfg.push_method == CRID_PUSH_METHOD_GET);

    if (get_mode) {
        /* GET：每台设备一次请求 */
        for (int i = 0; i < n; i++) {
            last_code = push_device(&snaps[i], 0, "sync", false);
            if (last_code > 0) sent++;
        }
    } else {
        /* POST：一次批量提交 */
        cJSON *root = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddStringToObject(root, "event", "sync");
        cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms_now());
        cJSON_AddNumberToObject(root, "count", n);
        for (int i = 0; i < n; i++) {
            cJSON *it = uav_to_json(&snaps[i], 0);
            if (it) cJSON_AddItemToArray(arr, it);
        }
        cJSON_AddItemToObject(root, "devices", arr);
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (body) {
            last_code = http_send(g_cfg.push_url, true, body);
            free(body);
            push_stat_update(last_code);
            if (last_code > 0) sent = n;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", last_code >= 200 && last_code < 300);
    cJSON_AddNumberToObject(root, "sent", sent);
    cJSON_AddNumberToObject(root, "http_code", last_code);
    cJSON_AddStringToObject(root, "url", g_cfg.push_url);
    cJSON_AddStringToObject(root, "method", get_mode ? "GET" : "POST");
    cJSON_AddStringToObject(root, "err", g_push_last_err);
    push_stat_to_json(root);
    free(snaps);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

/* ================================================================
 * API: /api/ext/status  综合状态
 * ================================================================ */

static esp_err_t api_ext_status(httpd_req_t *req) {
    req_ctx_t ctx;
    ctx_load(req, &ctx);
    if (!auth_ok(req, &ctx)) { ctx_free(&ctx); return send_err(req, 401, "invalid token"); }
    ctx_free(&ctx);

    sta_refresh_info();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms_now());
    cJSON_AddNumberToObject(root, "free_heap_kb", (double)(esp_get_free_heap_size() / 1024));
    cJSON_AddNumberToObject(root, "online", crid_tracker_get_active_count());
    cJSON_AddNumberToObject(root, "cached", crid_nvs_get_total_count());
    cJSON_AddNumberToObject(root, "new_seq", (double)crid_tracker_get_new_seq());

    cJSON *sta = cJSON_CreateObject();
    sta_state_to_json(sta);
    cJSON_AddItemToObject(root, "sta", sta);

    cJSON *push = cJSON_CreateObject();
    cJSON_AddBoolToObject(push, "enable", g_cfg.push_enable);
    cJSON_AddStringToObject(push, "url", g_cfg.push_url);
    cJSON_AddStringToObject(push, "method",
                            g_cfg.push_method == CRID_PUSH_METHOD_GET ? "GET" : "POST");
    cJSON_AddNumberToObject(push, "queue_free", g_push_q ? (int)uxQueueSpacesAvailable(g_push_q) : 0);
    cJSON_AddItemToObject(root, "push", push);
    push_stat_to_json(root);

    cJSON_AddBoolToObject(root, "token_enabled", g_cfg.api_token[0] != 0);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

/* ================================================================
 * 初始化 / 路由注册
 * ================================================================ */

esp_err_t crid_ext_init(void) {
    cfg_load();

    if (g_push_q == NULL) {
        g_push_q = xQueueCreate(PUSH_QUEUE_LEN, sizeof(crid_new_ev_t));
        if (g_push_q == NULL) {
            ESP_LOGE(TAG, "推送队列创建失败");
            return ESP_ERR_NO_MEM;
        }
    }
    /* 任务按需创建：未启用推送/STA 时不占内存 */
    if (g_cfg.push_enable && g_cfg.push_url[0] != 0) {
        if (xTaskCreate(push_task, "ext_push", 6144, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "推送任务创建失败");
        } else {
            g_push_task_ready = true;
        }
    }

    ESP_LOGI(TAG, "对外接口就绪: /api/ext/{sta,scan,uavs,clear,push_config,test_push,status}");
    return ESP_OK;
}

void crid_ext_attach_wifi(void) {
    g_wifi_started = true;
    if (!g_cfg.sta_enable || g_cfg.sta_ssid[0] == 0) {
        ESP_LOGI(TAG, "STA 未配置（默认 AP only）: 可 POST /api/ext/sta?ssid=..&password=..&enable=1 开启");
        return;
    }
    /* 开机先扫描：没有覆盖时不空转，也不会反复重连 */
    sta_task_start();
    g_sta_state = STA_ST_AP_IDLE;
    g_sta_next_scan_ms = now_ms() + STA_BOOT_DELAY_MS;
    ESP_LOGI(TAG, "开机 STA 待机：%lu 秒后扫描保存的 SSID=%s",
             (unsigned long)(STA_BOOT_DELAY_MS / 1000), g_cfg.sta_ssid);
}

esp_err_t crid_ext_register_handlers(httpd_handle_t server) {
    if (server == NULL) return ESP_ERR_INVALID_ARG;

    static const struct {
        const char *uri;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {
        {"/api/ext/status",      api_ext_status},
        {"/api/ext/sta",         api_ext_sta},
        {"/api/ext/wifi",        api_ext_sta},
        {"/api/ext/scan",        api_ext_scan},
        {"/api/ext/uavs",        api_ext_uavs},
        {"/api/ext/clear",       api_ext_clear},
        {"/api/ext/push_config", api_ext_push_config},
        {"/api/ext/test_push",   api_ext_test_push},
        /* 别名：更语义化的路径，便于外部系统对接 */
        {"/api/ext/devices",     api_ext_uavs},
        {"/api/ext/new_devices", api_ext_uavs},
    };

    int ok = 0;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t g = { .uri = routes[i].uri, .method = HTTP_GET,  .handler = routes[i].handler, .user_ctx = NULL };
        httpd_uri_t p = { .uri = routes[i].uri, .method = HTTP_POST, .handler = routes[i].handler, .user_ctx = NULL };
        if (httpd_register_uri_handler(server, &g) == ESP_OK) ok++;
        if (httpd_register_uri_handler(server, &p) == ESP_OK) ok++;
    }
    ESP_LOGI(TAG, "已注册 %d 条 /api/ext 路由 (GET+POST)", ok);
    return ESP_OK;
}
