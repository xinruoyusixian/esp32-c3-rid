/**
 * crid_screen.c — 板载 OLED 屏幕编排 + BOOT 按键交互
 *
 * 屏幕只有 72x40，5x7 字体下每行 12 字符、共 4 行（标题栏 + 3 行数据）。
 *
 * 统一导航模型（遵循 UI 重构任务书）：
 *   UI_HOME        主页：RID / SHOT / RELAY 三功能启动器
 *                   单击=循环选择  双击=进入  长按=无操作  超长按=熄屏/亮屏
 *   UI_FUNC        当前模式的功能根页
 *                    RID  -> 设备列表（单击移动选择 / 双击进详情 / 长按回主页）
 *                    SHOT -> 拍照页（见下方手势）
 *                    RELAY-> 中继页（单击换页 / 双击无操作 / 长按回主页）
 *   UI_DETAIL      RID 设备详情（标题=尾号+RSSI，三行 SPD/ALT/HDG）
 *                    单击=下一个设备  长按/双击=回列表
 *   UI_SET_GAP     拍照间隔设置（单击循环档位 / 双击确认 / 长按取消）
 *   UI_CONFIRM     切换模式确认（会重启；双击=GO，长按=取消）
 *
 * 键盘模式（UI_FUNC + SHOT）：
 *   手动档  单击=拍照  双击=进间隔设置  长按=回主页
 *   定时档  单击=暂停/恢复  双击=进间隔设置  长按=停止并回主页
 *
 * 拍照定时与屏幕刷新解耦：节拍在 shutter_task 内（s_next_at），熄屏/翻页不影响出片。
 *
 * 按键手势统一由 crid_button 识别（SINGLE/DOUBLE/TRIPLE/LONG/VERYLONG），
 * 本文件只处理识别好的事件。超长按(2.5s)只切屏幕亮灭，不与长按互斥冲突。
 *
 * 熄屏：超长按关屏；关屏后首次任意按键只唤醒并消费该次操作（不会误触拍照/翻页）。
 * 无屏板：crid_oled_init 探测不到 SSD1306 即不建按键/刷新任务，其余业务照常。
 */

#include "crid_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "crid_oled.h"
#include "crid_gfx.h"
#include "crid_button.h"
#include "crid_mode.h"
#include "crid_tracker.h"
#include "crid_rx_types.h"
#include "crid_shutter.h"
#include "crid_relay.h"
#include "crid_clock.h"
#include "crid_ext.h"
#include "crid_wifi.h"
#include "crid_web.h"
#include "crid_nvs.h"

static const char *TAG = "SCR";

/* 布局常量（72x40：顶栏 9px + 分隔线 1px + 内容 3 行 x 9px）
 * 顶栏改成"标题 + 右侧信息 + 下方分隔线"，不再整条反显，屏幕观感更轻 */
#define HDR_H      9
#define SEP_Y      9       /* 顶栏下方的分隔线 */
#define ROW0_Y     12
#define ROW_H      9
#define COLS       12      /* 72 / 6 */
#define SB_W       1       /* 右侧滚动条宽 */
#define ALT_MS     2200    /* 底部行"模式信息 / 时间"轮播间隔 */

/* 各模式页数：relay 用 s_page 取模自动轮播；其它模式只有 1 页（靠选择索引） */
#define PAGES_RELAY   3   /* 0=连接 1=累计流量 2=实时速率 */

/* 目标轮播最多记几架（屏幕一次显示一架，多架时轮流/选择展示） */
#define TGT_MAX       4

/* ---------------- 屏幕状态（与业务状态、按键状态严格分离） ---------------- */
static int   s_on     = true;
static bool  s_inited = false;
static bool  s_oled_ok = false;  /* 本板是否真的带屏幕 */

/* 导航状态 */
typedef enum {
    UI_HOME = 0,       /* 一级：主页三功能启动器 */
    UI_FUNC,           /* 二级：当前模式功能页（RID列表 / SHOT菜单 / 中继轮播） */
    UI_DETAIL,         /* 三级(RID)：设备详情（实时刷新） */
    UI_SET_GAP,        /* 三级(SHOT)：间隔档位列表 */
    UI_SHOOT,          /* 三级(SHOT)：START 拍摄态（手动单击即拍/自动双击倒计时） */
    UI_CONFIRM,        /* 切换模式确认（会重启） */
} ui_state_t;

static ui_state_t s_ui = UI_HOME;
static int   s_home_sel   = 0;   /* HOME 选中功能 0=RID 1=SHOT 2=RELAY */
static int   s_list_sel   = 0;   /* RID 列表选中下标 */
static int   s_detail_idx = 0;   /* RID 详情当前设备下标 */
static int   s_gap_idx    = 1;   /* 间隔设置选中档位下标（默认 3s） */
static int   s_shot_sel   = 0;   /* SHOT 二级选中项 0=调整GAP 1=START */
static int   s_page       = 0;   /* relay 子页 / 通用页下标 */

static crid_mode_t s_go = CRID_MODE_RID;   /* 确认页要切到的模式 */

#define MENU_TIMEOUT_MS 30000   /* 子页/确认页空闲自动退回主页 */
#define TOAST_MS        1400    /* 动作反馈停留时间 */
static int32_t s_menu_ttl = 0;
static char    s_toast[16];
static int32_t s_toast_ttl = 0;

/* 菜单绘制函数定义在 screen_task 之后，先声明 */
static void draw_home(void);
static void draw_rid_list(void);
static void draw_rid_detail(void);
static void draw_shot_menu(void);
static void draw_set_gap(void);
static void draw_confirm(void);
static void draw_toast(void);
static bool is_ascii_ssid(const char *s);
static void go_home(void);

/* ---------------- 小工具 ---------------- */

