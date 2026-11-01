/**
 * ESP32-C3 RID Combined Firmware
 * 
 * WiFi AP: SSID "rid", Password "12345678"
 * Captive Portal auto-redirects to management UI
 * 
 * Features:
 *   - C-RID WiFi sniffer + decoder (GB 42590, ASTM F3411)
 *   - C-RID simulated transmitter
 *   - NVS caching of received drone data
 *   - Serial debug output on UART1 (GPIO4 TX)
 *   - Web management interface
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "opendroneid.h"
#include "crid_rx_types.h"
#include "crid_sniffer.h"
#include "crid_parser.h"
#include "crid_tracker.h"
#include "crid_json.h"
#include "crid_web.h"
#include "crid_ext.h"
#include "crid_relay.h"
#include "crid_nvs.h"
#include "crid_serial.h"

// Simulator
#include "crid_config.h"
#include "crid_messages.h"
#include "crid_patrol.h"
#include "crid_config.h"
#include "crid_wifi.h"
#include "crid_shutter.h"
#include "crid_mode.h"
#include "crid_led.h"
#include "crid_screen.h"
#include "crid_cli.h"
#include <math.h>
#include "esp_random.h"

static const char *TAG = "APP";

/* ================================================================
 * WiFi AP Config
 * ================================================================ */
#define AP_SSID     "rid"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL  6

/* ================================================================
 * Simulator State
 * ================================================================ */
#define g_sim (*crid_web_get_sim())
static uint8_t g_beacon_frame[1024];
static uint16_t g_beacon_frame_len = 0;

static void wifi_init_ap(void) {
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();   /* STA 网口在真正启用 STA 时才创建（省内存） */
    crid_relay_set_ap_netif(ap_netif);
    crid_relay_clients_init();   /* 尽早挂上 DHCP 事件，才能拿到已连设备的 IP */

    /* APSTA 双接口内存吃紧：压缩 WiFi 静态/动态缓冲，避免 rx buffer 分配失败 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num  = 4;
    cfg.dynamic_rx_buf_num = 16;
    cfg.static_tx_buf_num  = 8;
    cfg.dynamic_tx_buf_num = 32;
    cfg.mgmt_sbuf_num      = 8;
    cfg.rx_ba_win          = 6;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 默认 AP only；启用 STA 时由 crid_ext 动态切到 APSTA（ESP32-C3 内存有限） */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    /* 热点名称/密码从 NVS 读取（可在 Web「网络」页配置，重启生效） */
    crid_relay_cfg_t rcfg;
    crid_relay_load_cfg(&rcfg);

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.ap.ssid, rcfg.ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(rcfg.ap_ssid);
    strlcpy((char *)wifi_config.ap.password, rcfg.ap_pass, sizeof(wifi_config.ap.password));
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = rcfg.ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.ap.channel = AP_CHANNEL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(AP_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    /* 对外接口的 WiFi/IP 事件（STA 连接状态、IP 获取） */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &crid_ext_wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &crid_ext_wifi_event_handler, NULL);
    crid_ext_attach_wifi();

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s CH=%d auth=%d",
             rcfg.ap_ssid, AP_CHANNEL, (int)wifi_config.ap.authmode);
}

/* ================================================================
 * Tasks
 * ================================================================ */

