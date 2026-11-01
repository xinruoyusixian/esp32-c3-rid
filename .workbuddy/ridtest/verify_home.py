# -*- coding: utf-8 -*-
"""验证主页光标对齐修复：切到非 RID 模式重启后，HOME 的 '>' 应落在当前激活模式行。
用法: python verify_home.py COM5 [relay|camera|rid]"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
TARGET = sys.argv[2] if len(sys.argv) > 2 else "relay"

def open_port():
    s = serial.Serial(PORT, 115200, timeout=0.4, dsrdtr=False, rtscts=False)
    try:
        s.setDTR(False); s.setRTS(False)
    except Exception:
        pass
    return s

def send(s, cmd, wait=2.0):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    deadline = time.time() + wait
    buf = b""
    while time.time() < deadline:
        d = s.read(4096)
        if d:
            buf += d
    return buf.decode("utf-8", "replace")

s = open_port()
time.sleep(1.2)
print("==== current mode ====")
print(send(s, "mode", 1.2))

print("==== switching to %s (reboots) ====" % TARGET)
send(s, "mode %s" % TARGET, 1.0)   # triggers reboot
time.sleep(8)                       # wait reboot + boot

print("==== HOME after reboot (scr menu 0) ====")
print(send(s, "scr menu 0", 1.5))
print("==== oled dump @ HOME ====")
print(send(s, "oled dump", 1.5))

print("==== FUNC page (scr menu 1) ====")
print(send(s, "scr menu 1", 1.0))
print("==== oled dump @ FUNC ====")
print(send(s, "oled dump", 1.5))

s.close()
print("done")