/* 把字符串裁剪到固定宽度并在末尾补空格，保证覆盖上一帧残留 */
static void fixed(char *out, size_t n, const char *s)
{
    if (n == 0) return;
    size_t len = s ? strlen(s) : 0;
    if (len >= n) len = n - 1;
    if (s) memcpy(out, s, len);
    memset(out + len, ' ', n - 1 - len);
    out[n - 1] = 0;
}

/* 电量的真实来源要接 ADC，板子现在是 USB 直供，先按满格画 */
static int bat_level(void) { return 3; }

/**
 * 顶栏：左侧标题 + 右侧信息 + 下方分隔线
 * right == NULL 时右侧画电池图标，"" 表示右侧留空，其它按字符串右对齐
 */
static void header_ex(const char *title, const char *right)
{
    crid_oled_fill(0, 0, CRID_OLED_W, SEP_Y, 0);      /* 清顶栏 */
    crid_oled_text(1, 1, title, 0);                   /* 5x7，顶栏 9px 刚好 */

    if (right && right[0]) {
        int w = (int)strlen(right) * 6;               /* 5x7 glyph + 1px 间距 */
        crid_oled_text(CRID_OLED_W - w - 1, 1, right, 0);
    } else if (!right) {
        gfx_battery(CRID_OLED_W - GFX_BATTERY_W - 1, 1, bat_level(), 0);
    }
    crid_oled_hline(0, SEP_Y, CRID_OLED_W, 1);        /* 分隔线 */
}

/* 顶栏右侧画信号强度（RID 页用），level 0..4 */
static void header_sig(const char *title, int level)
{
    crid_oled_fill(0, 0, CRID_OLED_W, SEP_Y, 0);
    crid_oled_text(1, 1, title, 0);
    gfx_signal(CRID_OLED_W - GFX_SIGNAL_W - 1, 1, level, 0);
    crid_oled_hline(0, SEP_Y, CRID_OLED_W, 1);
}

static void header(const char *title) { header_ex(title, ""); }

static void row(int i, const char *label, const char *val)
{
    char line[24];
    char cell[16];
    snprintf(line, sizeof(line), "%s%s", label, val);
    fixed(cell, sizeof(cell), line);
    /* 只画前 COLS 个字符 */
    char shown[COLS + 1];
    memcpy(shown, cell, COLS);
    shown[COLS] = 0;
    crid_oled_text(0, ROW0_Y + i * ROW_H, shown, 0);
}

/* SSID 是否可安全以 ASCII 显示（中文/乱码用 HIDDEN 代替） */
static bool is_ascii_ssid(const char *s)
{
    if (!s) return false;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

/* ---------------- 小工具（数值格式化 / 大号数字） ---------------- */

/* 速率 / 数据量的格式化输出固定写进 12 字节缓冲（屏幕一行只有 12 字符，
 * 固定上界还能让编译器算出输出长度上限，避免 -Werror=format-truncation） */
#define FMTB 12

static void fmt_rate(char *out, uint32_t bps)
{
    if (bps < 1024) {
        snprintf(out, FMTB, "%lu B/s", (unsigned long)bps);
    } else if (bps < 1024 * 1024) {
        snprintf(out, FMTB, "%.1fK/s", (double)bps / 1024.0);
    } else {
        snprintf(out, FMTB, "%.1fM/s", (double)bps / 1048576.0);
    }
}

static void fmt_bytes(char *out, uint64_t b)
{
    if (b < 1024) {
        snprintf(out, FMTB, "%luB", (unsigned long)b);
    } else if (b < 1024ULL * 1024) {
        snprintf(out, FMTB, "%.0fK", (double)b / 1024.0);
    } else if (b < 1024ULL * 1024 * 1024) {
        snprintf(out, FMTB, "%.1fM", (double)b / 1048576.0);
    } else {
        snprintf(out, FMTB, "%.2fG", (double)b / 1073741824.0);
    }
}

/* 3x5 数字字模（每行 3 bit，高位在左），用来把倒计时画大 */
static const uint8_t DIGIT3x5[10][5] = {
    {0x7,0x5,0x5,0x5,0x7},  /* 0 */
    {0x2,0x6,0x2,0x2,0x7},  /* 1 */
    {0x7,0x1,0x7,0x4,0x7},  /* 2 */
    {0x7,0x1,0x7,0x1,0x7},  /* 3 */
    {0x5,0x5,0x7,0x1,0x1},  /* 4 */
    {0x7,0x4,0x7,0x1,0x7},  /* 5 */
    {0x7,0x4,0x7,0x5,0x7},  /* 6 */
    {0x7,0x1,0x1,0x1,0x1},  /* 7 */
    {0x7,0x5,0x7,0x5,0x7},  /* 8 */
    {0x7,0x5,0x7,0x1,0x7},  /* 9 */
};

/* 大号数字：scale=3 时单个数字 9x15 px，倒计时一眼能看到 */
static int big_digit(int x, int y, int d, int scale)
{
    if (d < 0 || d > 9) d = 0;
    const uint8_t *g = DIGIT3x5[d];
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 3; c++) {
            if (g[r] & (1 << (2 - c))) {
                crid_oled_fill(x + c * scale, y + r * scale, scale, scale, 1);
            }
        }
    }
    return 3 * scale;   /* 本数字占的宽度 */
}

/* 右对齐画整数（最多 2 位），返回总宽 */
static int big_num(int x, int y, int v, int scale)
{
    char s[8];
    snprintf(s, sizeof(s), "%d", v);
    int w = 0;
    for (size_t i = 0; s[i]; i++) {
        w += big_digit(x + w, y, s[i] - '0', scale) + scale;  /* 数字间留 1 个 scale 的缝 */
    }
    return w;
}

/* 按字符串逐位画大号数字（保留前导 0，配对码必须 6 位齐全） */
static int big_digits(int x, int y, const char *s, int scale)
{
    int w = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            w += big_digit(x + w, y, s[i] - '0', scale) + scale;
        } else {
            w += scale * 2;   /* 非数字留个空位 */
        }
    }
    return w;
}

