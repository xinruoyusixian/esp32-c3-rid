#ifndef CRID_NVS_H
#define CRID_NVS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "crid_rx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void crid_nvs_init(void);
bool crid_nvs_save_uav(const uav_track_t *uav);
int crid_nvs_load_all(uav_track_t *buffer, int max_count);
int crid_nvs_get_total_count(void);
const uav_track_t *crid_nvs_get_by_index(int index);
const uav_track_t *crid_nvs_get_by_mac(const uint8_t *mac);
void crid_nvs_clear_all(void);
bool crid_nvs_save_sim_config(const void *config, size_t len);
size_t crid_nvs_load_sim_config(void *config, size_t max_len);

/* 门户（Captive Portal）重定向地址，最长 127 字节 */
bool crid_nvs_save_portal(const char *url);
size_t crid_nvs_load_portal(char *buf, size_t max);

#ifdef __cplusplus
}
#endif
#endif
