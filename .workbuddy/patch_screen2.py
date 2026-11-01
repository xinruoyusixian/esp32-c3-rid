# -*- coding: utf-8 -*-
"""重写 draw_rid / draw_camera / draw_relay"""
import io, os

P = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "main", "crid_screen.c")
src = io.open(P, encoding="utf-8").read()

start = src.index("static void draw_rid(void)")
end = src.index("/* ---------------- 刷新任务 ---------------- */")

new = r'''static void draw_rid(void)
{
    uav_stat_t st;
    collect_uav(&st);

    /* 目标一出现就自动跳到目标页：只在"从无到有"这一刻跳一次，
       用户手动翻回概览页不会被抢走 */
    if (st.n > 0 && !s_had_uav && (s_page % PAGES_RID) == 0) s_page = 1;
    s_had_uav = (st.n > 0);

    int idx = (st.n > 0) ? (s_uav_idx % st.n) : 0;
    char hd[16], b[24];

    switch (s_page % PAGES_RID) {
        case 0: {   /* 概览 / 扫描中 */
            header("RID MON");
            snprintf(b, sizeof(b), "UAV %02d/%02d", st.online, st.total);
            row(0, "", b);
            row(1, "", st.n > 0 ? "1X=VIEW" : "SCANNING");
            foot(2, crid_web_is_sim_running() ? "SIM ON" : "SCAN");
            break;
        }
        case 1: {   /* 目标：尾号 / 速度 / 高度 */
            if (st.n <= 0) {
                header("RID MON");
                row(0, "", "NO TARGET");
                foot(2, "");
                break;
            }
            const tgt_t *t = &st.t[idx];
            if (st.n > 1) snprintf(hd, sizeof(hd), "UAV %d/%d", idx + 1, st.n);
            else          snprintf(hd, sizeof(hd), "UAV");
            header(hd);
            row(0, "", t->id);
            snprintf(b, sizeof(b), "SPD %.1fm/s", (double)t->spd);
            row(1, "", b);
            snprintf(b, sizeof(b), "ALT %.0fm", (double)t->alt);
            foot(2, b);
            break;
        }
        default: {  /* 目标：尾号 / 航向 / RSSI */
            if (st.n <= 0) {
                header("RID MON");
                row(0, "", "NO TARGET");
                foot(2, "");
                break;
            }
            const tgt_t *t = &st.t[idx];
            if (st.n > 1) snprintf(hd, sizeof(hd), "UAV %d/%d", idx + 1, st.n);
            else          snprintf(hd, sizeof(hd), "UAV");
            header(hd);
            row(0, "", t->id);
            snprintf(b, sizeof(b), "HDG %.0f", (double)t->hdg);
            row(1, "", b);
            snprintf(b, sizeof(b), "RSSI %d", t->rssi);
            foot(2, b);
            break;
        }
    }
}

/* 键盘模式：屏幕只做拍照控制
 *   停止时  GAP <档位> / 提示 / 已拍张数
 *   运行时  左侧大号倒计时 + 右侧档位与张数 */
static void draw_camera(void)
{
    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);

    char gap[16];
    if (st.gap == SHUTTER_GAP_MANUAL) snprintf(gap, sizeof(gap), "MANUAL");
    else snprintf(gap, sizeof(gap), "%lus", (unsigned long)st.gap);

    if (st.continuous && st.gap > 0) {
        header("SHOT");
        int secs = (int)st.next_in;
        if (secs < 0) secs = 0;
        if (secs > 99) secs = 99;
        int w = big_num(2, ROW0_Y, secs, 3);       /* 每位 9x15 px */
        crid_oled_text(2 + w - 3 + 2, ROW0_Y + 8, "s", 0);

        char b[24];
        snprintf(b, sizeof(b), "GAP %s", gap);     /* 最长 "GAP 60s" 7 字符 */
        crid_oled_text(30, ROW0_Y, b, 0);
        if (st.shots <= 99999UL) snprintf(b, sizeof(b), "SHOT %lu", (unsigned long)st.shots);
        else                     snprintf(b, sizeof(b), "SHOT %luk", (unsigned long)(st.shots / 1000UL));
        crid_oled_text(30, ROW0_Y + ROW_H, b, 0);
        row(2, "", st.connected ? "LINKED" : "NO LINK");
        return;
    }

    header("SHUTTER");
    row(0, "GAP ", gap);
    if (!st.connected)      row(1, "", "NO LINK");
    else if (st.gap == 0)   row(1, "", "1X = SHOT");
    else                    row(1, "", "2X = START");
    {
        char b[24];
        snprintf(b, sizeof(b), "SHOTS %lu", (unsigned long)st.shots);
        foot(2, b);
    }
}

/* 中继模式：第 0 页自动轮播 客户端数量 / 上传 / 下载 / 数据量，
 * 第 1 页是客户端列表（单击切换） */
static void draw_relay(void)
{
    /* 直接读结构化状态，避免在本任务栈上做 JSON 生成/解析（栈只有 4 KB） */
    const char *ssid = "";
    const char *ip   = "";
    const char *gw   = "";
    bool napt = crid_relay_sta_info(&ssid, &ip, &gw);

    uint8_t macs[3][6];
    char    cips[3][16];
    memset(macs, 0, sizeof(macs));
    memset(cips, 0, sizeof(cips));
    int clients = crid_relay_client_list(macs, cips, 3);

    uint32_t up_bps = 0, dn_bps = 0;
    uint64_t up_tot = 0, dn_tot = 0;
    crid_relay_traffic(&up_bps, &dn_bps, &up_tot, &dn_tot);

    char b[24], c[24];

    if (s_page % PAGES_RELAY == 1) {
        header("CLIENTS");
        if (clients <= 0) {
            row(0, "", "NO DEVICE");
            foot(2, "");
            return;
        }
        /* 前两行放设备列表，末行留给时间轮播 */
        int n = (clients > 2) ? 2 : clients;
        for (int i = 0; i < n; i++) {
            /* 72 px 宽只放得下 12 个字符：序号 + IP 末段 + MAC 后两位 */
            snprintf(b, sizeof(b), "%d .%s %02X%02X",
                     i + 1, ip_last(cips[i]), macs[i][4], macs[i][5]);
            row(i, "", b);
        }
        foot(2, "");
        return;
    }

    switch (s_relay_rot) {
        case 0:     /* 客户端数量 */
            header("CLIENTS");
            snprintf(b, sizeof(b), "DEV %d", clients);
            row(0, "", b);
            snprintf(b, sizeof(b), "STA %s", ssid[0] ? ssid : "--");
            row(1, "", b);
            snprintf(b, sizeof(b), "NAT %s", napt ? "ON" : "OFF");
            foot(2, b);
            break;

        case 1:     /* 上传 */
            header("UPLOAD");
            fmt_rate(b, sizeof(b), up_bps);
            row(0, "", b);
            fmt_bytes(c, sizeof(c), up_tot);
            snprintf(b, sizeof(b), "SUM %s", c);
            row(1, "", b);
            fmt_bytes(c, sizeof(c), dn_tot);
            snprintf(b, sizeof(b), "DN %s", c);
            foot(2, b);
            break;

        case 2:     /* 下载 */
            header("DNLOAD");
            fmt_rate(b, sizeof(b), dn_bps);
            row(0, "", b);
            fmt_bytes(c, sizeof(c), dn_tot);
            snprintf(b, sizeof(b), "SUM %s", c);
            row(1, "", b);
            snprintf(b, sizeof(b), "CLI %d", clients);
            foot(2, b);
            break;

        default:    /* 数据量 */
            header("DATA");
            fmt_bytes(c, sizeof(c), up_tot);
            snprintf(b, sizeof(b), "UP %s", c);
            row(0, "", b);
            fmt_bytes(c, sizeof(c), dn_tot);
            snprintf(b, sizeof(b), "DN %s", c);
            row(1, "", b);
            fmt_bytes(c, sizeof(c), up_tot + dn_tot);
            snprintf(b, sizeof(b), "ALL %s", c);
            foot(2, b);
            break;
    }
}

'''

src = src[:start] + new + src[end:]
io.open(P, "w", encoding="utf-8").write(src)
print("draw_* replaced")