/* ---------------- 数据采集 ---------------- */

/* 屏幕一次只显示一架，多架时把关键字段抽出来轮播
 * （uav_track_t 很大，不能整份拷进任务栈，这里只取要显示的几个值） */
typedef struct {
    char  id[13];     /* 识别码：uas_id 或 MAC 后三位 */
    float spd;        /* 水平速度 m/s */
    float alt;        /* 相对高度 m（location.height） */
    float alt_real;   /* 真实高度 m（location.altitude_geo 大地高） */
    float hdg;        /* 航向 度 */
    int   rssi;       /* dBm（0 表示未知） */
} tgt_t;

typedef struct {
    int   total;      /* 表中有效条目 */
    int   online;     /* 活跃条目 */
    int   n;          /* 抽出来的目标数 */
    tgt_t t[TGT_MAX];
} uav_stat_t;

static void collect_uav(uav_stat_t *st)
{
    memset(st, 0, sizeof(*st));

    SemaphoreHandle_t m = crid_tracker_get_mutex();
    if (!m) return;
    if (xSemaphoreTake(m, pdMS_TO_TICKS(50)) != pdTRUE) return;

    uav_track_t *tab = crid_tracker_get_table();
    int cap = crid_tracker_get_capacity();
    if (!tab || cap <= 0) { xSemaphoreGive(m); return; }
    if (cap > 32) cap = 32;          /* 位图只有 32 位，够用了 */

    uint32_t used = 0;               /* 已挑走的槽位 */

    /* 两轮：先挑活跃的，没挑满再补"最近见过但已超时"的 */
    for (int pass = 0; pass < 2 && st->n < TGT_MAX; pass++) {
        while (st->n < TGT_MAX) {
            int bi = -1;
            uint32_t bseen = 0;
            for (int i = 0; i < cap; i++) {
                if (used & (1u << i)) continue;
                uav_track_t *u = &tab[i];
                /* 跳过空白槽位 */
                if (!(u->mac[0] | u->mac[1] | u->mac[2] |
                      u->mac[3] | u->mac[4] | u->mac[5])) continue;
                if (pass == 0) {
                    if (!u->active) continue;
                } else {
                    if (u->active) continue;   /* 活跃的上一轮已挑完 */
                }
                if (u->last_seen_ms <= bseen) continue;
                bseen = u->last_seen_ms;
                bi = i;
            }
            if (bi < 0) break;
            used |= (1u << bi);

            uav_track_t *u = &tab[bi];
            tgt_t *o = &st->t[st->n++];
            const char *uas = u->basic_id.valid ? u->basic_id.uas_id : "";
            if (uas[0]) snprintf(o->id, sizeof(o->id), "%.12s", uas);
            else        snprintf(o->id, sizeof(o->id), "%02X%02X%02X",
                                 u->mac[3], u->mac[4], u->mac[5]);
            o->spd  = u->location.speed_horizontal;
            o->alt  = u->location.height;
            o->alt_real = u->location.altitude_geo;   /* 真实高度（大地高/海拔） */
            o->hdg  = u->location.direction;
            o->rssi = (u->last_rssi == 0) ? 0 : (int)u->last_rssi;
        }
    }

    /* 统计总数（只读，不改表） */
    for (int i = 0; i < cap; i++) {
        uav_track_t *u = &tab[i];
        if (!(u->mac[0] | u->mac[1] | u->mac[2] |
              u->mac[3] | u->mac[4] | u->mac[5])) continue;
        st->total++;
        if (u->active) st->online++;
    }
    xSemaphoreGive(m);
}

/* RID 当前发现的设备数（用于列表选择越界保护） */
static int rid_total(void)
{
    uav_stat_t st;
    collect_uav(&st);
    return st.total;
}

/* ---------------- 详情/列表辅助 ---------------- */

/* 把选择下标夹到 [0, n-1]，设备消失后尽量保留原选择 */
static int clamp_idx(int idx, int n)
{
    if (n <= 0) return 0;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return idx;
}

/* ---------------- 各模式页面 ---------------- */

/* 主页：左上时间 + 右上电池，下面 RID / SHOT / RELAY 三功能启动器
 * 选中项 `>` 光标 + 反显条；当前生效模式在名字后带 * */
static void draw_home(void)
{
    char t[20];
    crid_clock_now_hm(t, sizeof(t));
    /* now_hm 返回 "MM-DD HH:MM"，顶栏只放得下 5 个字符，取末 5 位＝HH:MM；
       时钟还没同步时它返回 "--"，这时顶栏显示 MENU 而不是两个横杠 */
    size_t l = strlen(t);
    const char *show = (l > 5) ? (t + l - 5) : t;
    if (strcmp(show, "--") == 0) show = "MENU";
    header_ex(show, NULL);
    static const char *NAME[3] = {"RID", "SHOT", "RELAY"};
    int cur = (int)crid_mode_get();
    for (int i = 0; i < 3; i++) {
        char line[16];
        snprintf(line, sizeof(line), "%s%s%s",
                 (i == s_home_sel) ? "> " : "  ", NAME[i],
                 (i == cur) ? "*" : "");
        char shown[COLS + 1];
        fixed(shown, sizeof(shown), line);
        int y = ROW0_Y + i * ROW_H;
        if (i == s_home_sel) {
            crid_oled_fill(0, y - 1, CRID_OLED_W, ROW_H, 1);
            crid_oled_text(0, y, shown, 1);
        } else {
            crid_oled_text(0, y, shown, 0);
        }
    }
}

/* 档位名：MANUAL -> MAN，其余 "10"（"MANUAL" 太长，带上前缀会顶出 12 字符） */
static void gap_name(char *out, size_t n, int idx)
{
    if (idx < 0 || idx >= SHUTTER_GAP_N) idx = 0;
    if (crid_shutter_gaps[idx] == SHUTTER_GAP_MANUAL)
        snprintf(out, n, "MAN");
    else
        snprintf(out, n, "%lu", (unsigned long)crid_shutter_gaps[idx]);
}

