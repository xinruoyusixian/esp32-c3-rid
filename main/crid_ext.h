/**
 * crid_ext.h — 对外服务接口（网络 STA 模式 + 新设备拉取 + 清空 + 主动推送）
 *
 * 设计要点：
 *  1) WiFi 由 APP-ONLY 改为 APSTA：AP(192.168.4.1) 保留本机网页，STA 连接现场路由器，
 *     使外部服务（有线/无线同网段）可直接访问本机 80 端口。
 *  2) 外部服务两种取数方式并存：
 *       - 定时拉取：GET/POST /api/ext/uavs?since=<seq>  增量返回新设备
 *       - 被动接收：本机扫到新设备后主动 HTTP 推送（支持 GET / POST）
 *  3) /api/ext/clear 清空设备列表与缓存，释放内存。
 */

#ifndef CRID_EXT_H
#define CRID_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 推送方式 */
typedef enum {
    CRID_PUSH_METHOD_POST = 0,
    CRID_PUSH_METHOD_GET  = 1,
} crid_push_method_t;

/** 对外服务配置（持久化到 NVS namespace = "crid_ext"） */
typedef struct {
    /* STA */
    bool    sta_enable;
    char    sta_ssid[33];
    char    sta_pass[65];

    /* 主动推送 */
    bool    push_enable;
    char    push_url[192];
    uint8_t push_method;      /* crid_push_method_t */

    /* 可选访问令牌：为空表示 /api/ext/… 不校验 */
    char    api_token[33];
} crid_ext_cfg_t;

/**
 * @brief 初始化：从 NVS 载入配置，创建推送队列/任务
 * @note  必须在 esp_wifi_init() 之后、esp_wifi_start() 之前或之后调用均可；
 *        STA 的实际连接在 crid_ext_attach_wifi() 中发起。
 */
esp_err_t crid_ext_init(void);

/**
 * @brief esp_wifi_start() 之后调用：应用 STA 配置并发起连接，启动重连守护
 */
void crid_ext_attach_wifi(void);

/**
 * @brief WiFi/IP 事件处理（在 app_main 中注册到默认事件循环）
 */
void crid_ext_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

/**
 * @brief 注册 /api/ext/… 全部路由（GET + POST 双方法）
 */
esp_err_t crid_ext_register_handlers(httpd_handle_t server);

/**
 * @brief 新设备回调实现（由 crid_tracker 的弱符号调用，只入队，非阻塞）
 */
void crid_tracker_on_new_uav(const uint8_t *mac, uint32_t seq);

/** 读取当前配置（只读用途） */
const crid_ext_cfg_t *crid_ext_get_cfg(void);

/* ---------------- 中继模式联动 ---------------- */

/** 设置/取消中继语义：开启时强制 sta_enable 并启动 STA 守护 */
void crid_ext_set_relay(bool on);

/** 当前是否处于中继语义 */
bool crid_ext_is_relay(void);

/** 中继模式：跳过扫描，立即发起一轮连接 */
void crid_ext_relay_connect_now(void);

#ifdef __cplusplus
}
#endif

#endif // CRID_EXT_H
