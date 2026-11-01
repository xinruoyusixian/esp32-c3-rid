/**
 * crid_gfx.h — 72x40 单色屏的小图形库
 *
 * 屏幕太小，纯文字表达力不够，这里提供两类图元：
 *   1) 位图图标（相机 / 警告 / Wi-Fi / 齿轮…），由 tools/gen_icons.py 烘焙
 *   2) 几何图元（电量、信号强度、滚动条），按运行时数值绘制
 *
 * inverse=1 时按反显画（背景点亮、图形镂空），与顶栏/选中条的风格一致。
 */

#ifndef CRID_GFX_H
#define CRID_GFX_H

#ifdef __cplusplus
extern "C" {
#endif

/** 相机 16x11：拍照页中部主图 */
void gfx_camera(int x, int y, int inverse);
/** 相机 + 禁用斜杠：未连接拍摄设备 */
void gfx_camera_slash(int x, int y, int inverse);
/** 警告三角 13x12 */
void gfx_warn(int x, int y, int inverse);
/** Wi-Fi 9x8 */
void gfx_wifi(int x, int y, int inverse);
/** 齿轮 8x8 */
void gfx_gear(int x, int y, int inverse);

/** 信号强度 8x7：level 0..4（0 只画最左一条，表示极弱） */
void gfx_signal(int x, int y, int level, int inverse);
/** 电池 11x7：level 0..3 */
void gfx_battery(int x, int y, int level, int inverse);

/** 右侧竖直滚动条：total 项里当前显示到 index，可见 vis 项 */
void gfx_scrollbar(int x, int y, int h, int index, int vis, int total);

/** 图标尺寸，供调用方排版 */
#define GFX_CAMERA_W  16
#define GFX_CAMERA_H  11
#define GFX_WARN_W    13
#define GFX_WARN_H    12
#define GFX_WIFI_W    9
#define GFX_WIFI_H    8
#define GFX_GEAR_W    8
#define GFX_GEAR_H    8
#define GFX_SIGNAL_W  8
#define GFX_SIGNAL_H  7
#define GFX_BATTERY_W 11
#define GFX_BATTERY_H 7

#ifdef __cplusplus
}
#endif

#endif /* CRID_GFX_H */
