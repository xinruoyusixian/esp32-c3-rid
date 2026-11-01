/**
 * crid_relay.c — 网络中继模式实现（AP + STA + NAPT）
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "crid_relay.h"
#include "crid_ext.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

static const char *TAG = "RELAY";

#define RELAY_NS          "relay"
#define KEY_AP_SSID       "ap_ssid"
#define KEY_AP_PASS       "ap_pass"

#define DEFAULT_AP_SSID   "rid"
#define DEFAULT_AP_PASS   "12345678"

static esp_netif_t *s_ap_netif = NULL;
static bool         s_napt_on  = false;
static char         s_sta_ip[16] = "0.0.0.0";
static char         s_gw_ip[16]  = "0.0.0.0";
static uint32_t     s_napt_at_ms = 0;

/* ================================================================
 * 配置
 * ================================================================ */
void crid_relay_load_cfg(crid_relay_cfg_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    snprintf(out->ap_ssid, sizeof(out->ap_ssid), "%s", DEFAULT_AP_SSID);
    snprintf(out->ap_pass, sizeof(out->ap_pass), "%s", DEFAULT_AP_PASS);

    nvs_handle_t h;
    if (nvs_open(RELAY_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t len = sizeof(out->ap_ssid);
    if (nvs_get_str(h, KEY_AP_SSID, out->ap_ssid, &len) != ESP_OK || out->ap_ssid[0] == 0) {
        snprintf(out->ap_ssid, sizeof(out->ap_ssid), "%s", DEFAULT_AP_SSID);
    }
    len = sizeof(out->ap_pass);
    if (nvs_get_str(h, KEY_AP_PASS, out->ap_pass, &len) != ESP_OK) {
        snprintf(out->ap_pass, sizeof(out->ap_pass), "%s", DEFAULT_AP_PASS);
    }
    nvs_close(h);
}

esp_err_t crid_relay_save_cfg(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == 0 || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (pass != NULL && strlen(pass) > 64) return ESP_ERR_INVALID_ARG;
    if (pass != NULL && pass[0] != 0 && strlen(pass) < 8) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(RELAY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, KEY_AP_SSID, ssid);
    nvs_set_str(h, KEY_AP_PASS, pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "热点配置已保存: %s / %s", ssid, (pass && pass[0]) ? "(已设密码)" : "(开放)");
    }
    return err;
}

/* ================================================================
 * 已连设备表
 * 之前只靠 esp_netif_dhcps_get_clients_by_mac() 反查，STA 列表与 DHCP
 * 租约表对不上时 IP 恒为 "--"。这里改为监听 IP_EVENT_AP_STAIPASSIGNED，
 * 在分配瞬间就把 MAC->IP 记下来，查询时优先用这张表。
 * ================================================================ */
#define RELAY_MAX_CLIENTS 10

typedef struct {
    bool used;
    uint8_t mac[6];
    esp_ip4_addr_t ip;
    uint32_t seen_ms;
} relay_client_t;

static relay_client_t s_clients[RELAY_MAX_CLIENTS];
static bool s_clients_hooked = false;

static void relay_clients_add(const uint8_t mac[6], const esp_ip4_addr_t *ip)
{
    if (mac == NULL || ip == NULL || ip->addr == 0) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    int free_slot = -1;
    for (int i = 0; i < RELAY_MAX_CLIENTS; i++) {
        if (s_clients[i].used && memcmp(s_clients[i].mac, mac, 6) == 0) {
            s_clients[i].ip = *ip;
            s_clients[i].seen_ms = now;
            return;
        }
        if (!s_clients[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    memcpy(s_clients[free_slot].mac, mac, 6);
    s_clients[free_slot].ip = *ip;
    s_clients[free_slot].seen_ms = now;
    s_clients[free_slot].used = true;
}

/* 清掉已经断开（不在 AP station 列表里）的表项，避免老 IP 残留 */
static void relay_clients_prune(const wifi_sta_list_t *wsl)
{
    for (int i = 0; i < RELAY_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) continue;
        bool alive = false;
        for (int k = 0; wsl && k < wsl->num; k++) {
            if (memcmp(wsl->sta[k].mac, s_clients[i].mac, 6) == 0) { alive = true; break; }
        }
        if (!alive) s_clients[i].used = false;
    }
}

static void relay_on_sta_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_ap_staipassigned_t *ev = (ip_event_ap_staipassigned_t *)data;
    if (ev) relay_clients_add(ev->mac, &ev->ip);
}

void crid_relay_clients_init(void)
{
    if (s_clients_hooked) return;
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                   relay_on_sta_ip, NULL) == ESP_OK) {
        s_clients_hooked = true;
    }
}

