#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_icons.py — 把图标 ASCII 画稿烘焙成 C 位图，生成 main/crid_gfx_icons.h

画稿里 '#' = 点亮，'.' = 熄灭。改图标只需改 ICONS 里的画稿再重跑：
    python tools/gen_icons.py
"""
import os

ICONS = {
    # 拍照页中部的大相机（16x11）
    "camera": [
        ".....####.......",
        ".....#..#.......",
        ".##############.",
        ".#............#.",
        ".#...######...#.",
        ".#..#......#..#.",
        ".#..#..##..#..#.",
        ".#..#......#..#.",
        ".#...######...#.",
        ".#............#.",
        ".##############.",
    ],
    # 未连接：由 camera 自动叠加禁用斜杠（见 slash()）
    # 警告三角（13x12）
    "warn": [
        "......#......",
        ".....###.....",
        ".....###.....",
        "....##.##....",
        "....##.##....",
        "...##...##...",
        "...##...##...",
        "..##..#..##..",
        "..##..#..##..",
        "..##.....##..",
        "..##..#..##..",
        ".###########.",
    ],
    # Wi-Fi（9x8）
    "wifi": [
        "...###...",
        "..#...#..",
        ".#.....#.",
        ".........",
        "..#####..",
        ".#.....#.",
        ".........",
        "...###...",
    ],
    # 齿轮（8x8）
    "gear": [
        "..#..#..",
        ".######.",
        "##.##.##",
        "#.#..#.#",
        "#.#..#.#",
        "##.##.##",
        ".######.",
        "..#..#..",
    ],
}

OUT = os.path.join(os.path.dirname(__file__), "..", "main", "crid_gfx_icons.h")


def slash(rows):
    """在画稿上叠一条左上→右下的禁用斜杠"""
    w = max(len(r) for r in rows)
    h = len(rows)
    out = [list(r.ljust(w, ".")) for r in rows]
    for y in range(h):
        x = (w - 1) * y // (h - 1) if h > 1 else 0
        for d in (0, 1):          # 2px 粗，小屏上才看得清
            if x + d < w:
                out[y][x + d] = "#"
    return ["".join(r) for r in out]


def pack(rows):
    w = max(len(r) for r in rows)
    h = len(rows)
    stride = (w + 7) // 8
    data = []
    for r in rows:
        r = r.ljust(w, ".")
        for s in range(stride):
            b = 0
            for c in range(8):
                x = s * 8 + c
                if x < w and r[x] == "#":
                    b |= 0x80 >> c
            data.append(b)
    return w, h, stride, data


def main():
    ICONS["camera_slash"] = slash(ICONS["camera"])
    lines = [
        "/* 自动生成，勿手改：tools/gen_icons.py */",
        "#ifndef CRID_GFX_ICONS_H",
        "#define CRID_GFX_ICONS_H",
        "",
        "#include <stdint.h>",
        "",
        "/* 行优先位图：每行 stride 字节，MSB 对应每字节最左侧像素 */",
        "/* 结构体 gfx_bmp_t 定义在 crid_gfx.c，这里只出数据和尺寸宏 */",
    ]
    for name, rows in ICONS.items():
        w, h, stride, data = pack(rows)
        arr = ", ".join("0x%02X" % b for b in data)
        lines.append("static const uint8_t GFX_%s_DATA[%d] = { %s };"
                     % (name.upper(), len(data), arr))
        lines.append("#define GFX_%s_W %d" % (name.upper(), w))
        lines.append("#define GFX_%s_H %d" % (name.upper(), h))
        lines.append("#define GFX_%s_STRIDE %d" % (name.upper(), stride))
        lines.append("")
        print("-- %s %dx%d" % (name, w, h))
        for r in rows:
            print("   " + r)

    lines.append("/* 统一入口：由 crid_gfx.c 组装 */")
    lines.append("#endif /* CRID_GFX_ICONS_H */")
    path = os.path.abspath(OUT)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", path)


if __name__ == "__main__":
    main()
