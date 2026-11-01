#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dump_pages.py — 一次串口会话里抓多个界面的 OLED 点阵（避免反复开关端口复位板子）

用法：
    python .workbuddy/ridtest/dump_pages.py COM5 0 1 3 4
参数是要抓的 UI 编号：0=主菜单 1=二级功能页 2=RID详情 3=间隔设置 4=拍摄 5=切模式确认
"""
import sys
import time
import serial

UI_NAME = {0: "HOME", 1: "FUNC", 2: "DETAIL", 3: "INTERVAL", 4: "SHOOT", 5: "CONFIRM"}


def grab(s, sec):
    buf = []
    t0 = time.time()
    while time.time() - t0 < sec:
        d = s.read(8192)
        if d:
            buf.append(d.decode("utf-8", "replace"))
    return "".join(buf)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    pages = [int(x) for x in sys.argv[2:]] or [0, 1, 3, 4]
    s = serial.Serial(port, 115200, timeout=1, dsrdtr=False, rtscts=False)
    time.sleep(3.0)          # 等开机 banner 吐完
    s.read(65536)
    out = []
    for ui in pages:
        s.reset_input_buffer()
        s.write(("scr menu %d\r\n" % ui).encode())
        time.sleep(1.2)
        s.write(b"oled dump\r\n")
        time.sleep(2.5)
        txt = grab(s, 3.0)
        grid = [ln for ln in txt.splitlines() if set(ln.strip()) <= set("#.") and len(ln.strip()) > 20]
        print("===== UI %d (%s) =====" % (ui, UI_NAME.get(ui, "?")))
        if not grid:
            print("(no grid captured)")
            print(txt[:300])
        else:
            for ln in grid[-40:]:
                print(ln.strip())
        print()
        out.append((ui, grid))
    s.close()

    import os
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dumps")
    os.makedirs(d, exist_ok=True)
    for ui, grid in out:
        if grid:
            with open(os.path.join(d, "ui%d.txt" % ui), "w", encoding="utf-8") as f:
                f.write("\n".join(ln.strip() for ln in grid[-40:]) + "\n")
    print("saved to", d)


if __name__ == "__main__":
    main()