/* 当前生效档位对应的下标（找不到就当手动档） */
static int gap_idx_of(uint32_t g)
{
    for (int i = 0; i < SHUTTER_GAP_N; i++)
        if (crid_shutter_gaps[i] == g) return i;
    return 0;
}

/* RSSI -> 信号格数（0..4），-60 满格、-90 以下 1 格 */
static int rssi_level(int rssi)
{
    if (rssi == 0)  return 1;
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

/* 空态 / 异常态：左侧图标 + 右侧两行提示，占满整屏（无顶栏） */
static void draw_notice(int kind, const char *l1, const char *l2)
{
    crid_oled_clear();
    int iy = (CRID_OLED_HEIGHT - GFX_WARN_H) / 2;
    if (kind == 0)      gfx_warn(2, iy, 0);          /* 无设备 */
    else if (kind == 1) gfx_camera_slash(1, iy - 1, 0);  /* 未连接相机 */
    else                gfx_wifi(3, iy, 0);          /* 中继异常 */

    int tx = (kind == 1) ? 19 : 17;
    crid_oled_text(tx, 13, l1, 0);
    if (l2 && l2[0]) crid_oled_text(tx, 24, l2, 0);
}

/* RID 设备列表：顶栏 RID i/n + 信号格；每行 >ID -RSSI，选中反显
 * 3 行窗口滚动，右侧竖条指示当前位置；单击移动/双击进详情/长按回主页 */
static void draw_rid_list(void)
{
    uav_stat_t st;
    collect_uav(&st);

    if (st.total <= 0) {
        draw_notice(0, "NO DEVICE", "SCANNING");
        return;
    }
    s_list_sel = clamp_idx(s_list_sel, st.total);
    /* 3 行窗口，以选中项为中心 */
    int start = 0;
    if (st.total > 3) {
        start = s_list_sel - 1;
        if (start < 0) start = 0;
        if (start > st.total - 3) start = st.total - 3;
    }

    const tgt_t *sel = (s_list_sel < st.n) ? &st.t[s_list_sel] : NULL;
    char hd[20];
    snprintf(hd, sizeof(hd), "RID %d/%d",
             (s_list_sel + 1) & 0xFF, st.total & 0xFF);
    header_sig(hd, sel ? rssi_level(sel->rssi) : 1);

    for (int r = 0; r < 3; r++) {
        int di = start + r;
        if (di >= st.total) break;
        int ti = -1;
        for (int k = 0; k < st.n; k++) if (k == di) { ti = k; break; }
        char b[32];
        if (ti >= 0) {
            const tgt_t *t = &st.t[ti];
            char id4[5];
            memcpy(id4, t->id, 4); id4[4] = 0;
            char rs[6];
            if (t->rssi) snprintf(rs, sizeof(rs), "%d", t->rssi);
            else         snprintf(rs, sizeof(rs), "--");
            snprintf(b, sizeof(b), "%s%s %s", (di == s_list_sel) ? ">" : " ", id4, rs);
        } else {
            snprintf(b, sizeof(b), "%s.... --", (di == s_list_sel) ? ">" : " ");
        }
        char shown[COLS + 1];
        fixed(shown, sizeof(shown), b);
        int y = ROW0_Y + r * ROW_H;
        if (di == s_list_sel) {
            crid_oled_fill(0, y - 1, CRID_OLED_W - 2, ROW_H, 1);
            crid_oled_text(0, y, shown, 1);
        } else {
            crid_oled_text(0, y, shown, 0);
        }
    }
    gfx_scrollbar(CRID_OLED_W - 1, ROW0_Y - 1, 3 * ROW_H, s_list_sel, 3, st.total);
}

/* RID 设备详情：标题=尾号 + 右侧 RSSI；三行 SPD/ALT/HDG */
static void draw_rid_detail(void)
{
    uav_stat_t st;
    collect_uav(&st);

    if (st.n <= 0) {
        draw_notice(0, "NO DEVICE", "SCANNING");
        return;
    }
    s_detail_idx = clamp_idx(s_detail_idx, st.n);
    const tgt_t *t = &st.t[s_detail_idx];

    /* 顶栏：左识别码（4 位，屏幕放不下完整 SN）+ 右 RSSI */
    char id5[5];
    memcpy(id5, t->id, 4); id5[4] = 0;   /* 顶栏右侧要给 RSSI 留 7 字符，ID 只能 4 位 */
    char rs[12];
    if (t->rssi) snprintf(rs, sizeof(rs), "RSSI%d", t->rssi);
    else         snprintf(rs, sizeof(rs), "RSSI--");
    header_ex(id5, rs);

    char b[24];
    snprintf(b, sizeof(b), "SPD %.1fM/S", (double)t->spd);
    row(0, "", b);
    if (t->alt_real != 0.f)
        snprintf(b, sizeof(b), "ALT %.0fM", (double)t->alt_real);
    else
        snprintf(b, sizeof(b), "ALT %.0fM", (double)t->alt);
    row(1, "", b);
    snprintf(b, sizeof(b), "HDG %.0f", (double)t->hdg);
    row(2, "", b);
}

/* BLE 配对中：配对码是此刻唯一重要的事，全屏放大显示 */
static void draw_pairing(void)
{
    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);

    char code[8];
    snprintf(code, sizeof(code), "%06lu", (unsigned long)(st.passkey % 1000000UL));

    header("PAIR");
    big_digits(0, ROW0_Y, code, 3);
    row(2, "", "KEY IN PHONE");
}

