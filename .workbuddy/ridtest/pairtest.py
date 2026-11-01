# -*- coding: utf-8 -*-
"""清掉配对 -> 等对端重连触发新的配对流程 -> 抓屏幕确认配对码显示出来了"""
import sys, time, re, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"

s = serial.Serial(PORT, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
try:
    s.setDTR(False); s.setRTS(False)
except Exception:
    pass
time.sleep(1.0)


def cmd(c, wait=1.5):
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    dl = time.time() + wait
    buf = b""
    while time.time() < dl:
        d = s.read(4096)
        if d:
            buf += d
        if len(buf) > 60000:
            break
    return buf.decode("utf-8", "replace")


print("clearbonds ->", cmd("clearbonds", 2.0)[-120:].strip(), flush=True)

for i in range(45):
    out = cmd("pair", 1.0)
    m = re.search(r"PAIRING  code=(\d{6})", out)
    if m:
        print("PAIRING DETECTED code=" + m.group(1), flush=True)
        print(cmd("oled dump", 3.0), flush=True)
        break
    time.sleep(0.8)
else:
    print("NO PAIRING within budget; last:", cmd("pair", 1.0)[-200:], flush=True)
s.close()
