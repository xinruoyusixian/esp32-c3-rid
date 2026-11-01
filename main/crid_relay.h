/**
 * crid_relay.h — 网络中继模式（AP + STA + NAPT）
 *
 * 设计要点：
 *  1) 中继是独立运行模式（CRID_MODE_RELAY），与 RID 侦听 / 蓝牙键盘互斥：
 *     中继时 AP 信道必须跟随上游 STA 信道，与 sniff 固定信道冲突，故重启切换。
 *  2) 中继不启动 tracker / NimBLE，内存占用远低于另外两种模式。
 *  3) STA 连接复用 crid_ext 的状态机（扫描命中后才连、失败退避、掉线重连），
 *     本模块只负责：拿到 IP 后开 NAPT、给 DHCP 客户端下发可用 DNS、暴露状态。
 *  4) AP 热点名称/密码可配置（NVS namespace "relay"），三种模式共用。
 */

#ifndef CRID_RELAY_H
#define CRID_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 中继配置（NVS namespace = "relay"） */
typedef struct {
    char ap_ssid[33];
    char ap_pass[65];
} crid_relay_cfg_t;

/** 从 NVS 载入热点配置（未配置时填默认 rid / 12345678） */
void crid_relay_load_cfg(crid_relay_cfg_t *out);

/** 保存热点配置（SSID 1~32 字节，密码为空表示开放网络） */
esp_err_t crid_relay_save_cfg(const char *ssid, const char *pass);

/** 记录 AP netif（app_main 创建 AP 后调用） */
void crid_relay_set_ap_netif(esp_netif_t *ap_netif);

/** 读取已记录的 AP netif 句柄（未设置时返回 NULL） */
esp_netif_t *crid_relay_get_ap_netif(void);

/**
 * @brief 进入中继模式：强制启用 STA 并立即发起连接
 * @return ESP_OK / ESP_ERR_INVALID_STATE（未配置上游 WiFi）
 */
esp_err_t crid_relay_start(void);

/** STA 拿到 IP 回调（由 crid_ext 事件处理调用）：开启 NAPT、下发 DNS */
void crid_relay_on_got_ip(const esp_ip4_addr_t *ip, const esp_ip4_addr_t *gw);

/** STA 掉线回调：NAPT 保持开启，仅更新状态 */
void crid_relay_on_sta_lost(void);

/** 中继是否已生效（NAPT 已开启） */
bool crid_relay_is_active(void);

/**
 * @brief 直接读取中继运行状态（供屏幕等低栈上下文使用，避免 JSON 解析）
 *
 * 返回的指针指向模块内部静态缓冲，调用者只读、不释放、不跨任务长期持有。
 *
 * @param[out] ssid  上游热点名（可为 ""，不会为 NULL）
 * @param[out] ip    STA 拿到的 IP（未连接时为 ""）
 * @param[out] gw    上游网关（未连接时为 ""）
 * @return NAPT 是否已开启
 */
bool crid_relay_sta_info(const char **ssid, const char **ip, const char **gw);

/** 中继运行状态（JSON，供 /api/relay 使用） */
void crid_relay_status_json(char *buf, size_t n);

/**
 * @brief 挂上 STA 网口的收发计数钩子（可重复调用，网口就绪后自动生效）
 *
 * lwIP 未开启 SNMP/MIB2，没有现成的字节计数器，这里把 STA 网口的
 * input / linkoutput 换成计数包装函数：
 *   linkoutput = 本机发出（经 NAPT 转发到上游）-> 上传
 *   input      = 本机收到（上游回来的数据）    -> 下载
 * 计数用 uint32_t，回绕由差值运算天然处理。
 */
void crid_relay_traffic_init(void);

/**
 * @brief 取上下行速率与累计流量
 *
 * @param[out] up_bps   上行速率（字节/秒），可为 NULL
 * @param[out] dn_bps   下行速率（字节/秒），可为 NULL
 * @param[out] up_total 上行累计字节，可为 NULL
 * @param[out] dn_total 下行累计字节，可为 NULL
 * @note 每次调用按与上次调用的间隔算瞬时速率并做指数平滑，
 *       建议以 300~1000ms 的周期调用（屏幕任务 400ms 正合适）。
 */
void crid_relay_traffic(uint32_t *up_bps, uint32_t *dn_bps,
                        uint64_t *up_total, uint64_t *dn_total);

/** 注册 DHCP 下发事件，用于记录已连设备的 MAC->IP（启动时调用一次） */
void crid_relay_clients_init(void);

/** 已连设备列表 JSON（堆上字符串，调用者 free）；失败返回 NULL */
char *crid_relay_clients_json(void);

/**
 * @brief 结构化读取已连设备（供屏幕等低栈上下文使用，避免 JSON 解析）
 *
 * 每个下标对应一台设备，macs[i] 为 6 字节 MAC，ips[i] 为点分十进制串
 * （查不到 IP 时填 "--"）。两个数组均不可为 NULL。
 *
 * @param[out] macs  调用方提供的 macs[max][6]
 * @param[out] ips   调用方提供的 ips[max][16]
 * @param[in]  max   最多写入多少条
 * @return 实际写入的条数
 */
int crid_relay_client_list(uint8_t macs[][6], char ips[][16], int max);

#ifdef __cplusplus
}
#endif

#endif // CRID_RELAY_H
