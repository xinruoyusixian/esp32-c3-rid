#include "index_html.h"
#include "crid_web.h"
#include "crid_rx_types.h"
#include "crid_tracker.h"
#include "crid_nvs.h"
#include "crid_config.h"
#include "crid_messages.h"
#include "crid_mode.h"
#include "crid_shutter.h"
#include "crid_led.h"
#include "crid_ext.h"
#include "crid_relay.h"
#include "crid_clock.h"
#include "crid_oled.h"
#include "crid_screen.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

static const char *TAG = "CRID_WEB";

// Simulator state (shared with app_main)
static sim_control_t g_sim;

sim_control_t *crid_web_get_sim(void) { return &g_sim; }

/**
 * 给无人机条目补上"可读时间"。
 * 只有在时间基准已同步、且记录属于本次开机时才写 *_time 字段；
 * 历史缓存（NVS 里跨开机的记录）不写，前端会显示"历史记录"。
 */
static void add_seen_times(cJSON *item, uint32_t first_ms, uint32_t last_ms) {
    char buf[24];
    /* 缓存记录常常没写 last_seen，退化为首次发现时间，避免前端显示空白 */
    if (last_ms == 0) last_ms = first_ms;

    if (crid_clock_from_uptime(first_ms, buf, sizeof(buf))) {
        cJSON_AddStringToObject(item, "first_seen_time", buf);
    }
    if (crid_clock_from_uptime(last_ms, buf, sizeof(buf))) {
        cJSON_AddStringToObject(item, "last_seen_time", buf);
    }
}
bool crid_web_is_sim_running(void) { return g_sim.running; }
void crid_web_set_sim_running(bool running) { g_sim.running = running; }

void crid_web_set_trajectory(const double *lats, const double *lons, const float *alts, int count, float speed) {
    if (count <= 0 || count > MAX_TRAJECTORY_POINTS) return;
    if (g_sim.trajectory.latitudes) free(g_sim.trajectory.latitudes);
    if (g_sim.trajectory.longitudes) free(g_sim.trajectory.longitudes);
    if (g_sim.trajectory.altitudes) { free(g_sim.trajectory.altitudes); g_sim.trajectory.altitudes = NULL; }

    g_sim.trajectory.latitudes = malloc(count * sizeof(double));
    g_sim.trajectory.longitudes = malloc(count * sizeof(double));
    if (!g_sim.trajectory.latitudes || !g_sim.trajectory.longitudes) {
        if (g_sim.trajectory.latitudes) { free(g_sim.trajectory.latitudes); g_sim.trajectory.latitudes = NULL; }
        if (g_sim.trajectory.longitudes) { free(g_sim.trajectory.longitudes); g_sim.trajectory.longitudes = NULL; }
        return;
    }

    memcpy(g_sim.trajectory.latitudes, lats, count * sizeof(double));
    memcpy(g_sim.trajectory.longitudes, lons, count * sizeof(double));

    /* 高度可选：分配失败就退化为默认高度，不影响轨迹本身 */
    if (alts) {
        g_sim.trajectory.altitudes = malloc(count * sizeof(float));
        if (g_sim.trajectory.altitudes) {
            memcpy(g_sim.trajectory.altitudes, alts, count * sizeof(float));
        }
    }

    g_sim.trajectory.total_points = count;
    g_sim.trajectory.current_index = 0;
    g_sim.trajectory.speed = speed > 0 ? speed : 1.0f;
    g_sim.trajectory.active = true;
    g_sim.running = true;
    
    ESP_LOGI(TAG, "Trajectory set: %d points, speed=%.1f", count, speed);
}

/* ================================================================
 * Captive Portal DNS Server
 * ================================================================ */
/* 中继模式下：把 DNS 查询转发到上游公共 DNS，使客户端正常上网 */
static int dns_forward_query(const uint8_t *q, int len, uint8_t *resp, int cap)
{
    static const char *upstreams[] = { "223.5.5.5", "119.29.29.29" };
    int n = -1;
    for (size_t i = 0; i < sizeof(upstreams) / sizeof(upstreams[0]) && n <= 0; i++) {
        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) continue;
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in up;
        memset(&up, 0, sizeof(up));
        up.sin_family = AF_INET;
        up.sin_port = htons(53);
        up.sin_addr.s_addr = inet_addr(upstreams[i]);
        if (sendto(s, q, len, 0, (struct sockaddr *)&up, sizeof(up)) > 0) {
            n = recv(s, resp, cap, 0);
        }
        close(s);
    }
    return n;
}

static void dns_server_task(void *pvParameters) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);

    // Get AP IP
    esp_netif_ip_info_t ip_info;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_get_ip_info(ap_netif, &ip_info);

    int sock = -1;
    while (1) {
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ESP_LOGW(TAG, "DNS bind failed, retry in 2s");
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            ESP_LOGI(TAG, "DNS server started on port 53, AP IP: " IPSTR, IP2STR(&ip_info.ip));
        }

        uint8_t rx_buf[512];
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int len = recvfrom(sock, rx_buf, sizeof(rx_buf), 0,
                          (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            close(sock);          /* socket 被回收时重建 */
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (len < 12) continue; // DNS header minimum 12 bytes

        /* 中继模式：转发到上游 DNS，正常解析外网域名 */
        if (crid_relay_is_active()) {
            uint8_t fwd[512];
            int n = dns_forward_query(rx_buf, len, fwd, sizeof(fwd));
            if (n > 0) {
                sendto(sock, fwd, n, 0, (struct sockaddr *)&client_addr, client_addr_len);
                continue;
            }
            /* 上游不可达时退回劫持，至少保证配置页面可打开 */
        }

        // Build DNS response: copy query header, set response flags
        uint8_t response[512];
        memcpy(response, rx_buf, len);
        
        // Set QR bit (response) and recursion available
        response[2] = 0x81;  // QR=1, Opcode=0, AA=0, TC=0, RD=1
        response[3] = 0x80;  // RA=1, Z=0, RCODE=0
        // Set answer count to 1
        response[6] = 0x00;
        response[7] = 0x01;

        // Add answer: pointer to name, type A, class IN, TTL 60, IP
        int ans_pos = len;
        response[ans_pos++] = 0xC0; // Name pointer
        response[ans_pos++] = 0x0C; // Offset to query name (byte 12)
        response[ans_pos++] = 0x00; // Type A (high)
        response[ans_pos++] = 0x01; // Type A (low)
        response[ans_pos++] = 0x00; // Class IN (high)
        response[ans_pos++] = 0x01; // Class IN (low)
        response[ans_pos++] = 0x00; // TTL (high)
        response[ans_pos++] = 0x00;
        response[ans_pos++] = 0x00;
        response[ans_pos++] = 0x3C; // TTL 60 (low)
        response[ans_pos++] = 0x00; // Data length (high)
        response[ans_pos++] = 0x04; // Data length (low)
        // IP address
        memcpy(&response[ans_pos], &ip_info.ip.addr, 4);
        ans_pos += 4;

        sendto(sock, response, ans_pos, 0,
               (struct sockaddr *)&client_addr, client_addr_len);
    }
}

