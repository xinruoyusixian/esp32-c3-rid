# -*- coding: utf-8 -*-
"""用固件里同一份字模离线渲染"配对码页面"，确认 72x40 屏放得下且不重叠。

字模直接从 main/crid_oled.c（font5x7）和 main/crid_screen.c（DIGIT3x5）里解析，
保证和固件用的是同一份数据。
"""
import re, sys

ROOT = "main/"


def parse_u8_array(src, name):
    txt = open(ROOT + src, encoding="utf-8").read()
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


font = parse_u8_array("crid_oled.c", "font5x7")          # 95 * 5 列优先
digits = parse_u8_array("crid_screen.c", "DIGIT3x5")      # 10 * 5 行

W, H = 72, 40
buf = [[0] * W for _ in range(H)]


def px(x, y, on):
    if 0 <= x < W and 0 <= y < H:
        buf[y][x] = 1 if on else 0


def fill(x, y, w, h, on):
    for r in range(y, y + h):
        for c in range(x, x + w):
            px(c, r, on)


def text(x, y, s, inverse=0):
    """5x7 列优先字模，字符间距 1 px"""
    cx = x
    for ch in s:
        idx = ord(ch) - 0x20
        if 0 <= idx < 95:
            for col in range(5):
                bits = font[idx * 5 + col]
                for row in range(7):
                    v = (bits >> row) & 1
                    px(cx + col, y + row, 1 if (v ^ (1 if inverse else 0)) else 0)
        cx += 6
    return cx


def big_digit(x, y, d, scale):
    g = digits[d * 5:(d + 1) * 5]
    for r in range(5):
        for c in range(3):
            if g[r] & (1 << (2 - c)):
                fill(x + c * scale, y + r * scale, scale, scale, 1)
    return 3 * scale


# ---- 复刻 draw_pairing() ----
code = sys.argv[1] if len(sys.argv) > 1 else "104523"
title = "PAIR"
fill(0, 0, W, 9, 1)          # header 反显
text(2, 1, title, inverse=1)
hm = "03:07"
text(W - len(hm) * 6 - 1, 1, hm, inverse=1)

w = 0
for c in code:               # big_digits(0, ROW0_Y=12, code, 3)
    w += big_digit(w, 12, int(c), 3) + 3
text(0, 28, "KEY IN PHONE")  # row(2)

print("code width = %d px (screen %d)" % (w, W))
print("+" + "-" * W + "+")
for row in buf:
    print("|" + "".join("#" if v else "." for v in row) + "|")
print("+" + "-" * W + "+")