static void simulator_task(void *pv) {
    crid_wifi_init(AP_CHANNEL);
    TickType_t lastBeacon = xTaskGetTickCount();
    TickType_t lastPatrol = xTaskGetTickCount();
    const TickType_t bInt = pdMS_TO_TICKS(1000);
    const TickType_t pInt = pdMS_TO_TICKS(10000);

    while (1) {
        vTaskDelayUntil(&lastBeacon, bInt);
        if (!g_sim.running) continue;

        TickType_t now = xTaskGetTickCount();
        
        // Check if trajectory is active
        if (g_sim.trajectory.active && g_sim.trajectory.current_index < g_sim.trajectory.total_points) {
            // Follow trajectory - move all drones to follow points with offsets
            double tlat = g_sim.trajectory.latitudes[g_sim.trajectory.current_index];
            double tlon = g_sim.trajectory.longitudes[g_sim.trajectory.current_index];
            
            for (int i = 0; i < g_sim.count; i++) {
                double off_lat = (i == 0) ? 0 : ((double)(esp_random() % 200) - 100.0) / 111000.0;
                double off_lon = (i == 0) ? 0 : ((double)(esp_random() % 200) - 100.0) / (111000.0 * cos(tlat * 3.14159 / 180.0));
                
                cn_crid_config_t *cfg = &g_sim.drones[i];
                cfg->latitude = tlat + off_lat;
                cfg->longitude = tlon + off_lon;
                /* 轨迹点自带高度则以其为基准，否则沿用默认 120 m；编队其余机再加梯度差 */
                float base_alt = (g_sim.trajectory.altitudes)
                                     ? g_sim.trajectory.altitudes[g_sim.trajectory.current_index]
                                     : 120.0f;
                cfg->altitude_msl = base_alt + (float)(i * 10);
                cfg->altitude_agl = cfg->altitude_msl - 5.0f;
                cfg->heading = (i == 0 && g_sim.trajectory.current_index + 1 < g_sim.trajectory.total_points)
                    ? atan2(g_sim.trajectory.longitudes[g_sim.trajectory.current_index+1] - tlon,
                            g_sim.trajectory.latitudes[g_sim.trajectory.current_index+1] - tlat) * 180.0 / 3.14159
                    : cfg->heading;
                cfg->status = 3;
                cfg->message_counter++;
                
                if (crid_build_beacon_frame(cfg, g_beacon_frame, sizeof(g_beacon_frame), &g_beacon_frame_len)) {
                    crid_wifi_send_raw_frame(g_beacon_frame, g_beacon_frame_len);
                }
                vTaskDelay(pdMS_TO_TICKS(g_sim.trajectory.speed > 0 ? (int)(1000.0 / g_sim.trajectory.speed) : 100));
            }
            
            g_sim.trajectory.current_index++;
            
            // Stop when trajectory ends
            if (g_sim.trajectory.current_index >= g_sim.trajectory.total_points) {
                g_sim.trajectory.active = false;
                g_sim.running = false;
                ESP_LOGI(TAG, "Trajectory complete, simulation stopped");
            }
            
            // Serial output
            char dbg[256];
            snprintf(dbg, sizeof(dbg), "{\"evt\":\"traj\",\"pt\":%d/%d,\"lat\":%.6f,\"lon\":%.6f}\n",
                g_sim.trajectory.current_index, g_sim.trajectory.total_points, tlat, tlon);
            crid_serial_write(dbg, strlen(dbg));
            
        } else {
            // Normal random walk mode
            if ((now - lastPatrol) >= pInt) {
                for (int i = 0; i < g_sim.count; i++) {
                    crid_patrol_random_step(&g_sim.drones[i]);
                }
                lastPatrol = now;
            }
            
            for (int i = 0; i < g_sim.count; i++) {
                cn_crid_config_t *cfg = &g_sim.drones[i];
                if (crid_build_beacon_frame(cfg, g_beacon_frame, sizeof(g_beacon_frame), &g_beacon_frame_len)) {
                    crid_wifi_send_raw_frame(g_beacon_frame, g_beacon_frame_len);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            
            // Serial debug every 5s
            static int dbg_count = 0;
            if (++dbg_count % 5 == 0) {
                char dbg[256];
                snprintf(dbg, sizeof(dbg), "{\"evt\":\"sim\",\"drones\":%d,\"center\":[%.6f,%.6f]}\n",
                    g_sim.count, g_sim.center_lat, g_sim.center_lon);
                crid_serial_write(dbg, strlen(dbg));
            }
        }
    }
}

static void monitor_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int online = crid_tracker_get_active_count();
        int total = crid_nvs_get_total_count();
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"evt\":\"status\",\"ts\":%lu,\"online\":%d,\"total\":%d,\"sim\":%s,\"heap\":%lu}\n",
            esp_log_timestamp(), online, total,
            crid_web_is_sim_running() ? "true" : "false",
            esp_get_free_heap_size());
        crid_serial_write(buf, strlen(buf));
        ESP_LOGI(TAG, "Status: online=%d total=%d sim=%s heap=%lu",
            online, total, crid_web_is_sim_running() ? "ON" : "OFF", esp_get_free_heap_size());
    }
}

/* ================================================================
 * Main
 * ================================================================ */
void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-C3 RID Scanner + Simulator ===");
    ESP_LOGI(TAG, "WiFi AP: %s / %s", AP_SSID, AP_PASSWORD);
    ESP_LOGI(TAG, "Target: ESP32-C3 SuperMini");

    // 板载蓝色指示灯：上电快闪，进入工作模式后由 crid_mode 切换为心跳节奏
    crid_led_init();

    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Init TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Init 对外服务配置（STA 凭据 / 推送目标，来自 NVS）+ 推送任务
    crid_ext_init();

    // Init WiFi AP (shared by sniffer + web + transmitter)
    wifi_init_ap();

    // Init serial debug
    crid_serial_init();

    // Init NVS cache for received drones
    crid_nvs_init();

    // Init web server (始终在线，用于模式切换与状态查看)
    crid_web_init();

    // 进入默认工作模式：RID 侦听 或 BLE 拍照（由 NVS 决定，可失败自动重启兜底）
    crid_mode_boot();

    // Init simulator config
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.count = 1;
    g_sim.center_lat = 39.9042;
    g_sim.center_lon = 116.4074;
    crid_nvs_load_sim_config(&g_sim, sizeof(g_sim));
    if (g_sim.count == 0) g_sim.count = 1;

    // Init onboard OLED + BOOT key (ABROBOT ESP32-C3); 无屏幕时自动跳过
    crid_screen_init();

    // USB Serial/JTAG 命令行（调试用：两块板各占一个 COM 口，可直接下命令）
    crid_cli_init();

    // Create tasks
    xTaskCreate(simulator_task, "sim", 4096, NULL, 4, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 2, NULL);

    // Start web server on calling task (blocking)
    crid_web_start();
}
