/**
 * crid_gfx.c — 72x40 单色屏的小图形库实现
 *
 * 位图数据来自 tools/gen_icons.py（改图标改脚本后重跑即可）。
 * 几何图元（电量/信号/滚动条）直接按数值画，省点阵空间。
 */

#include "crid_gfx.h"
#include "crid_oled.h"
#include "crid_gfx_icons.h"

/* 行优先位图：每行 stride 字节，MSB 是该字节最左侧像素 */
typedef struct {
    uint8_t w, h, stride;
    const uint8_t *bmp;
} gfx_bmp_t;

#define BMP(name) \
    { GFX_##name##_W, GFX_##name##_H, GFX_##name##_STRIDE, GFX_##name##_DATA }

static const gfx_bmp_t B_CAMERA       = BMP(CAMERA);
static const gfx_bmp_t B_CAMERA_SLASH = BMP(CAMERA_SLASH);
static const gfx_bmp_t B_WARN         = BMP(WARN);
static const gfx_bmp_t B_WIFI         = BMP(WIFI);
static const gfx_bmp_t B_GEAR         = BMP(GEAR);

/* 画一个位图；inverse 时先铺亮底再镂空，效果等同于反显 */
static void bmp_draw(int x, int y, const gfx_bmp_t *b, int inverse)
{
    if (inverse) crid_oled_fill(x, y, b->w, b->h, 1);
    for (int r = 0; r < b->h; r++) {
        for (int c = 0; c < b->w; c++) {
            int byte = b->bmp[r * b->stride + (c >> 3)];
            int on = (byte >> (7 - (c & 7))) & 1;
            crid_oled_pixel(x + c, y + r, inverse ? !on : on);
        }
    }
}

void gfx_camera(int x, int y, int inverse)       { bmp_draw(x, y, &B_CAMERA, inverse); }
void gfx_camera_slash(int x, int y, int inverse) { bmp_draw(x, y, &B_CAMERA_SLASH, inverse); }
void gfx_warn(int x, int y, int inverse)         { bmp_draw(x, y, &B_WARN, inverse); }
void gfx_wifi(int x, int y, int inverse)         { bmp_draw(x, y, &B_WIFI, inverse); }
void gfx_gear(int x, int y, int inverse)         { bmp_draw(x, y, &B_GEAR, inverse); }

/* 单点绘制，inverse 时把"点亮"取反（配合先铺亮底＝反显镂空） */
static void px(int x, int y, int on, int inv)
{
    crid_oled_pixel(x, y, inv ? !on : on);
}

/* 信号强度：4 条竖条，高度从左到右递增（最右最高），level 决定点亮几条 */
void gfx_signal(int x, int y, int level, int inverse)
{
    static const uint8_t BX[4]  = {1, 3, 5, 7};
    static const uint8_t TOP[4] = {4, 3, 2, 0};   /* 条顶端行号（共 7 行） */
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    if (inverse) crid_oled_fill(x, y, GFX_SIGNAL_W, GFX_SIGNAL_H, 1);
    for (int i = 0; i < 4; i++) {
        int lit = (i < level);
        for (int r = TOP[i]; r < GFX_SIGNAL_H; r++)
            px(x + BX[i], y + r, lit, inverse);
    }
}

/* 电池：外壳 9x5 + 正极帽，内部 3 格电量 */
void gfx_battery(int x, int y, int level, int inverse)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    if (inverse) crid_oled_fill(x, y, GFX_BATTERY_W, GFX_BATTERY_H, 1);
    /* 外壳描边 */
    for (int c = 1; c <= 8; c++) { px(x + c, y, 1, inverse); px(x + c, y + 6, 1, inverse); }
    for (int r = y; r <= y + 6; r++) { px(x, r, 1, inverse); px(x + 8, r, 1, inverse); }
    /* 正极帽 */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 2; c++) px(x + 9 + c, y + 2 + r, 1, inverse);
    /* 电量格 */
    for (int i = 0; i < level; i++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 2; c++) px(x + 2 + i * 2 + c, y + 2 + r, 1, inverse);
}

/* 右侧滚动条：按可见窗口在总项数中的位置画一段亮条 */
void gfx_scrollbar(int x, int y, int h, int index, int vis, int total)
{
    if (total <= vis) return;
    int thumb = h * vis / total;
    if (thumb < 2) thumb = 2;
    int travel = h - thumb;
    int pos = (total > vis) ? (travel * index / (total - vis)) : 0;
    crid_oled_fill(x, y, 1, h, 0);
    crid_oled_fill(x, y + pos, 1, thumb, 1);
}