/* ================================================================
 * Embedded Web Page
 * ================================================================ */

;

/* ================================================================
 * API Helpers
 * ================================================================ */

static void add_cors_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// Convert MAC to string "XX:XX:XX:XX:XX:XX"
static void mac_to_str(const uint8_t *mac, char *buf, size_t sz) {
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Get protocol name string
static const char *protocol_name(uint8_t proto) {
    switch (proto) {
        case 1: return "GB 42590";
        case 2: return "GB 46750";
        case 3: return "ASTM F3411";
        default: return "Unknown";
    }
}

// Get status text
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

/* ================================================================
 * API: GET /api/status
 * ================================================================ */
static esp_err_t api_status_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    int online = crid_tracker_get_active_count();
    int total = crid_nvs_get_total_count();
    uint32_t heap = esp_get_free_heap_size();

    char buf[768];
    char now_s[32];
    crid_clock_now_str(now_s, sizeof(now_s));
    /* uptime_ms 供前端把 uptime 换算成本地时间；
       led 字段让网页上的蓝色指示灯与板载灯同步（心跳计数停止增长即程序跑飞）；
       ui / ui_text 让网页知道屏幕当前停在哪一级页面（开机直进二级，长按才回主菜单） */
    static const char *UI_TEXT[] = { "主菜单", "功能页", "详情", "间隔设置", "切换确认", "拍摄" };
    int ui = crid_screen_get_ui();
    if (ui < 0 || ui > 5) ui = 0;
    snprintf(buf, sizeof(buf),
        "{\"online\":%d,\"total\":%d,\"heap_kb\":%lu,\"sim_running\":%s,"
        "\"mode\":\"%s\",\"switching\":%s,\"uptime_ms\":%lu,"
        "\"ui\":%d,\"ui_text\":\"%s\","
        "\"led\":{\"level\":%d,\"blinks\":%lu,\"state\":\"%s\","
        "\"state_text\":\"%s\",\"pattern\":\"%s\",\"health\":%s,\"clock_ready\":%s,\"clock_now\":\"%s\"}}",
        online, total, heap / 1024, g_sim.running ? "true" : "false",
        crid_mode_name(crid_mode_get()),
        crid_mode_is_switching() ? "true" : "false",
        (unsigned long)esp_log_timestamp(),
        ui, UI_TEXT[ui],
        crid_led_get_level(), (unsigned long)crid_led_get_blinks(),
        crid_led_mode_key(crid_led_get_mode()),
        crid_led_mode_text(crid_led_get_mode()),
        crid_led_mode_pattern(crid_led_get_mode()),
        /* 只有 RID 模式依赖业务心跳；相机/中继模式无周期任务，视为健康 */
        (crid_led_health_ok() || crid_mode_get() != CRID_MODE_RID) ? "true" : "false", crid_clock_ready() ? "true" : "false", now_s);
    
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: GET /api/drones - list all drones
 * ================================================================ */
static esp_err_t api_drones_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();

    // First add active (online) drones
    SemaphoreHandle_t mutex = crid_tracker_get_mutex();
    uav_track_t *table = crid_tracker_get_table();
    if (mutex != NULL && table != NULL &&
        xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        int cap = crid_tracker_get_capacity();
        for (int i = 0; i < cap; i++) {
            if (!table[i].active) continue;
            cJSON *item = cJSON_CreateObject();
            char mac_str[20];
            mac_to_str(table[i].mac, mac_str, sizeof(mac_str));
            cJSON_AddStringToObject(item, "mac", mac_str);
            cJSON_AddBoolToObject(item, "online", true);
            cJSON_AddNumberToObject(item, "rssi", table[i].last_rssi);
            
            if (table[i].basic_id.valid && table[i].basic_id.ua_type != ODID_UATYPE_NONE) {
                cJSON_AddStringToObject(item, "id", table[i].basic_id.uas_id);
                cJSON_AddStringToObject(item, "id_type", id_type_text(table[i].basic_id.id_type));
                cJSON_AddStringToObject(item, "model", table[i].basic_id.uas_id);
            }
            if (table[i].location.valid) {
                cJSON_AddNumberToObject(item, "latitude", table[i].location.latitude);
                cJSON_AddNumberToObject(item, "longitude", table[i].location.longitude);
                cJSON_AddNumberToObject(item, "altitude_msl", table[i].location.altitude_geo);
                cJSON_AddNumberToObject(item, "altitude_agl", table[i].location.height);
                cJSON_AddNumberToObject(item, "speed_h", table[i].location.speed_horizontal);
                cJSON_AddNumberToObject(item, "speed_v", table[i].location.speed_vertical);
                cJSON_AddNumberToObject(item, "heading", table[i].location.direction);
                cJSON_AddStringToObject(item, "status_text", status_text(table[i].location.status));
            }
            if (table[i].operator_id.valid) {
                cJSON_AddStringToObject(item, "operator_id", table[i].operator_id.id);
                cJSON_AddNumberToObject(item, "operator_lat", table[i].system.operator_latitude);
                cJSON_AddNumberToObject(item, "operator_lon", table[i].system.operator_longitude);
            }
            cJSON_AddStringToObject(item, "protocol_name", protocol_name(table[i].protocol));
            cJSON_AddNumberToObject(item, "first_seen", table[i].first_seen_ms);
            cJSON_AddNumberToObject(item, "last_seen", table[i].last_seen_ms);
            add_seen_times(item, table[i].first_seen_ms, table[i].last_seen_ms);
            cJSON_AddItemToArray(list, item);
        }
        xSemaphoreGive(mutex);
    }

    // Add cached drones
    int cached_count = crid_nvs_get_total_count();
    for (int i = 0; i < cached_count && i < 50; i++) {
        const uav_track_t *uav = crid_nvs_get_by_index(i);
        if (!uav) continue;
        
        // Skip if already in active list
        bool already = false;
        char mac1[20];
        mac_to_str(uav->mac, mac1, sizeof(mac1));
        // Simple check - just add all cached, duplicates will be visible
        if (already) continue;
        
        cJSON *item = cJSON_CreateObject();
        char mac_str[20];
        mac_to_str(uav->mac, mac_str, sizeof(mac_str));
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddBoolToObject(item, "online", false);
        cJSON_AddNumberToObject(item, "rssi", uav->last_rssi);
        
        if (uav->basic_id.valid && uav->basic_id.ua_type != ODID_UATYPE_NONE) {
            cJSON_AddStringToObject(item, "id", uav->basic_id.uas_id);
            cJSON_AddStringToObject(item, "model", uav->basic_id.uas_id);
        }
        if (uav->location.valid) {
            cJSON_AddNumberToObject(item, "latitude", uav->location.latitude);
            cJSON_AddNumberToObject(item, "longitude", uav->location.longitude);
            cJSON_AddNumberToObject(item, "altitude_msl", uav->location.altitude_geo);
            cJSON_AddNumberToObject(item, "altitude_agl", uav->location.height);
            cJSON_AddNumberToObject(item, "speed_h", uav->location.speed_horizontal);
            cJSON_AddNumberToObject(item, "speed_v", uav->location.speed_vertical);
            cJSON_AddNumberToObject(item, "heading", uav->location.direction);
            cJSON_AddStringToObject(item, "status_text", status_text(uav->location.status));
        }
        if (uav->operator_id.valid) {
            cJSON_AddStringToObject(item, "operator_id", uav->operator_id.id);
            cJSON_AddNumberToObject(item, "operator_lat", uav->system.operator_latitude);
            cJSON_AddNumberToObject(item, "operator_lon", uav->system.operator_longitude);
        }
        cJSON_AddStringToObject(item, "protocol_name", protocol_name(uav->protocol));
        cJSON_AddNumberToObject(item, "first_seen", uav->first_seen_ms);
        cJSON_AddNumberToObject(item, "last_seen", uav->last_seen_ms);
        add_seen_times(item, uav->first_seen_ms, uav->last_seen_ms);
        cJSON_AddItemToArray(list, item);
    }

    cJSON_AddNumberToObject(root, "uptime_ms", (double)esp_log_timestamp());
    cJSON_AddItemToObject(root, "list", list);
    
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}