/* 底部连接/张数行：LINKED #12（未连接时 NO LINK） */
static void shot_foot(const shutter_status_t *st)
{
    char b[20];
    if (st->connected) {
        if (st->shots <= 99999UL) snprintf(b, sizeof(b), "LINKED #%lu", (unsigned long)st->shots);
        else snprintf(b, sizeof(b), "LINK %luk", (unsigned long)(st->shots / 1000UL));
    } else {
        snprintf(b, sizeof(b), "NO LINK");
    }
    int w = (int)strlen(b) * 6;
    crid_oled_text((CRID_OLED_W - w) / 2, ROW0_Y + 2 * ROW_H, b, 0);
}

/* 拍摄态（三级 START）：
 *   自动档 = 顶栏 SHOT AUTO 10 + 中部大号倒计时 + 底部 LINKED #N
 *   手动档 = 顶栏 SHOT MANUAL + 中部相机图标（未连接则带禁用斜杠） */
static void draw_camera(void)
{
    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);

    char right[16];
    if (st.continuous && st.gap > 0)
        snprintf(right, sizeof(right), "AUTO %lu", (unsigned long)st.gap);
    else if (st.gap == SHUTTER_GAP_MANUAL)
        snprintf(right, sizeof(right), "MANUAL");
    else
        snprintf(right, sizeof(right), "%lus", (unsigned long)st.gap);
    header_ex("SHOT", right);

    int mid_y = 13;
    if (st.continuous && st.gap > 0) {
        if (st.paused) {
            crid_oled_text((CRID_OLED_W - 6 * 6) / 2, mid_y + 4, "PAUSED", 0);
        } else {
            int secs = (int)st.next_in;
            if (secs < 0) secs = 0;
            if (secs > 99) secs = 99;
            /* scale=3：单字 9x15；先按位数算出总宽再居中，避免画两次留残影 */
            char s[8];
            snprintf(s, sizeof(s), "%d", secs);
            int n = (int)strlen(s);
            int x = (CRID_OLED_W - (n * 9 + (n - 1) * 3)) / 2;
            big_num(x, mid_y, secs, 3);
        }
    } else {
        int cx = (CRID_OLED_W - GFX_CAMERA_W) / 2;
        if (st.connected) gfx_camera(cx, mid_y, 0);
        else              gfx_camera_slash(cx, mid_y, 0);
    }
    shot_foot(&st);
}

/* SHOT 二级：连接状态（不可选）+ [调整GAP, START] 可选列表
 * 单击循环选中 / 双击进入对应三级 / 长按回一级主菜单 */
static void draw_shot_menu(void)
{
    shutter_status_t st;
    memset(&st, 0, sizeof(st));
    crid_shutter_get_status(&st);

    char right[16];
    if (st.gap == SHUTTER_GAP_MANUAL) snprintf(right, sizeof(right), "MANUAL");
    else snprintf(right, sizeof(right), "%lus", (unsigned long)st.gap);
    header_ex("SHOT", right);

    /* 列表里的档位统一用短名（MAN），"MANUAL" 加前缀会顶到第 13 个字符被裁 */
    char gap[16];
    gap_name(gap, sizeof(gap), gap_idx_of(st.gap));

    /* 第 0/1 行：可选列表，> 为光标，反显当前选中项 */
    for (int i = 0; i < 2; i++) {
        char b[24];
        if (i == 0) {
            snprintf(b, sizeof(b), "%s GAP %s",
                     (i == s_shot_sel) ? "> " : "  ", gap);
        } else {
            if (st.continuous && !st.paused)
                snprintf(b, sizeof(b), "%s START RUN", (i == s_shot_sel) ? "> " : "  ");
            else
                snprintf(b, sizeof(b), "%s START", (i == s_shot_sel) ? "> " : "  ");
        }
        char shown[COLS + 1];
        fixed(shown, sizeof(shown), b);
        int y = ROW0_Y + i * ROW_H;
        if (i == s_shot_sel) {
            crid_oled_fill(0, y - 1, CRID_OLED_W, ROW_H, 1);
            crid_oled_text(0, y, shown, 1);
        } else {
            crid_oled_text(0, y, shown, 0);
        }
    }
    /* 第 2 行：连接状态 + 已拍张数 */
    if (crid_shutter_is_pairing()) row(2, "", "PAIRING...");
    else                           shot_foot(&st);
}

/* 中继模式：3 页自动轮播，单击可手动换页（不改业务；长按回主页） */
static void draw_relay(void)
{
    /* 直接读结构化状态，避免在本任务栈上做 JSON 生成/解析（栈只有 4 KB） */
    const char *ssid = "";
    const char *ip   = "";
    const char *gw   = "";
    (void)crid_relay_sta_info(&ssid, &ip, &gw);

    uint8_t macs[3][6];
    char    cips[3][16];
    memset(macs, 0, sizeof(macs));
    memset(cips, 0, sizeof(cips));
    int clients = crid_relay_client_list(macs, cips, 3);

    uint32_t up_bps = 0, dn_bps = 0;
    uint64_t up_tot = 0, dn_tot = 0;
    crid_relay_traffic(&up_bps, &dn_bps, &up_tot, &dn_tot);

    char b[24], c[FMTB];
    int page = s_page % PAGES_RELAY;
    char hd[16];
    snprintf(hd, sizeof(hd), "RELAY %d/%d", page + 1, PAGES_RELAY);

    /* STA 都没连上，中继等于没工作：直接给异常页 */
    if (!ssid[0]) {
        draw_notice(2, "RELAY OFF", "CHECK WIFI");
        return;
    }

    switch (page) {
        case 0:     /* 连接信息：SSID + 客户端数 + 上行速率 */
            header_ex(hd, "");
            snprintf(b, sizeof(b), "SSID %s",
                     is_ascii_ssid(ssid) ? ssid : "HIDDEN");
            row(0, "", b);
            snprintf(b, sizeof(b), "CLIENTS %d", clients);
            row(1, "", b);
            fmt_rate(c, up_bps);
            snprintf(b, sizeof(b), "UP %s", c);
            row(2, "", b);
            break;

        case 1:     /* 累计流量：上行 / 下行 */
            snprintf(hd, sizeof(hd), "RELAY TOTAL");
            header_ex(hd, "");
            fmt_bytes(c, up_tot);
            snprintf(b, sizeof(b), "UP %s", c);
            row(0, "", b);
            fmt_bytes(c, dn_tot);
            snprintf(b, sizeof(b), "DN %s", c);
            row(1, "", b);
            break;

        default:    /* 实时速率：上行 / 下行 */
            snprintf(hd, sizeof(hd), "RELAY SPEED");
            header_ex(hd, "");
            fmt_rate(c, up_bps);
            snprintf(b, sizeof(b), "UP %s", c);
            row(0, "", b);
            fmt_rate(c, dn_bps);
            snprintf(b, sizeof(b), "DN %s", c);
            row(1, "", b);
            break;
    }
}