/* 返回堆上的 JSON 字符串，调用者负责 free()；失败返回 NULL */
int crid_relay_client_list(uint8_t macs[][6], char ips[][16], int max)
{
    if (macs == NULL || ips == NULL || max <= 0) return 0;

    wifi_sta_list_t wsl;
    memset(&wsl, 0, sizeof(wsl));
    esp_err_t we = esp_wifi_ap_get_sta_list(&wsl);

    int n = (we == ESP_OK && wsl.num > 0)
                ? ((wsl.num < RELAY_MAX_CLIENTS) ? wsl.num : RELAY_MAX_CLIENTS)
                : 0;
    relay_clients_prune(we == ESP_OK ? &wsl : NULL);
    if (n > max) n = max;
    if (n <= 0) return 0;

    esp_netif_pair_mac_ip_t pairs[RELAY_MAX_CLIENTS];
    memset(pairs, 0, sizeof(pairs));
    esp_err_t de = ESP_FAIL;
    if (s_ap_netif != NULL) {
        de = esp_netif_dhcps_get_clients_by_mac(s_ap_netif, n, pairs);
    }

    for (int i = 0; i < n; i++) {
        memcpy(macs[i], wsl.sta[i].mac, 6);

        const esp_ip4_addr_t *hit = NULL;
        /* 1) DHCP 事件记录的表（最可靠） */
        for (int k = 0; k < RELAY_MAX_CLIENTS && hit == NULL; k++) {
            if (s_clients[k].used && memcmp(s_clients[k].mac, wsl.sta[i].mac, 6) == 0
                && s_clients[k].ip.addr != 0) {
                hit = &s_clients[k].ip;
            }
        }
        /* 2) 退回 DHCP 租约表反查 */
        if (hit == NULL && de == ESP_OK) {
            for (int k = 0; k < n; k++) {
                if (memcmp(pairs[k].mac, wsl.sta[i].mac, 6) == 0 && pairs[k].ip.addr != 0) {
                    hit = &pairs[k].ip;
                    break;
                }
            }
        }

        if (hit) {
            snprintf(ips[i], 16, IPSTR, IP2STR(hit));
        } else {
            snprintf(ips[i], 16, "--");
        }
    }
    return n;
}

char *crid_relay_clients_json(void)
{
    uint8_t macs[RELAY_MAX_CLIENTS][6];
    char    ips[RELAY_MAX_CLIENTS][16];
    memset(macs, 0, sizeof(macs));
    memset(ips, 0, sizeof(ips));

    int n = crid_relay_client_list(macs, ips, RELAY_MAX_CLIENTS);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        char m[18];
        snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X",
                 macs[i][0], macs[i][1], macs[i][2],
                 macs[i][3], macs[i][4], macs[i][5]);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mac", m);
        cJSON_AddStringToObject(item, "ip", ips[i][0] ? ips[i] : "--");
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddNumberToObject(root, "count", n);
    cJSON_AddItemToObject(root, "list", list);

    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return js;
}

/* ================================================================
 * 流量统计
 *
 * lwIP 没开 SNMP/MIB2，拿不到网口字节计数，只能把 STA 网口的
 * input / linkoutput 换成计数包装函数。STA 口上的流量就是"上网流量"：
 *   linkoutput -> 发出去（客户端 -> 互联网） = 上传
 *   input      -> 收进来（互联网 -> 客户端） = 下载
 * ================================================================ */