/* ================================================================
 * API: POST /api/sim_toggle
 * ================================================================ */
static esp_err_t api_sim_toggle_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = 0;
        cJSON *json = cJSON_Parse(buf);
        if (json) {
            cJSON *running = cJSON_GetObjectItem(json, "running");
            if (running && cJSON_IsBool(running)) {
                g_sim.running = cJSON_IsTrue(running);
                /* 开关状态落盘：断电/重启后模拟器自动续跑，
               这样"一块板单独供电当发射源、另一块负责侦听"的双板联调才成立 */
                crid_nvs_save_sim_config(&g_sim, sizeof(g_sim));
                ESP_LOGI(TAG, "Simulator %s (saved)", g_sim.running ? "ON" : "OFF");
            }
            cJSON_Delete(json);
        }
    }

    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: GET /api/sim_config
 * ================================================================ */
static esp_err_t api_sim_config_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", g_sim.count);
    cJSON_AddNumberToObject(root, "center_lat", g_sim.center_lat);
    cJSON_AddNumberToObject(root, "center_lon", g_sim.center_lon);
    cJSON_AddBoolToObject(root, "running", g_sim.running);
    cJSON_AddBoolToObject(root, "trajectory_active", g_sim.trajectory.active);
    cJSON_AddNumberToObject(root, "trajectory_points", g_sim.trajectory.total_points);
    cJSON_AddNumberToObject(root, "trajectory_index", g_sim.trajectory.current_index);
    
    // First drone details
    cJSON_AddStringToObject(root, "uas_id", g_sim.drones[0].uas_id);
    cJSON_AddStringToObject(root, "drone_name", g_sim.drones[0].drone_name);
    cJSON_AddNumberToObject(root, "ua_type", g_sim.drones[0].ua_type);
    cJSON_AddNumberToObject(root, "latitude", g_sim.drones[0].latitude);
    cJSON_AddNumberToObject(root, "longitude", g_sim.drones[0].longitude);
    cJSON_AddNumberToObject(root, "trajectory_altitude",
                            (g_sim.trajectory.altitudes && g_sim.trajectory.total_points > 0)
                                ? g_sim.trajectory.altitudes[0] : 0);
    cJSON_AddNumberToObject(root, "altitude_msl", g_sim.drones[0].altitude_msl);
    cJSON_AddNumberToObject(root, "altitude_agl", g_sim.drones[0].altitude_agl);
    cJSON_AddNumberToObject(root, "speed_horizontal", g_sim.drones[0].speed_horizontal);
    cJSON_AddNumberToObject(root, "speed_vertical", g_sim.drones[0].speed_vertical);
    cJSON_AddNumberToObject(root, "heading", g_sim.drones[0].heading);
    cJSON_AddNumberToObject(root, "status", g_sim.drones[0].status);
    cJSON_AddStringToObject(root, "operator_id", g_sim.drones[0].operator_id);
    
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ================================================================
 * API: POST /api/sim_config
 * ================================================================ */