/* 档位网格一行（最多 4 格 x 18px = 72px），已生效档位反显 */
static void gap_grid(int y, const int *idx, int cnt)
{
    uint32_t cur = crid_shutter_get_gap();
    for (int i = 0; i < cnt; i++) {
        int gi = idx[i];
        if (gi < 0 || gi >= SHUTTER_GAP_N) continue;
        char nm[6];
        gap_name(nm, sizeof(nm), gi);
        int x = i * 18;
        if (crid_shutter_gaps[gi] == (uint32_t)cur) {
            crid_oled_fill(x, y - 1, 18, ROW_H, 1);
            crid_oled_text(x + (18 - (int)strlen(nm) * 6) / 2, y, nm, 1);
        } else {
            crid_oled_text(x + (18 - (int)strlen(nm) * 6) / 2, y, nm, 0);
        }
    }
}

/* 间隔设置：顶栏 INTERVAL，中部 `< 10S >` 反显（尖括号表示单击可加减），
 * 下面两行 4x2 档位网格，已生效档位反显 */
static void draw_set_gap(void)
{
    header_ex("INTERVAL", "");

    char nm[10];
    gap_name(nm, sizeof(nm), s_gap_idx);
    char b[20];
    snprintf(b, sizeof(b), "< %s >", nm);
    int w = (int)strlen(b) * 6;
    int x = (CRID_OLED_W - w) / 2;
    crid_oled_fill(x - 3, ROW0_Y - 1, w + 6, ROW_H, 1);
    crid_oled_text(x, ROW0_Y, b, 1);

    static const int ROW1[4] = {1, 2, 3, 4};   /* 3 5 10 15 */
    static const int ROW2[4] = {5, 6, 7, 0};   /* 20 30 60 MAN */
    gap_grid(ROW0_Y + ROW_H, ROW1, 4);
    gap_grid(ROW0_Y + 2 * ROW_H, ROW2, 4);
}

/* 切换模式确认（会重启） */
static void draw_confirm(void)
{
    header("SWITCH?");
    static const char *NAME[3] = {"RID", "SHOT", "RELAY"};
    char line[20];
    snprintf(line, sizeof(line), "TO %s", NAME[(int)s_go]);
    crid_oled_text(0, ROW0_Y, line, 0);
    crid_oled_text(0, ROW0_Y + ROW_H, "WILL REBOOT", 0);
    crid_oled_text(0, ROW0_Y + 2 * ROW_H, "2X=GO LONG=NO", 0);
}

/* ---------------- 刷新任务 ---------------- */