static struct netif        *s_sta_nf        = NULL;
static netif_input_fn       s_orig_input    = NULL;
static netif_linkoutput_fn  s_orig_linkout  = NULL;
static volatile uint32_t    s_tx_bytes      = 0;   /* 上传累计（低 32 位，允许回绕） */
static volatile uint32_t    s_rx_bytes      = 0;   /* 下载累计 */
static uint32_t             s_tx_prev       = 0;
static uint32_t             s_rx_prev       = 0;
static uint64_t             s_tx_total      = 0;
static uint64_t             s_rx_total      = 0;
static int64_t              s_last_us       = 0;
static uint32_t             s_up_bps        = 0;
static uint32_t             s_dn_bps        = 0;

static err_t relay_count_input(struct pbuf *p, struct netif *inp)
{
    if (p != NULL) s_rx_bytes += p->tot_len;
    return (s_orig_input != NULL) ? s_orig_input(p, inp) : ERR_OK;
}

static err_t relay_count_linkout(struct netif *nf, struct pbuf *p)
{
    if (p != NULL) s_tx_bytes += p->tot_len;
    return (s_orig_linkout != NULL) ? s_orig_linkout(nf, p) : ERR_OK;
}

/* 按名字找 STA 网口：wlanif_init_sta() 把 name 设为 "st" */
static struct netif *relay_find_sta_netif(void)
{
    for (struct netif *nf = netif_list; nf != NULL; nf = nf->next) {
        if (nf->name[0] == 's' && nf->name[1] == 't') return nf;
    }
    return NULL;
}

void crid_relay_traffic_init(void)
{
    if (s_sta_nf != NULL) return;
    struct netif *nf = relay_find_sta_netif();
    if (nf == NULL || nf->input == NULL || nf->linkoutput == NULL) return;

    /* 别重复包一层（例如自己去调自己） */
    if (nf->input == relay_count_input || nf->linkoutput == relay_count_linkout) return;

    s_sta_nf       = nf;
    s_orig_input   = nf->input;
    s_orig_linkout = nf->linkoutput;
    nf->input      = relay_count_input;
    nf->linkoutput = relay_count_linkout;
    ESP_LOGI(TAG, "流量统计已挂载到 STA 网口 st%d", nf->num);
}

void crid_relay_traffic(uint32_t *up_bps, uint32_t *dn_bps,
                        uint64_t *up_total, uint64_t *dn_total)
{
    if (s_sta_nf == NULL) crid_relay_traffic_init();

    uint32_t tx = s_tx_bytes;
    uint32_t rx = s_rx_bytes;
    int64_t  now_us = esp_timer_get_time();

    if (s_last_us != 0) {
        uint32_t d_tx = tx - s_tx_prev;      /* 无符号减法天然处理回绕 */
        uint32_t d_rx = rx - s_rx_prev;
        int64_t  dt_us = now_us - s_last_us;
        s_tx_total += d_tx;
        s_rx_total += d_rx;

        if (dt_us > 0) {
            uint32_t inst_up = (uint32_t)((int64_t)d_tx * 1000000LL / dt_us);
            uint32_t inst_dn = (uint32_t)((int64_t)d_rx * 1000000LL / dt_us);
            /* 指数平滑：3/4 历史 + 1/4 瞬时，屏幕上的数字才不会乱跳 */
            s_up_bps = (uint32_t)(((uint64_t)s_up_bps * 3 + inst_up) / 4);
            s_dn_bps = (uint32_t)(((uint64_t)s_dn_bps * 3 + inst_dn) / 4);
        }
    }
    s_tx_prev = tx;
    s_rx_prev = rx;
    s_last_us = now_us;

    if (up_bps)   *up_bps   = s_up_bps;
    if (dn_bps)   *dn_bps   = s_dn_bps;
    if (up_total) *up_total = s_tx_total;
    if (dn_total) *dn_total = s_rx_total;
}

/* ================================================================
 * 网口
 * ================================================================ */
esp_netif_t *crid_relay_get_ap_netif(void)
{
    return s_ap_netif;
}

void crid_relay_set_ap_netif(esp_netif_t *ap_netif)
{
    s_ap_netif = ap_netif;
}

bool crid_relay_is_active(void) { return s_napt_on; }

/* DHCP 客户端下发 DNS：NAPT 会转发任意 UDP，故下发公网 DNS 即可 */
static void relay_set_dhcp_dns(void)
{
    if (s_ap_netif == NULL) return;

    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("119.29.29.29");
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_BACKUP, &dns);
    esp_err_t err = esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "AP DHCP 重新启动(下发 DNS 223.5.5.5/119.29.29.29): %s", esp_err_to_name(err));
}