static esp_err_t api_sim_config_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char buf[4096];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = 0;
        cJSON *json = cJSON_Parse(buf);
        if (json) {
            cJSON *item;
            item = cJSON_GetObjectItem(json, "count");
            if (item && cJSON_IsNumber(item)) {
                int cnt = item->valueint;
                if (cnt < 1) cnt = 1;
                if (cnt > MAX_SIM_DRONES) cnt = MAX_SIM_DRONES;
                if (cnt != g_sim.count) {
                    g_sim.count = cnt;
                    for (int i = 0; i < cnt; i++) {
                        crid_config_init_random(&g_sim.drones[i], i, g_sim.center_lat, g_sim.center_lon);
                    }
                }
            }
            item = cJSON_GetObjectItem(json, "center_lat");
            if (item && cJSON_IsNumber(item)) {
                g_sim.center_lat = item->valuedouble;
                // Re-init all drones with new center
                for (int i = 0; i < g_sim.count; i++) {
                    crid_config_init_random(&g_sim.drones[i], i, g_sim.center_lat, g_sim.center_lon);
                }
            }
            item = cJSON_GetObjectItem(json, "center_lon");
            if (item && cJSON_IsNumber(item)) g_sim.center_lon = item->valuedouble;
            
            crid_nvs_save_sim_config(&g_sim, sizeof(g_sim));
            cJSON_Delete(json);
        }
    }
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: POST /api/clear_cache
 * ================================================================ */
static esp_err_t api_clear_cache_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    crid_nvs_clear_all();
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * Captive Portal 重定向地址（存 NVS，供 404 劫持与 /api/portal 共用）
 * ================================================================ */
#define PORTAL_URL_MAX 128

/* 默认跳转地址跟随 AP 网关实际 IP，AP 未起来时退回 192.168.4.1 */
static const char *portal_default_url(void) {
    static char def[48] = "http://192.168.4.1/";
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(def, sizeof(def), "http://" IPSTR "/", IP2STR(&ip.ip));
        }
    }
    return def;
}

static void portal_read(char *out, size_t len) {
    char tmp[PORTAL_URL_MAX];
    memset(tmp, 0, sizeof(tmp));
    if (crid_nvs_load_portal(tmp, sizeof(tmp)) > 0 && tmp[0]) {
        strlcpy(out, tmp, len);
    } else {
        strlcpy(out, portal_default_url(), len);
    }
}

/* ================================================================
 * Captive Portal 404 Handler (Redirects unknown URIs to portal URL)
 * ================================================================ */
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    char url[PORTAL_URL_MAX];
    portal_read(url, sizeof(url));
    if (!url[0]) strlcpy(url, portal_default_url(), sizeof(url));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ================================================================
 * Redirect all non-API requests to index.html (captive portal)
 * ================================================================ */
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


/* ================================================================
 * API: POST /api/trajectory - upload KMZ trajectory points
 * ================================================================ */
static esp_err_t api_trajectory_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    /* 轨迹包可能上千字节而 TCP 会分片，必须循环收完整个 body：
       原先只 recv 一次，超过一个 MSS 的轨迹会被静默截断成非法 JSON */
    int total = req->content_len;
    if (total <= 0 || total > MAX_TRAJECTORY_BODY) {
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"payload_size\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        received += r;
    }
    buf[received] = 0;

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        httpd_resp_send(req, "{\"ok\":false,\"err\":\"parse\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *err = NULL;
    int count = 0;
    cJSON *pts = cJSON_GetObjectItem(json, "points");
    if (!pts || !cJSON_IsArray(pts)) {
        err = "no_points";
    } else {
        count = cJSON_GetArraySize(pts);
        if (count <= 0) {
            err = "no_points";
        } else if (count > MAX_TRAJECTORY_POINTS) {
            err = "too_many";
        } else {
            double *lats = malloc(count * sizeof(double));
            double *lons = malloc(count * sizeof(double));
            float  *alts = malloc(count * sizeof(float));
            if (!lats || !lons) {
                err = "no_mem";
            } else {
                /* 缺省高度：优先顶层 altitude，其次 120 m */
                float def_alt = 120.0f;
                cJSON *ja = cJSON_GetObjectItem(json, "altitude");
                if (ja && cJSON_IsNumber(ja)) def_alt = (float)ja->valuedouble;
                bool has_alt = false;
                for (int i = 0; i < count; i++) {
                    cJSON *pt = cJSON_GetArrayItem(pts, i);
                    cJSON *lat = pt ? cJSON_GetObjectItem(pt, "lat") : NULL;
                    cJSON *lon = pt ? cJSON_GetObjectItem(pt, "lon") : NULL;
                    cJSON *alt = pt ? cJSON_GetObjectItem(pt, "alt") : NULL;
                    lats[i] = (lat && cJSON_IsNumber(lat)) ? lat->valuedouble : 0;
                    lons[i] = (lon && cJSON_IsNumber(lon)) ? lon->valuedouble : 0;
                    if (alts) {
                        float a = (alt && cJSON_IsNumber(alt)) ? (float)alt->valuedouble : def_alt;
                        if (a < -100.0f) a = -100.0f;
                        if (a > 10000.0f) a = 10000.0f;
                        alts[i] = a;
                        if (alt && cJSON_IsNumber(alt)) has_alt = true;
                    }
                    if (!lat || !lon || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
                        err = "bad_point";
                        break;
                    }
                }
                if (!err) {
                    float spd = 1.0f;
                    cJSON *s = cJSON_GetObjectItem(json, "speed");
                    if (s && cJSON_IsNumber(s)) spd = (float)s->valuedouble;
                    crid_web_set_trajectory(lats, lons, alts, count, spd);
                    if (!has_alt) {
                        ESP_LOGW(TAG, "trajectory without per-point alt, using default %.0f m", def_alt);
                    }
                }
                free(lats);
                free(lons);
                if (alts) free(alts);
            }
        }
    }
    cJSON_Delete(json);

    if (err) {
        char resp[96];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"err\":\"%s\",\"count\":%d,\"max\":%d}",
                 err, count, MAX_TRAJECTORY_POINTS);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%d}", count);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * BLE 快门 / 拍照 API
 * ================================================================ */