static void screen_task(void *pv)
{
    (void)pv;
    TickType_t last_alt = xTaskGetTickCount();
    while (1) {
        /* 空闲超时：三级页退回二级；二级页就停在二级（不弹主菜单，
           主菜单只在二级页长按返回时出现） */
        if (s_menu_ttl > 0) {
            s_menu_ttl -= 400;
            if (s_menu_ttl <= 0) {
                if (s_ui == UI_DETAIL || s_ui == UI_SET_GAP ||
                    s_ui == UI_SHOOT  || s_ui == UI_CONFIRM) {
                    s_ui = UI_FUNC;
                    ESP_LOGI(TAG, "idle timeout, back to level-2");
                }
            }
        }
        if (s_toast_ttl > 0) s_toast_ttl -= 400;

        if (crid_oled_ready() && s_on && !crid_oled_test_locked()) {
            crid_oled_lock();
            crid_oled_clear();
            if (crid_mode_get() == CRID_MODE_CAMERA && crid_shutter_is_pairing()) {
                /* 配对中：压过一切界面，先把 6 位配对码给人看 */
                draw_pairing();
            } else {
                switch (s_ui) {
                    case UI_HOME:    draw_home();       break;
                    case UI_DETAIL:  draw_rid_detail(); break;
                    case UI_SET_GAP: draw_set_gap();    break;
                    case UI_SHOOT:   draw_camera();     break;   /* 三级 START 拍摄态 */
                    case UI_CONFIRM: draw_confirm();    break;
                    case UI_FUNC:
                    default:
                        switch (crid_mode_get()) {
                            case CRID_MODE_CAMERA: draw_shot_menu(); break;
                            case CRID_MODE_RELAY:  draw_relay();     break;
                            case CRID_MODE_RID:
                            default:               draw_rid_list();  break;
                        }
                        break;
                }
            }
            if (s_toast_ttl > 0) draw_toast();
            crid_oled_flush();
            crid_oled_unlock();
        }
        /* 中继三页按固定节拍轮播（单击可手动换页） */
        if (xTaskGetTickCount() - last_alt >= pdMS_TO_TICKS(ALT_MS)) {
            last_alt = xTaskGetTickCount();
            s_page = (s_page + 1) % PAGES_RELAY;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

/* ---------------- 公共小工具 ---------------- */

static void toast(const char *msg)
{
    snprintf(s_toast, sizeof(s_toast), "%s", msg);
    s_toast_ttl = TOAST_MS;
}

static void draw_toast(void)
{
    int w = (int)strlen(s_toast) * 6;   /* 5x7 字模 + 1 px 间隙 */
    int x = (CRID_OLED_W - w) / 2;
    if (x < 0) x = 0;
    int y = ROW0_Y + ROW_H;
    crid_oled_fill(0, y - 2, CRID_OLED_W, ROW_H + 2, 1);
    crid_oled_text(x, y - 1, s_toast, 1);
}

static void switch_mode_to(crid_mode_t target)
{
    ESP_LOGI(TAG, "switch mode %s -> %s",
             crid_mode_name(crid_mode_get()), crid_mode_name(target));
    crid_mode_request(target);
}

/* ---------------- 按键响应 ---------------- */

/* 退回主页：光标默认停在「当前激活模式」，双击即可直接进入本模式功能页。
 * 否则当板子不在 RID 模式时，主页双击会掉进「切换确认页」→ 重启 → 又回主页，
 * 形成进不了功能页的死循环（用户感知为"只有主菜单、进不去子菜单"）。 */
static void go_home(void)
{
    s_ui = UI_HOME;
    s_home_sel = (int)crid_mode_get();
    s_menu_ttl = 0;
}

/* 进入所选功能：当前模式直接进根页；其它模式走确认（会重启） */
static void home_enter(void)
{
    crid_mode_t sel = (crid_mode_t)s_home_sel;   /* 0=RID 1=CAMERA 2=RELAY */
    if (sel == crid_mode_get()) {
        s_ui = UI_FUNC;
        s_menu_ttl = MENU_TIMEOUT_MS;
    } else {
        s_go = sel;
        s_ui = UI_CONFIRM;
        s_menu_ttl = MENU_TIMEOUT_MS;
    }
}

/* 键盘模式：进入/退出间隔设置时同步选中档位下标 */
static void enter_gap_setting(void)
{
    uint32_t g = crid_shutter_get_gap();
    s_gap_idx = 0;
    for (int i = 0; i < SHUTTER_GAP_N; i++) {
        if (crid_shutter_gaps[i] == g) { s_gap_idx = i; break; }
    }
    s_ui = UI_SET_GAP;
    s_menu_ttl = MENU_TIMEOUT_MS;
}

static void on_gesture(crid_btn_event_t ev, void *arg)
{
    (void)arg;

    /* 校准图案期间，任意按键先退出校准，避免误以为按键失灵 */
    if (crid_oled_test_locked()) {
        ESP_LOGI(TAG, "key pressed: leaving calibration pattern");
        crid_oled_test_pattern(0);
        return;
    }

    /* 熄屏时，首次任意按键只唤醒并消费该次操作（任务书 §4.3） */
    if (!s_on) {
        s_on = true;
        crid_oled_display_on(true);
        ESP_LOGI(TAG, "wake on key (consumed)");
        return;
    }

    if (s_ui != UI_HOME) s_menu_ttl = MENU_TIMEOUT_MS;   /* 任何操作都续期 */

    switch (ev) {
        case CRID_BTN_SINGLE:
            if (s_ui == UI_HOME) {
                s_home_sel = (s_home_sel + 1) % 3;
            } else if (s_ui == UI_FUNC) {
                switch (crid_mode_get()) {
                    case CRID_MODE_RID:
                        s_list_sel = (s_list_sel + 1) % (rid_total() > 0 ? rid_total() : 1);
                        break;
                    case CRID_MODE_CAMERA:
                        /* 二级：在 [调整GAP, START] 之间循环 */
                        s_shot_sel = (s_shot_sel + 1) % 2;
                        break;
                    case CRID_MODE_RELAY:
                    default:
                        s_page = (s_page + 1) % PAGES_RELAY;   /* 仅换显示页，不动业务 */
                        break;
                }
            } else if (s_ui == UI_DETAIL) {
                s_detail_idx++;                        /* 三级：循环到下一架 */
            } else if (s_ui == UI_SET_GAP) {
                s_gap_idx = (s_gap_idx + 1) % SHUTTER_GAP_N;
            } else if (s_ui == UI_SHOOT) {
                /* 三级(START)：手动档单击即拍一张；自动档运行中单击暂停/恢复 */
                shutter_status_t st;
                memset(&st, 0, sizeof(st));
                crid_shutter_get_status(&st);
                if (st.gap == SHUTTER_GAP_MANUAL) {
                    toast(crid_shutter_take_photo() ? "SHOT" : "NO LINK");
                    ESP_LOGI(TAG, "single: shutter fired (manual)");
                } else if (st.continuous) {
                    if (st.paused) { crid_shutter_resume(); toast("GO"); }
                    else           { crid_shutter_pause();   toast("PAUSE"); }
                }
            }
            break;

        case CRID_BTN_DOUBLE:
            if (s_ui == UI_HOME) {
                home_enter();
            } else if (s_ui == UI_FUNC) {
                switch (crid_mode_get()) {
                    case CRID_MODE_RID:
                        if (rid_total() > 0) {
                            s_detail_idx = clamp_idx(s_list_sel, rid_total());
                            s_ui = UI_DETAIL;
                        }
                        break;
                    case CRID_MODE_CAMERA:
                        /* 二级确认：GAP -> 三级档位列表；START -> 三级拍摄态 */
                        if (s_shot_sel == 0) enter_gap_setting();
                        else                 s_ui = UI_SHOOT;
                        break;
                    case CRID_MODE_RELAY:
                    default:
                        /* 中继双击无操作（任务书 §8.6） */
                        break;
                }
            } else if (s_ui == UI_DETAIL) {
                s_ui = UI_FUNC;            /* 详情双击回列表 */
            } else if (s_ui == UI_SET_GAP) {
                crid_shutter_set_gap(crid_shutter_gaps[s_gap_idx]);
                crid_shutter_save_config();
                s_ui = UI_FUNC;
                ESP_LOGI(TAG, "gap set -> %lus", (unsigned long)crid_shutter_gaps[s_gap_idx]);
            } else if (s_ui == UI_SHOOT) {
                /* 三级(START) 自动档：双击启动自动倒计时 */
                shutter_status_t st;
                memset(&st, 0, sizeof(st));
                crid_shutter_get_status(&st);
                if (st.gap != SHUTTER_GAP_MANUAL && !st.continuous) {
                    crid_shutter_set_continuous(true);
                    toast("START");
                    ESP_LOGI(TAG, "double: continuous started");
                }
            } else if (s_ui == UI_CONFIRM) {
                toast("REBOOTING");
                switch_mode_to(s_go);
            }
            break;

        case CRID_BTN_TRIPLE:
            /* 逃生键：仅从任意子页/确认页退回主页（不依赖它做常规返回） */
            go_home();
            break;

        case CRID_BTN_LONG:
            if (s_ui == UI_HOME) {
                /* 一级长按无操作 */
            } else if (s_ui == UI_FUNC) {
                /* 二级长按 -> 回一级主菜单（中继业务继续跑） */
                go_home();
            } else if (s_ui == UI_DETAIL || s_ui == UI_SET_GAP) {
                s_ui = UI_FUNC;             /* 三级 -> 回二级 */
            } else if (s_ui == UI_SHOOT) {
                /* 三级(START)长按 -> 停止拍摄并回二级 */
                shutter_status_t st;
                memset(&st, 0, sizeof(st));
                crid_shutter_get_status(&st);
                if (st.continuous || st.paused) {
                    crid_shutter_stop();
                    crid_shutter_save_config();
                    toast("STOP");
                }
                s_ui = UI_FUNC;
            } else if (s_ui == UI_CONFIRM) {
                go_home();                  /* 取消切换 */
            }
            break;

        case CRID_BTN_VERYLONG:
            /* 超长按只切屏幕亮灭，不触发任何其他动作 */
            s_on = !s_on;
            crid_oled_display_on(s_on);
            ESP_LOGI(TAG, "display %s", s_on ? "on" : "off");
            break;

        default:
            break;
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t crid_screen_init(void)
{
    if (s_inited) return ESP_OK;

    /* 让主页光标默认停在「当前激活模式」，双击直接进入该模式功能页，
     * 避免非 RID 模式下双击反复掉进切模式确认页→重启→主页死循环 */
    s_home_sel = (int)crid_mode_get();

    /* 开机（含切模式重启后）直接进该模式的二级功能页，不显示一级主菜单；
       主菜单只在二级页「长按返回」时出现。s_menu_ttl=0 表示二级页不自动回主页。 */
    s_ui = UI_FUNC;
    s_menu_ttl = 0;

    esp_err_t e = crid_oled_init(CRID_SCREEN_SDA_GPIO,
                                 CRID_SCREEN_SCL_GPIO,
                                 CRID_SCREEN_I2C_ADDR);
    if (e != ESP_OK) {
        /* 这块板没焊屏幕（同一份固件要兼容两种硬件）：
           不建按键、不建刷新任务，其余功能完全不受影响。
           没有屏幕不是错误，返回 OK 让上层继续。 */
        ESP_LOGW(TAG, "no OLED on SDA=%d SCL=%d addr=0x%02X (%s) -> "
                 "screen & button disabled",
                 CRID_SCREEN_SDA_GPIO, CRID_SCREEN_SCL_GPIO,
                 CRID_SCREEN_I2C_ADDR, esp_err_to_name(e));
        s_oled_ok = false;
        s_inited = true;
        return ESP_OK;
    }
    s_oled_ok = true;

    /* 开机画面 */
    crid_oled_clear();
    header("RID NODE");
    row(0, "AP ", "rid");          /* 注意留空格，否则拼成 "APrid" */
    row(1, "IP ", ".4.1");         /* 12 字符放不下完整 IP，只显示末两段 */
    row(2, "", "READY");
    crid_oled_flush();

    e = crid_button_init(CRID_SCREEN_BTN_GPIO, on_gesture, NULL);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "button init failed: %s", esp_err_to_name(e));
        /* 屏幕仍可用，仅无按键 */
    }

    /* 栈里跑 snprintf + ESP_LOG + WiFi 查询，3 KB 会踩到栈保护，给 4 KB */
    BaseType_t ok = xTaskCreate(screen_task, "crid_scr", 4096, NULL, 4, NULL);
    if (ok != pdPASS) return ESP_FAIL;

    s_inited = true;
    return ESP_OK;
}

int crid_screen_get_page(void) { return s_page; }
void crid_screen_set_page(int page) { if (page >= 0) s_page = page; }
int crid_screen_get_page_count(void)
{
    switch (crid_mode_get()) {
        case CRID_MODE_RELAY:  return PAGES_RELAY;
        case CRID_MODE_CAMERA:
        case CRID_MODE_RID:
        default:               return 1;
    }
}
int crid_screen_get_ui(void) { return (int)s_ui; }
void crid_screen_set_ui(int ui)
{
    /* Web 调试用：把数字映射到新状态（0 主页 / 1 功能根页 / 2 详情 / 3 间隔设置 / 4 确认） */
    switch (ui) {
        case 1: s_ui = UI_FUNC;   s_menu_ttl = MENU_TIMEOUT_MS; break;
        case 2: s_ui = UI_DETAIL; s_menu_ttl = MENU_TIMEOUT_MS; break;
        case 3: s_ui = UI_SET_GAP; s_menu_ttl = MENU_TIMEOUT_MS; break;
        case 4: s_ui = UI_CONFIRM; s_go = CRID_MODE_RELAY; s_menu_ttl = MENU_TIMEOUT_MS; break;
        case 5: s_ui = UI_SHOOT;   s_menu_ttl = MENU_TIMEOUT_MS; break;
        default: s_ui = UI_HOME; s_menu_ttl = 0; break;
    }
}

bool crid_screen_is_on(void) { return s_on; }

void crid_screen_set_on(bool on)
{
    s_on = on;
    if (crid_oled_ready()) crid_oled_display_on(on);
}
