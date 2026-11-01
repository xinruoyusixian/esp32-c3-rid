/**
 * crid_sniffer.h — Wi-Fi Sniffer 模块接口
 */

#ifndef CRID_SNIFFER_H
#define CRID_SNIFFER_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "crid_rx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取 sniffer 消息队列句柄
 */
QueueHandle_t crid_sniffer_get_queue(void);

/**
 * 获取全局统计指针
 */
sniffer_stats_t *crid_sniffer_get_stats(void);

/**
 * 初始化 Wi-Fi 监控模式（NULL mode + Promiscuous）
 * 幂等：已初始化时直接返回 ESP_OK
 * @return ESP_OK 成功，否则失败
 */
esp_err_t crid_sniffer_init(void);

/**
 * 暂停抓包（关闭 promiscuous 回调，保留队列，可随时再开）
 */
void crid_sniffer_pause(void);

/**
 * 恢复抓包（重新打开 promiscuous）
 */
esp_err_t crid_sniffer_resume(void);

/**
 * 反初始化：关闭 promiscuous 并销毁队列
 * 调用前必须保证接收任务已退出
 */
void crid_sniffer_deinit(void);

/**
 * 抓包是否处于运行状态
 */
bool crid_sniffer_is_running(void);

/**
 * 启动信道保持任务（锁定信道，防止漂移）
 */
void crid_sniffer_start_channel_hold(void);

#ifdef __cplusplus
}
#endif

#endif // CRID_SNIFFER_H