static esp_err_t api_photo_status_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    shutter_status_t st;
    crid_shutter_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ble_connected", st.connected);
    cJSON_AddBoolToObject(root, "ble_ready", st.ready);
    cJSON_AddBoolToObject(root, "continuous", st.continuous);
    cJSON_AddNumberToObject(root, "interval", st.interval);
    cJSON_AddStringToObject(root, "key", st.key == SHUTTER_KEY_VOL_DOWN ? "vol_down" : "vol_up");
    cJSON_AddNumberToObject(root, "shots", st.shots);
    cJSON_AddNumberToObject(root, "passkey", st.passkey);
    cJSON_AddBoolToObject(root, "pairing", st.pairing);
    cJSON_AddStringToObject(root, "name", st.name);
    cJSON_AddStringToObject(root, "mode", crid_mode_name(crid_mode_get()));
    cJSON_AddBoolToObject(root, "switching", crid_mode_is_switching());

    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_photo_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    shutter_status_t st;
    crid_shutter_get_status(&st);

    if (crid_mode_get() != CRID_MODE_CAMERA) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"当前为 RID 模式，请先切换到拍照模式\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!st.connected) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"蓝牙未连接，请先在手机蓝牙中配对\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool ok = crid_shutter_take_photo();
    httpd_resp_send(req, ok ? "{\"ok\":true,\"msg\":\"拍照成功\"}"
                            : "{\"ok\":false,\"msg\":\"拍照失败，请重试\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_photo_config_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[384];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    bool changed = false;

    if (received > 0) {
        buf[received] = 0;
        cJSON *json = cJSON_Parse(buf);
        if (json) {
            cJSON *item = cJSON_GetObjectItem(json, "interval");
            if (item && cJSON_IsNumber(item)) {
                crid_shutter_set_interval((uint32_t)item->valueint);
                changed = true;
            }
            item = cJSON_GetObjectItem(json, "key");
            if (item && cJSON_IsString(item)) {
                crid_shutter_set_key(strcmp(item->valuestring, "vol_down") == 0
                                     ? SHUTTER_KEY_VOL_DOWN : SHUTTER_KEY_VOL_UP);
                changed = true;
            }
            item = cJSON_GetObjectItem(json, "name");
            if (item && cJSON_IsString(item) && item->valuestring[0]) {
                if (crid_shutter_set_name(item->valuestring) == ESP_OK) changed = true;
            }
            item = cJSON_GetObjectItem(json, "continuous");
            if (item && cJSON_IsBool(item)) {
                crid_shutter_set_continuous(cJSON_IsTrue(item));
            }
            cJSON_Delete(json);
        }
    }

    if (changed) crid_shutter_save_config();

    shutter_status_t st;
    crid_shutter_get_status(&st);
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"interval\":%lu,\"key\":\"%s\",\"continuous\":%s,\"msg\":\"设置已保存\"}",
             (unsigned long)st.interval,
             st.key == SHUTTER_KEY_VOL_DOWN ? "vol_down" : "vol_up",
             st.continuous ? "true" : "false");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_photo_stop_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    crid_shutter_set_continuous(false);
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"连拍已暂停\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_ble_forget_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    int rc = crid_shutter_clear_bonds();
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"rc\":%d,\"msg\":\"配对信息已清除\"}",
             rc == 0 ? "true" : "false", rc);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: 远程键盘 /api/key  /api/key/text
 * ================================================================ */
static esp_err_t api_key_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    shutter_status_t st;
    crid_shutter_get_status(&st);

    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"mode\":\"%s\",\"ble_ready\":%s,\"ble_connected\":%s,\"name\":\"%s\"}",
             crid_mode_name(crid_mode_get()),
             st.ready ? "true" : "false",
             st.connected ? "true" : "false",
             st.name);
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 键盘键码名称 → HID 用法码，便于前端用可读名配置快捷键 */
typedef struct { const char *name; uint8_t code; } key_name_t;
static const key_name_t kKeyNames[] = {
    {"enter", 0x28}, {"esc", 0x29}, {"backspace", 0x2A}, {"tab", 0x2B}, {"space", 0x2C},
    {"up", 0x52}, {"down", 0x51}, {"left", 0x50}, {"right", 0x4F},
    {"home", 0x4A}, {"end", 0x4D}, {"pageup", 0x4B}, {"pagedown", 0x4E},
    {"delete", 0x4C}, {"insert", 0x49},
    {"f1", 0x3A}, {"f2", 0x3B}, {"f3", 0x3C}, {"f4", 0x3D}, {"f5", 0x3E},
    {"f6", 0x3F}, {"f7", 0x40}, {"f8", 0x41}, {"f9", 0x42}, {"f10", 0x43},
    {"f11", 0x44}, {"f12", 0x45},
    {"volume_up", 0x80}, {"volume_down", 0x81}, {"mute", 0x7F},
    {"play_pause", 0xCD}, {"next", 0xB5}, {"prev", 0xB6},
    {"capslock", 0x39}, {"printscreen", 0x46},
};

static uint8_t key_name_to_code(const char *nm, uint8_t *mod) {
    if (!nm) return 0;
    if (strlen(nm) == 1) {
        uint8_t c = (uint8_t)nm[0];
        if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
        if (c >= 'A' && c <= 'Z') { if (mod) *mod |= 0x02; return (uint8_t)(0x04 + (c - 'A')); }
        if (c >= '1' && c <= '9') return (uint8_t)(0x1E + (c - '1'));
        if (c == '0') return 0x27;
    }
    for (size_t i = 0; i < sizeof(kKeyNames) / sizeof(kKeyNames[0]); i++) {
        if (strcasecmp(nm, kKeyNames[i].name) == 0) return kKeyNames[i].code;
    }
    return 0;
}

