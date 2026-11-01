# -*- coding: utf-8 -*-
"""按 UI 状态抓帧：scr menu <n> 强制切页 + oled dump，输出存文件便于查看
用法: python verify_ui.py COM5 1 2 5      # 依次抓 menu 1/2/5
"""
import sys, time, serial, os, re

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
STATES = sys.argv[2:] or ["0", "1", "2"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dumps")
os.makedirs(OUT, exist_ok=True)

s = serial.Serial(PORT, 115200, timeout=0.4, dsrdtr=False, rtscts=False)
try:
    s.setDTR(False); s.setRTS(False)
except Exception:
    pass
time.sleep(1.5)

def send(cmd, wait=1.6):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    deadline = time.time() + wait
    buf = b""
    while time.time() < deadline:
        d = s.read(4096)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")

for st in STATES:
    send("scr menu %s" % st, 1.0)
    raw = send("oled dump", 1.6)
    # 提取 72 列网格行（只含 # 和 . 的行）
    grid = [l for l in raw.split("\n") if re.fullmatch(r"[#.]{20,}", l.strip())]
    fn = os.path.join(OUT, "ui_%s.txt" % st)
    open(fn, "w").write("\n".join(grid))
    print("==== menu %s -> %s (%d rows) ====" % (st, os.path.basename(fn), len(grid)))
    for i, l in enumerate(grid):
        print("%2d|%s" % (i, l))
    print()
s.close()
