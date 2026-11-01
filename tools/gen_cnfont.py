#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_cnfont.py — 由系统字体烘焙 12x12 汉字点阵，生成 main/crid_cnfont.h

板载 OLED 只有 72x40，5x7 的 ASCII 字模画不了中文，只能按字烘焙点阵。
用法（需要 Pillow，本机 Python3.15 才有）：
    /c/Users/Alan/AppData/Local/Programs/Python/Python315/python.exe tools/gen_cnfont.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "main", "crid_cnfont.h")
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simsun.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
]

# 界面会用到的汉字，按需增删后重跑本脚本即可
CHARS = (
    "间隔设置秒客户端流量统计实时速度未发现设备请稍后重试"
    "连接拍摄中继启动检查网络无法停止暂手动模式屏幕熄"
    "返回确认取消开关搜索架张数已完成待机错误警告"
    "主页列表详情照高低电池信号空空闲未知道蓝牙配"
    "对码请输入手机拍照次数自动间隔手动"
)

SIZE = 12          # 点阵边长
PAD = 3            # 渲染画布留边，避免笔画被切


def render(ch, font):
    """把单个汉字渲染并居中裁剪成 SIZE x SIZE 的 0/1 矩阵"""
    img = Image.new("L", (SIZE + PAD * 2, SIZE + PAD * 2), 0)
    d = ImageDraw.Draw(img)
    d.text((PAD, PAD - 1), ch, font=font, fill=255)
    px = img.load()
    w, h = img.size
    # 阈值化后求有效笔画包围盒
    xs, ys = [], []
    for y in range(h):
        for x in range(w):
            if px[x, y] > 70:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    bw, bh = x1 - x0 + 1, y1 - y0 + 1
    if bw > SIZE or bh > SIZE:
        bw = bh = SIZE
        x0 = max(0, (w - SIZE) // 2)
        y0 = max(0, (h - SIZE) // 2)
    ox = (SIZE - bw) // 2
    oy = (SIZE - bh) // 2
    grid = [[0] * SIZE for _ in range(SIZE)]
    for y in range(bh):
        for x in range(bw):
            if px[x0 + x, y0 + y] > 70:
                grid[oy + y][ox + x] = 1
    return grid


def pack(grid):
    """12 列 -> 每行 2 字节，byte0=列0..7(MSB在前)，byte1 高4位=列8..11"""
    out = []
    for row in grid:
        b0 = 0
        for c in range(8):
            if row[c]:
                b0 |= 0x80 >> c
        b1 = 0
        for c in range(8, 12):
            if row[c]:
                b1 |= 0x80 >> (c - 8)
        out.append((b0, b1))
    return out


def main():
    font_path = next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)
    if not font_path:
        raise SystemExit("未找到中文字体")
    font = ImageFont.truetype(font_path, SIZE + 2)

    seen, glyphs = set(), []
    for ch in CHARS:
        if ch in seen:
            continue
        seen.add(ch)
        g = render(ch, font)
        if g is None:
            print("skip(blank):", ch)
            continue
        glyphs.append((ch, g))

    lines = []
    lines.append("/* 自动生成，勿手改：tools/gen_cnfont.py */")
    lines.append("#ifndef CRID_CNFONT_H")
    lines.append("#define CRID_CNFONT_H")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("/* 12x12 汉字点阵：每行 2 字节（byte0=列0..7，byte1 高4位=列8..11），共 24 字节 */")
    lines.append("#define CRID_CN_W  12")
    lines.append("#define CRID_CN_H  12")
    lines.append("#define CRID_CN_STRIDE 2")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    uint32_t cp;        /* Unicode 码点 */")
    lines.append("    uint8_t  bmp[24];   /* 12 行 x 2 字节 */")
    lines.append("} crid_cn_glyph_t;")
    lines.append("")
    lines.append("static const crid_cn_glyph_t CRID_CN_FONT[] = {")
    for ch, g in glyphs:
        data = pack(g)
        hexs = ", ".join("0x%02X" % b for byte in data for b in byte)
        lines.append("    {0x%06X, {%s}},  /* %s */" % (ord(ch), hexs, ch))
    lines.append("};")
    lines.append("")
    lines.append("#define CRID_CN_GLYPH_N  (sizeof(CRID_CN_FONT)/sizeof(CRID_CN_FONT[0]))")
    lines.append("")
    lines.append("#endif /* CRID_CNFONT_H */")

    out = os.path.abspath(OUT)
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s, %d glyphs" % (out, len(glyphs)))

    # 顺手打印前几个字的点阵，肉眼确认没糊
    for ch, g in glyphs[:6]:
        print("--", ch)
        for row in g:
            print("   " + "".join("#" if v else "." for v in row))


if __name__ == "__main__":
    main()