/* POST /api/key
 * body: {"key":4} 或 {"key":"enter"}，可选 {"mods":["ctrl","shift"]} 或 {"mod":3}
 *       {"action":"tap"|"down"|"up"}，默认 tap */
static esp_err_t api_key_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    shutter_status_t st;
    crid_shutter_get_status(&st);
    if (crid_mode_get() != CRID_MODE_CAMERA) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请先切换到蓝牙键盘模式\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!st.connected) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"蓝牙未连接，请先在系统蓝牙中配对\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请求体为空\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    uint8_t mod = 0, code = 0;
    bool down = false, up = false;

    cJSON *json = cJSON_Parse(buf);
    if (json) {
        cJSON *item = cJSON_GetObjectItem(json, "mod");
        if (item && cJSON_IsNumber(item)) mod = (uint8_t)item->valueint;

        item = cJSON_GetObjectItem(json, "mods");
        if (item && cJSON_IsArray(item)) {
            cJSON *m = NULL;
            cJSON_ArrayForEach(m, item) {
                if (!cJSON_IsString(m)) continue;
                const char *v = m->valuestring;
                if (strcasecmp(v, "ctrl") == 0)       mod |= 0x01;
                else if (strcasecmp(v, "shift") == 0) mod |= 0x02;
                else if (strcasecmp(v, "alt") == 0)   mod |= 0x04;
                else if (strcasecmp(v, "gui") == 0 ||
                         strcasecmp(v, "win") == 0)   mod |= 0x08;
            }
        }

        item = cJSON_GetObjectItem(json, "key");
        if (item && cJSON_IsNumber(item)) {
            code = (uint8_t)item->valueint;
        } else if (item && cJSON_IsString(item)) {
            code = key_name_to_code(item->valuestring, &mod);
        }

        item = cJSON_GetObjectItem(json, "action");
        if (item && cJSON_IsString(item)) {
            if (strcmp(item->valuestring, "down") == 0) down = true;
            else if (strcmp(item->valuestring, "up") == 0) up = true;
        }
        cJSON_Delete(json);
    }

    if (code == 0 && !up) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"缺少 key 参数\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool ok;
    if (down)      ok = crid_shutter_key_hold(mod, code, true);
    else if (up)   ok = crid_shutter_key_hold(0, 0, false);
    else           ok = crid_shutter_key_tap(mod, code);

    httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"msg\":\"发送失败\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/key/text  body: {"text":"hello"} */
static esp_err_t api_key_text_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    shutter_status_t st;
    crid_shutter_get_status(&st);
    if (crid_mode_get() != CRID_MODE_CAMERA || !st.connected) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"蓝牙键盘未连接\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char buf[192];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请求体为空\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    const char *text = NULL;
    char plain[96];
    cJSON *json = cJSON_Parse(buf);
    if (json) {
        cJSON *item = cJSON_GetObjectItem(json, "text");
        if (item && cJSON_IsString(item)) {
            strlcpy(plain, item->valuestring, sizeof(plain));
            text = plain;
        }
        cJSON_Delete(json);
    }

    if (!text) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"缺少 text\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    int sent = crid_shutter_type_text(text);
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"sent\":%d}", sent >= 0 ? "true" : "false", sent);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: 中继配置 /api/relay  /api/relay_config
 * ================================================================ */
