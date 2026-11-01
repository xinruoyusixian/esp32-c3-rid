# -*- coding: utf-8 -*-
"""离线渲染新增/改版页面，确认 72x40 排版不溢出、字段齐全。
字模取自 main/crid_oled.c（font5x7），与固件同源。
"""
import re, os, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "main")

def parse_u8_array(src, name):
    txt = open(os.path.join(ROOT, src), encoding="utf-8").read()
    i = txt.index(name); j = txt.index("{", i)
    depth, k = 0, j
    while k < len(txt):
        if txt[k] == "{": depth += 1
        elif txt[k] == "}":
            depth -= 1
            if depth == 0: break
        k += 1
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", txt[j+1:k])]

font = parse_u8_array("crid_oled.c", "font5x7")
W, H = 72, 40
HDR_H, ROW0_Y, ROW_H, COLS = 9, 12, 8, 12

class Scr:
    def __init__(self): self.b = [[0]*W for _ in range(H)]
    def fill(self, x, y, w, h, on):
        for r in range(y, y+h):
            for c in range(x, x+w):
                if 0 <= c < W and 0 <= r < H: self.b[r][c] = 1 if on else 0
    def text(self, x, y, s, inv=0):
        cx = x
        for ch in s:
            idx = ord(ch) - 0x20
            if 0 <= idx < 95:
                for col in range(5):
                    bits = font[idx*5+col]
                    for row in range(7):
                        v = (bits >> row) & 1
                        if 0 <= cx+col < W and 0 <= y+row < H:
                            self.b[y+row][cx+col] = 1 if (v ^ (1 if inv else 0)) else 0
            cx += 6
        return cx
    def show(self, title):
        print("--- %s ---" % title)
        print("+" + "-"*W + "+")
        for r in self.b:
            print("|" + "".join("#" if v else "." for v in r) + "|")
        print("+" + "-"*W + "+")

def header_ex(s, title, badge=None):
    s.fill(0, 0, W, HDR_H, 1)
    s.text(2, 1, title, inv=1)
    right = badge if badge else "12:34"
    x = W - len(right)*6 - 1
    s.text(x, 1, right, inv=1)

def row(s, i, txt):
    """row(i, label, val) 合并后按 12 字符定宽绘制"""
    s.text(0, ROW0_Y + i*ROW_H, txt[:COLS])

# ---------- 1) RID 三级详情（新增真实高度/相对高度） ----------
s = Scr()
header_ex(s, "UAV", "-90dBm")
row(s, 0, "1A2B3C4D5E6F")            # 识别码
row(s, 1, "ALT 120/45")               # 真实高度/相对高度
row(s, 2, "SPD12 H180")               # 速度 + 航向
s.show("RID 三级详情 (识别码/真实·相对高度/速度·航向)")

# ---------- 2) SHOT 二级菜单 ----------
s = Scr()
header_ex(s, "SHOT", "SHOT")
row(s, 0, "LINKED")                   # 连接状态（不可选）
# 列表：> 为光标；此处演示选中 GAP
s.fill(0, ROW0_Y + 1*ROW_H - 1, W, ROW_H, 1)
s.text(0, ROW0_Y + 1*ROW_H, "> GAP 3s", inv=1)
s.text(0, ROW0_Y + 2*ROW_H, "  START")
s.show("SHOT 二级 (连接状态 + [GAP, START])")

# ---------- 3) SHOT 三级 GAP 档位列表 ----------
gaps = ["MANUAL", "3s", "5s", "10s", "15s", "20s", "30s", "60s"]
s = Scr()
header_ex(s, "GAP", "SHOT")
cur = 1                                # 光标在 "3s"，已生效档位假设为 5s(idx=2)
for r in range(3):
    idx = (cur - 1 + r) % len(gaps)
    line = "%s%s%s" % (">" if idx == cur else " ",
                       "*" if idx == 2 else " ",
                       gaps[idx])
    y = ROW0_Y + r*ROW_H
    if idx == cur:
        s.fill(0, y-1, W, ROW_H, 1); s.text(0, y, line, inv=1)
    else:
        s.text(0, y, line)
s.show("SHOT 三级 GAP 列表 (> 光标, * 已生效)")
