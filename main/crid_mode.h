/**
 * crid_mode.h — RID 侦听 / BLE 拍照 双模式管理器
 *
 * 设计要点：
 *   1. 默认 RID 模式；进入拍照模式前释放追踪表并关闭 promiscuous，
 *      把内部 RAM 全部让给 NimBLE。
 *   2. 切换前把目标模式写入 NVS（pending），切换成功才落盘为正式模式。
 *   3. 任一步骤失败 → esp_restart()；重启后按 pending 直接进入目标模式，
 *      连续失败超过上限则自动回退到另一模式，避免死循环。
 */

#ifndef CRID_MODE_H
#define CRID_MODE_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRID_MODE_RID    = 0,   /* Wi-Fi RID 侦听（默认） */
    CRID_MODE_CAMERA = 1,   /* BLE HID 键盘（原蓝牙拍照，含音量键快门） */
    CRID_MODE_RELAY  = 2,   /* 网络中继：AP 热点 + STA 上游 + NAPT */
} crid_mode_t;

/**
 * 开机启动：读取 NVS 决定进入 RID 还是拍照模式
 * @return ESP_OK 成功，否则两种模式都起不来
 */
esp_err_t crid_mode_boot(void);

/**
 * 当前生效模式
 */
crid_mode_t crid_mode_get(void);

/**
 * 是否正在切换中（切换期间 Web 接口仍可访问）
 */
bool crid_mode_is_switching(void);

/**
 * 模式名称 "rid" / "camera"
 */
const char *crid_mode_name(crid_mode_t mode);

/**
 * 异步请求切换模式（立即返回，后台任务执行）
 * 切换过程中失败会自动重启设备
 */
esp_err_t crid_mode_request(crid_mode_t target);

#ifdef __cplusplus
}
#endif

#endif // CRID_MODE_H