static esp_err_t api_relay_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char status[256];
    crid_relay_status_json(status, sizeof(status));

    crid_relay_cfg_t cfg;
    crid_relay_load_cfg(&cfg);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"cfg\":{\"ap_ssid\":\"%s\",\"ap_pass\":\"%s\"},\"status\":%s}",
             cfg.ap_ssid, cfg.ap_pass, status);
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* POST /api/relay_config  body: {"ap_ssid":"xxx","ap_pass":"yyy"} —— 重启后生效 */
static esp_err_t api_relay_config_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[192];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请求体为空\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    crid_relay_cfg_t cfg;
    crid_relay_load_cfg(&cfg);
    char ssid[33], pass[65];
    strlcpy(ssid, cfg.ap_ssid, sizeof(ssid));
    strlcpy(pass, cfg.ap_pass, sizeof(pass));

    cJSON *json = cJSON_Parse(buf);
    if (json) {
        cJSON *item = cJSON_GetObjectItem(json, "ap_ssid");
        if (item && cJSON_IsString(item) && item->valuestring[0]) {
            strlcpy(ssid, item->valuestring, sizeof(ssid));
        }
        item = cJSON_GetObjectItem(json, "ap_pass");
        if (item && cJSON_IsString(item)) {
            strlcpy(pass, item->valuestring, sizeof(pass));
        }
        cJSON_Delete(json);
    }

    esp_err_t err = crid_relay_save_cfg(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"保存失败，SSID 需 1~32 字节\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send(req,
                    "{\"ok\":true,\"msg\":\"已保存，热点名称将在设备重启后生效\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * API: 模式切换 GET/POST /api/mode
 * ================================================================ */
static esp_err_t api_mode_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"switching\":%s,\"heap_kb\":%lu}",
             crid_mode_name(crid_mode_get()),
             crid_mode_is_switching() ? "true" : "false",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_mode_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    crid_mode_t target = crid_mode_get();

    if (received > 0) {
        buf[received] = 0;
        cJSON *json = cJSON_Parse(buf);
        if (json) {
            cJSON *item = cJSON_GetObjectItem(json, "mode");
            if (item && cJSON_IsString(item)) {
                if (strcmp(item->valuestring, "camera") == 0) target = CRID_MODE_CAMERA;
                else if (strcmp(item->valuestring, "relay") == 0) target = CRID_MODE_RELAY;
                else if (strcmp(item->valuestring, "rid") == 0) target = CRID_MODE_RID;
            }
            cJSON_Delete(json);
        }
    }

    if (target == crid_mode_get()) {
        httpd_resp_send(req, "{\"ok\":true,\"msg\":\"已处于该模式\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    esp_err_t err = crid_mode_request(target);
    if (err == ESP_OK) {
        char resp[192];
        const char *mname = (target == CRID_MODE_CAMERA) ? "蓝牙键盘"
                          : (target == CRID_MODE_RELAY)  ? "网络中继" : "RID 侦听";
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"msg\":\"设备将重启进入%s模式，WiFi 会中断约 5 秒，请稍后重新连接并刷新页面\"}",
                 mname);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"正在切换中，请稍候\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"切换请求失败\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

/* ================================================================
 * API: POST /api/time — 浏览器下发时间，建立设备时间基准
 * 载荷：{"ts": <UTC 毫秒>, "tz": <时区偏移分钟，东八区=480>}
 * ================================================================ */
static esp_err_t api_time_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[160];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received < 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"读取请求体失败\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    double ts = 0;
    int tz = 480;                                   /* 默认东八区 */
    cJSON *json = cJSON_Parse(buf);
    if (json) {
        cJSON *t = cJSON_GetObjectItem(json, "ts");
        if (t && cJSON_IsNumber(t)) ts = t->valuedouble;
        cJSON *z = cJSON_GetObjectItem(json, "tz");
        if (z && cJSON_IsNumber(z)) tz = (int)z->valuedouble;
        cJSON_Delete(json);
    }

    esp_err_t err = crid_clock_sync((int64_t)ts, tz);
    if (err != ESP_OK) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"时间戳非法\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char now_s[32];
    crid_clock_now_str(now_s, sizeof(now_s));
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"msg\":\"时间基准已同步\",\"now\":\"%s\",\"tz_min\":%d,\"unix_ms\":%lld}",
             now_s, crid_clock_tz_minutes(), (long long)crid_clock_unix_ms());
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * URI Handlers
 * ================================================================ */
/* ================================================================
 * API: GET /api/relay_clients — 本机热点已连接设备
 * ================================================================ */
static esp_err_t api_relay_clients_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char *js = crid_relay_clients_json();
    if (js == NULL) {
        httpd_resp_send(req, "{\"ok\":false,\"count\":0,\"list\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
    free(js);
    return ESP_OK;
}


/* ================================================================
 * API: /api/portal - Captive Portal 重定向地址（可读写，存 NVS）
 *   GET  -> {"ok":true,"url":"http://192.168.4.1/","default":"..."}
 *   POST -> {"url":"http://example.com/"}  留空则恢复默认
 * ================================================================ */
#define PORTAL_URL_MAX 128

static esp_err_t api_portal_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");
    char url[PORTAL_URL_MAX];
    portal_read(url, sizeof(url));
    char resp[PORTAL_URL_MAX + 96];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"url\":\"%s\",\"def\":\"%s\"}",
             url, portal_default_url());
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_portal_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[PORTAL_URL_MAX + 96];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请求体为空\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    char url[PORTAL_URL_MAX];
    portal_read(url, sizeof(url));          /* 默认保留当前值，未提供的字段不动 */

    cJSON *json = cJSON_Parse(buf);
    if (json) {
        cJSON *item = cJSON_GetObjectItem(json, "url");
        if (item && cJSON_IsString(item)) strlcpy(url, item->valuestring, sizeof(url));
        cJSON_Delete(json);
    }

    /* 只允许 http / https，避免把客户端弹到 file:// 之类的危险协议 */
    if (url[0] && strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"只支持 http:// 或 https:// 开头\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool ok = crid_nvs_save_portal(url);   /* 空串 = 清除，回落到默认地址 */
    char resp[PORTAL_URL_MAX + 96];
    snprintf(resp, sizeof(resp), "{\"ok\":%s,\"url\":\"%s\",\"msg\":\"%s\"}",
             ok ? "true" : "false", url,
             ok ? "已保存到设备，连上热点的设备将跳转到这里" : "写入 Flash 失败");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ================================================================
 * /api/oled — 板载 0.42" 屏幕状态与偏移校准
 * ================================================================ */
static esp_err_t api_oled_get(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"ready\":%s,\"present\":%s,\"on\":%s,\"x_offset\":%d,\"y_offset\":%d,"
             "\"w\":%d,\"h\":%d,\"page\":%d,\"pages\":%d,\"ui\":%d}",
             crid_oled_ready() ? "true" : "false",
             crid_oled_ready() ? "true" : "false",
             crid_screen_is_on() ? "true" : "false",
             crid_oled_get_x_offset(), crid_oled_get_y_offset(),
             CRID_OLED_W, CRID_OLED_HEIGHT,
             crid_screen_get_page(), crid_screen_get_page_count(),
             crid_screen_get_ui());
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_oled_post(httpd_req_t *req) {
    add_cors_headers(req);
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"请求体为空\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[received] = 0;

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send(req, "{\"ok\":false,\"msg\":\"JSON 解析失败\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *item;
    item = cJSON_GetObjectItem(json, "x_offset");
    if (item && cJSON_IsNumber(item)) {
        crid_oled_set_offset((int)item->valuedouble, crid_oled_get_y_offset());
    }
    item = cJSON_GetObjectItem(json, "y_offset");
    if (item && cJSON_IsNumber(item)) {
        crid_oled_set_offset(crid_oled_get_x_offset(), (int)item->valuedouble);
    }
    item = cJSON_GetObjectItem(json, "contrast");
    if (item && cJSON_IsNumber(item)) {
        crid_oled_contrast((uint8_t)item->valuedouble);
    }
    item = cJSON_GetObjectItem(json, "on");
    if (item && cJSON_IsBool(item)) {
        crid_screen_set_on(cJSON_IsTrue(item));
    }
    item = cJSON_GetObjectItem(json, "page");
    if (item && cJSON_IsNumber(item)) {
        crid_screen_set_page((int)item->valuedouble);
    }
    item = cJSON_GetObjectItem(json, "menu");
    if (item && cJSON_IsNumber(item)) {
        crid_screen_set_ui((int)item->valuedouble);
    }
    item = cJSON_GetObjectItem(json, "test");
    if (item && cJSON_IsNumber(item)) {
        crid_oled_test_pattern((int)item->valuedouble);
    }

    cJSON_Delete(json);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"x_offset\":%d,\"y_offset\":%d,\"msg\":\"屏幕参数已更新\"}",
             crid_oled_get_x_offset(), crid_oled_get_y_offset());
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* 显存导出：GET /api/oled_dump，返回 72x40 的 ASCII 图（排障用） */
static esp_err_t api_oled_dump_get(httpd_req_t *req)
{
    add_cors_headers(req);

    char *art = heap_caps_malloc(3200, MALLOC_CAP_8BIT);
    if (!art) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_send(req, "no memory", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_err_t e = crid_oled_dump(art, 3200);
    if (e != ESP_OK) {
        free(art);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "dump failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, art, HTTPD_RESP_USE_STRLEN);
    free(art);
    return ESP_OK;
}

static const httpd_uri_t uri_handlers[] = {
    {.uri = "/api/trajectory", .method = HTTP_POST, .handler = api_trajectory_post},
    {.uri = "/",              .method = HTTP_GET,  .handler = root_handler},
    {.uri = "/index.html",    .method = HTTP_GET,  .handler = root_handler},
    {.uri = "/api/status",    .method = HTTP_GET,  .handler = api_status_get},
    {.uri = "/api/drones",    .method = HTTP_GET,  .handler = api_drones_get},
    {.uri = "/api/sim_config",.method = HTTP_GET,  .handler = api_sim_config_get},
    {.uri = "/api/sim_config",.method = HTTP_POST, .handler = api_sim_config_post},
    {.uri = "/api/sim_toggle",.method = HTTP_POST, .handler = api_sim_toggle_post},
    {.uri = "/api/clear_cache",.method = HTTP_POST,.handler = api_clear_cache_post},
    {.uri = "/api/photo_status",.method = HTTP_GET, .handler = api_photo_status_get},
    {.uri = "/api/photo",      .method = HTTP_POST,.handler = api_photo_post},
    {.uri = "/api/photo_config",.method = HTTP_POST,.handler = api_photo_config_post},
    {.uri = "/api/photo_stop", .method = HTTP_POST,.handler = api_photo_stop_post},
    {.uri = "/api/ble_forget",.method = HTTP_POST,.handler = api_ble_forget_post},
    {.uri = "/api/key",        .method = HTTP_GET, .handler = api_key_get},
    {.uri = "/api/key",        .method = HTTP_POST,.handler = api_key_post},
    {.uri = "/api/key/text",   .method = HTTP_POST,.handler = api_key_text_post},
    {.uri = "/api/relay",      .method = HTTP_GET, .handler = api_relay_get},
    {.uri = "/api/relay_clients", .method = HTTP_GET, .handler = api_relay_clients_get},
    {.uri = "/api/relay_config",.method = HTTP_POST,.handler = api_relay_config_post},
    {.uri = "/api/mode",       .method = HTTP_GET, .handler = api_mode_get},
    {.uri = "/api/mode",       .method = HTTP_POST,.handler = api_mode_post},
    {.uri = "/api/time",       .method = HTTP_POST,.handler = api_time_post},
    {.uri = "/api/portal",     .method = HTTP_GET, .handler = api_portal_get},
    {.uri = "/api/portal",     .method = HTTP_POST,.handler = api_portal_post},
    {.uri = "/api/oled",       .method = HTTP_GET, .handler = api_oled_get},
    {.uri = "/api/oled",       .method = HTTP_POST,.handler = api_oled_post},
    {.uri = "/api/oled_dump",  .method = HTTP_GET, .handler = api_oled_dump_get},
};

static httpd_handle_t g_server = NULL;

void crid_web_init(void) {
    memset(&g_sim, 0, sizeof(g_sim));
    g_sim.count = 1;
    g_sim.center_lat = 39.9042;
    g_sim.center_lon = 116.4074;
    crid_config_init_random(&g_sim.drones[0], 0, g_sim.center_lat, g_sim.center_lon);
    size_t loaded = crid_nvs_load_sim_config(&g_sim, sizeof(g_sim));
    if (loaded > 0 && g_sim.count > 0) {
        ESP_LOGI(TAG, "Loaded saved sim config: %d drones", g_sim.count);
    }
}

void crid_web_start(void) {
    // Start DNS server for captive portal
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS captive portal task created");

    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 48;
    config.stack_size = 8192;
    config.max_open_sockets = 8;      /* 留出 socket 给 DNS 捕获与 STA 侧连接 */
    config.backlog_conn = 6;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    
    if (httpd_start(&g_server, &config) == ESP_OK) {
        httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, http_404_error_handler);
        for (int i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
            httpd_register_uri_handler(g_server, &uri_handlers[i]);
        }
        /* 对外服务接口 /api/ext/…（GET + POST 双方法） */
        crid_ext_register_handlers(g_server);
        ESP_LOGI(TAG, "HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server!");
    }
}

/* ================================================================
 * 释放轨迹内存（供 /api/ext/clear?deep=1 使用）
 * ================================================================ */
void crid_web_release_trajectory(void) {
    g_sim.trajectory.active = false;
    g_sim.running = false;
    vTaskDelay(pdMS_TO_TICKS(100));   /* 让 simulator_task 跳出当前帧 */

    if (g_sim.trajectory.latitudes)  { free(g_sim.trajectory.latitudes);  g_sim.trajectory.latitudes  = NULL; }
    if (g_sim.trajectory.longitudes) { free(g_sim.trajectory.longitudes); g_sim.trajectory.longitudes = NULL; }
    if (g_sim.trajectory.altitudes)  { free(g_sim.trajectory.altitudes);  g_sim.trajectory.altitudes  = NULL; }
    g_sim.trajectory.total_points = 0;
    g_sim.trajectory.current_index = 0;
    ESP_LOGI(TAG, "trajectory released, heap=%u", (unsigned)esp_get_free_heap_size());
}
