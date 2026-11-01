/**
 * crid_tracker.h — 无人机追踪表接口（线程安全）
 *
 * 追踪表改为堆上动态分配，可在 RID / BLE 拍照模式切换时释放与重建，
 * 把内部 RAM 让给 NimBLE 协议栈。
 */

#ifndef CRID_TRACKER_H
#define CRID_TRACKER_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "crid_rx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化追踪表（堆上分配，容量不足时自动降级）
 * @return ESP_OK 成功；ESP_ERR_NO_MEM 内存不足
 */
esp_err_t crid_tracker_init(void);

/**
 * 释放追踪表，回收内存
 */
void crid_tracker_deinit(void);

/**
 * 追踪表是否已就绪
 */
bool crid_tracker_ready(void);

/**
 * 当前实际可追踪条目数（未就绪返回 0）
 */
int crid_tracker_get_capacity(void);

/**
 * 获取追踪互斥锁句柄（未就绪返回 NULL）
 */
SemaphoreHandle_t crid_tracker_get_mutex(void);

/**
 * 通过 MAC 地址查找或创建追踪条目
 * @param mac  6 字节 MAC 地址
 * @return 追踪条目指针，表满或未就绪返回 NULL
 */
uav_track_t *crid_tracker_find_or_create(const uint8_t *mac);

/**
 * 获取活跃无人机数量（未就绪返回 0）
 */
int crid_tracker_get_active_count(void);

/**
 * 获取追踪表数组（用于遍历，未就绪返回 NULL）
 */
uav_track_t *crid_tracker_get_table(void);

/**
 * 清理超时条目
 * @param timeout_ms  超时阈值（毫秒）
 */
void crid_tracker_cleanup(uint32_t timeout_ms);


/* ================================================================
 * 新设备事件（供外部 REST 接口做增量拉取 / 主动推送）
 * ================================================================ */

/** 新设备事件队列长度（环形，超出后最旧事件被覆盖） */
#define CRID_NEW_EV_MAX 32

/** 一条"新设备"事件 */
typedef struct {
    uint32_t seq;      /* 全局递增序号，永不清零 */
    uint8_t  mac[6];   /* 设备 MAC */
    uint32_t ts_ms;    /* 事件时间戳（开机毫秒） */
} crid_new_ev_t;

/**
 * @brief 当前最新新设备序号（外部拉取用 since_seq 做游标）
 */
uint32_t crid_tracker_get_new_seq(void);

/**
 * @brief 取 seq > since_seq 的新设备当前快照
 * @param since_seq  上次拉取到的 seq（首次传 0）
 * @param out        输出数组（每个元素是设备当前时刻的完整快照）
 * @param max_count  输出数组容量
 * @param out_seq    返回本次已覆盖到的最新 seq，外部下次用它作为 since
 * @param out_miss   返回因环形队列覆盖而丢失的事件数估算
 * @return 实际填充条数
 */
int crid_tracker_snapshot_new(uint32_t since_seq, uav_track_t *out, int max_count,
                              uint32_t *out_seq, int *out_miss);

/**
 * @brief 按 MAC 取设备当前快照（不改变状态）
 * @return true 命中
 */
bool crid_tracker_copy_by_mac(const uint8_t *mac, uav_track_t *out);

/**
 * @brief 清空所有跟踪条目并释放内部资源（调用方需保证并发安全）
 * @return 被清掉的条目数
 */
int crid_tracker_clear_all(void);

/**
 * @brief 新设备回调（弱符号）：由推送模块实现
 * @note  在 tracker 互斥锁持有期间被调用，实现必须非阻塞（只做入队）
 */
void crid_tracker_on_new_uav(const uint8_t *mac, uint32_t seq);

#ifdef __cplusplus
}
#endif

#endif // CRID_TRACKER_H