void crid_relay_on_got_ip(const esp_ip4_addr_t *ip, const esp_ip4_addr_t *gw)
{
    if (ip != NULL) {
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(ip));
    }
    if (gw != NULL) {
        snprintf(s_gw_ip, sizeof(s_gw_ip), IPSTR, IP2STR(gw));
    }

    if (s_ap_netif == NULL) {
        ESP_LOGW(TAG, "AP 网口缺失，无法开启 NAPT");
        return;
    }

    if (!s_napt_on) {
        esp_err_t err = esp_netif_napt_enable(s_ap_netif);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NAPT 开启失败: %s", esp_err_to_name(err));
            return;
        }
        s_napt_on = true;
        s_napt_at_ms = esp_log_timestamp();
        relay_set_dhcp_dns();
        ESP_LOGI(TAG, "中继生效：AP(192.168.4.1) -> STA(%s) 已开启 NAPT", s_sta_ip);
    }
}

void crid_relay_on_sta_lost(void)
{
    snprintf(s_sta_ip, sizeof(s_sta_ip), "0.0.0.0");
    /* NAPT 保持开启：重连成功后无需再次设置 */
    ESP_LOGW(TAG, "中继 STA 掉线，等待重连（NAPT 保持%s）", s_napt_on ? "开启" : "关闭");
}

/* ================================================================
 * 启动
 * ================================================================ */
esp_err_t crid_relay_start(void)
{
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "AP 网口未注册");
        return ESP_ERR_INVALID_STATE;
    }

    const crid_ext_cfg_t *cfg = crid_ext_get_cfg();
    if (cfg == NULL || cfg->sta_ssid[0] == 0) {
        ESP_LOGW(TAG, "未配置上游 WiFi（STA SSID 为空），中继无法工作");
        return ESP_ERR_INVALID_STATE;
    }

    /* STA 网口此时已建好，挂上流量计数 */
    crid_relay_traffic_init();

    /* 让 crid_ext 的 STA 状态机以"中继"语义接管：常连、快速重试 */
    crid_ext_set_relay(true);
    crid_ext_relay_connect_now();

    ESP_LOGI(TAG, "中继模式启动，上游 SSID=%s，热点=192.168.4.1", cfg->sta_ssid);
    return ESP_OK;
}

/* ================================================================
 * 状态
 * ================================================================ */
bool crid_relay_sta_info(const char **ssid, const char **ip, const char **gw)
{
    static const char *empty = "";
    const crid_ext_cfg_t *cfg = crid_ext_get_cfg();

    if (ssid) *ssid = (cfg && cfg->sta_ssid[0]) ? cfg->sta_ssid : empty;
    if (ip)   *ip   = s_sta_ip[0] ? s_sta_ip : empty;
    if (gw)   *gw   = s_gw_ip[0]  ? s_gw_ip  : empty;

    return s_napt_on;
}

void crid_relay_status_json(char *buf, size_t n)
{
    if (buf == NULL || n == 0) return;

    const crid_ext_cfg_t *cfg = crid_ext_get_cfg();
    bool napt = s_napt_on;

    uint32_t up_bps = 0, dn_bps = 0;
    uint64_t up_tot = 0, dn_tot = 0;
    crid_relay_traffic(&up_bps, &dn_bps, &up_tot, &dn_tot);

    snprintf(buf, n,
             "{\"mode\":\"relay\",\"napt\":%s,\"ap_ip\":\"192.168.4.1\","
             "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"gw\":\"%s\",\"heap_kb\":%lu,"
             "\"up_bps\":%lu,\"dn_bps\":%lu,\"up_kb\":%lu,\"dn_kb\":%lu}",
             napt ? "true" : "false",
             cfg ? cfg->sta_ssid : "",
             s_sta_ip, s_gw_ip,
             (unsigned long)(esp_get_free_heap_size() / 1024),
             (unsigned long)up_bps, (unsigned long)dn_bps,
             (unsigned long)(up_tot / 1024), (unsigned long)(dn_tot / 1024));
}
