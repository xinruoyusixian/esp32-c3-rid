# -*- coding: utf-8 -*-
"""切模式 -> 重启 -> 按 UI 状态抓帧并存盘
用法: python verify_mode.py COM5 rid 1 2
      python verify_mode.py COM5 camera 1 3 5
"""
import sys, time, serial, os, re

PORT = sys.argv[1]
MODE = sys.argv[2]
STATES = sys.argv[3:] or ["1"]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dumps")
os.makedirs(OUT, exist_ok=True)

def open_port():
    s = serial.Serial(PORT, 115200, timeout=0.4, dsrdtr=False, rtscts=False)
    try:
        s.setDTR(False); s.setRTS(False)
    except Exception:
        pass
    return s

s = open_port()
time.sleep(1.2)

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

print("---- switch to %s ----" % MODE)
send("mode %s" % MODE, 1.0)
s.close()
time.sleep(9)                 # 等重启 + 开机

s = open_port()
time.sleep(1.8)
for st in STATES:
    send("scr menu %s" % st, 1.0)
    raw = send("oled dump", 1.7)
    grid = [l.strip() for l in raw.split("\n") if re.fullmatch(r"[#.]{40,}", l.strip())]
    fn = os.path.join(OUT, "%s_%s.txt" % (MODE, st))
    open(fn, "w").write("\n".join(grid))
    print("saved %s (%d rows)" % (os.path.basename(fn), len(grid)))
s.close()
print("done")
