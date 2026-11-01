# -*- coding: utf-8 -*-
"""USB Serial/JTAG 命令行小工具：直接对某块板下命令

用法:
  python cli.py COM5 "stat"            # 单条命令
  python cli.py COM6 "stat" "mode" 3   # 多条，间隔 0.6s
  python cli.py COM5 --boot 8          # 只抓开机日志 8 秒，不下命令
"""
import sys, time, serial, threading

def run(port, cmds, wait=2.5, boot=0):
    s = serial.Serial(port, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
    try:
        s.setDTR(False); s.setRTS(False)
    except Exception:
        pass

    if boot:
        t0 = time.time()
        while time.time() - t0 < boot:
            d = s.read(4096)
            if d:
                sys.stdout.write(d.decode("utf-8", "replace"))
        return

    time.sleep(1.2)          # 等 REPL 起来
    s.reset_input_buffer()
    for c in cmds:
        s.write((c + "\n").encode())
        deadline = time.time() + wait
        buf = b""
        while time.time() < deadline:
            d = s.read(4096)
            if d:
                buf += d
                if len(buf) > 40000:
                    break
        print("---- %s %s ----" % (port, c))
        sys.stdout.write(buf.decode("utf-8", "replace"))
        print()
    s.close()

if __name__ == "__main__":
    a = sys.argv[1:]
    port = a.pop(0)
    if a and a[0] == "--boot":
        run(port, [], boot=float(a[1]) if len(a) > 1 else 8)
    else:
        run(port, a)
