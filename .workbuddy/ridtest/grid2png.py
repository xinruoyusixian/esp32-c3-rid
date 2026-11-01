#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
grid2png.py — 把 oled dump 的 #/. 网格放大成 PNG，方便肉眼校对排版

用法：
    python .workbuddy/ridtest/grid2png.py ui0.txt ui1.txt ui3.txt -o out.png
需要 Pillow（本机 Python3.15）
"""
import sys
import os
from PIL import Image

SCALE = 6
GAP = 10


def load(path):
    rows = [ln.strip() for ln in open(path, encoding="utf-8") if set(ln.strip()) <= set("#.") and ln.strip()]
    return rows


def main():
    out = "screens.png"
    args = []
    a = sys.argv[1:]
    i = 0
    while i < len(a):
        if a[i] == "-o":
            out = a[i + 1] if i + 1 < len(a) else out
            i += 2
            continue
        args.append(a[i])
        i += 1
    grids = [load(p) for p in args]
    if not grids:
        raise SystemExit("no input")
    w = max(len(r[0]) for r in grids if r)
    h = max(len(r) for r in grids if r)
    tw = len(grids) * w * SCALE + (len(grids) + 1) * GAP
    th = h * SCALE + 2 * GAP
    img = Image.new("RGB", (tw, th), (24, 24, 28))
    for i, g in enumerate(grids):
        ox = GAP + i * (w * SCALE + GAP)
        for y, row in enumerate(g):
            for x, c in enumerate(row):
                if c == "#":
                    for dy in range(SCALE):
                        for dx in range(SCALE):
                            img.putpixel((ox + x * SCALE + dx, GAP + y * SCALE + dy), (235, 240, 245))
    img.save(out)
    print("wrote", os.path.abspath(out))


if __name__ == "__main__":
    main()
