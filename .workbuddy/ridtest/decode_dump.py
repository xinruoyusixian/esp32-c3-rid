# -*- coding: utf-8 -*-
"""把 oled dump 的 72x40 点阵还原成文字，并测量标题栏右侧文字宽度。

用法: python decode_dump.py dumps/ui_1.txt [dumps/ui_2.txt ...]
"""
import re, sys, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "main")

def parse_u8_array(src, name):
    txt = open(os.path.join(ROOT, src), encoding="utf-8").read()
    i = txt.index(name)
    j = txt.index("{", i)
    depth, k = 0, j
    while k < len(txt):
        if txt[k] == "{":
            depth += 1
        elif txt[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    body = txt[j + 1:k]
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", body)]

font = parse_u8_array("crid_oled.c", "font5x7")   # 95 * 5, 列优先

# 预建 字符 -> 5x7 位图（bit=1 表示点亮）
GLYPHS = {}
for i in range(95):
    cols = font[i * 5:(i + 1) * 5]
    rows = []
    for r in range(7):
        v = 0
        for c in range(5):
            v |= ((cols[c] >> r) & 1) << (4 - c)
        rows.append(v)
    GLYPHS[chr(0x20 + i)] = rows

def read_grid(path):
    lines = [l.rstrip("\n") for l in open(path, encoding="utf-8")]
    g = []
    for l in lines:
        s = l.strip()
        if re.fullmatch(r"[#.]{40,}", s):
            g.append([1 if ch == "#" else 0 for ch in s])
    return g

def decode_cell(grid, x, y, inverse):
    """取 (x,y) 起 5x7，返回匹配到的字符"""
    rows = []
    for r in range(7):
        v = 0
        for c in range(5):
            yy, xx = y + r, x + c
            if yy >= len(grid) or xx >= len(grid[yy]):
                continue
            bit = grid[yy][xx]
            if inverse:
                bit = 1 - bit
            v |= bit << (4 - c)
        rows.append(v)
    best, bestd = " ", 99
    for ch, gr in GLYPHS.items():
        d = sum(1 for a, b in zip(rows, gr) if a != b)
        if d < bestd:
            best, bestd = ch, d
    return best if bestd <= 3 else "?"

def decode_line(grid, y, x0, n=12, inverse=False):
    return "".join(decode_cell(grid, x0 + c * 6, y, inverse) for c in range(n))

def header_runs(grid, y=1):
    """标题栏：找"有文字像素（即出现 . 镂空）"的列区间"""
    w = len(grid[y])
    cols = []
    for x in range(w):
        has = any(grid[y + r][x] == 0 for r in range(7) if y + r < len(grid))
        cols.append(has)
    runs, start = [], None
    for x, v in enumerate(cols):
        if v and start is None:
            start = x
        elif not v and start is not None:
            runs.append((start, x - 1)); start = None
    if start is not None:
        runs.append((start, w - 1))
    return runs

for path in sys.argv[1:]:
    g = read_grid(path)
    if not g:
        print("%s: 空" % path); continue
    print("=== %s (%dx%d) ===" % (os.path.basename(path), len(g[0]), len(g)))
    hr = header_runs(g)
    print("标题栏文字列区间:", [(a, b, b - a + 1) for a, b in hr],
          "  -> 右侧宽 %d px ≈ %.1f 字符" % (
              (hr[-1][1] - hr[-1][0] + 1) if hr else 0,
              ((hr[-1][1] - hr[-1][0] + 1) / 6.0) if hr else 0))
    print("标题栏(x=2起): [%s]" % decode_line(g, 1, 2, 12, inverse=True).rstrip())
    for i, y in enumerate((12, 20, 28)):
        print("  行%d(y=%d): [%s]" % (i, y, decode_line(g, y, 0, 12)))
    print()
