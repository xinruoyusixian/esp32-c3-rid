"""iOS 键盘端到端测试：等到 iPhone 连上并订阅键盘通知后自动发送文本。

用法: python ios_type.py COM5 "Hello" [等待秒数]
"""
import re
import sys
import time

import serial

KEYS = ("ready=", "handles", "notify", "tx ")


def open_port(port):
    return serial.Serial(port, 115200, timeout=1.0, dsrdtr=False, rtscts=False)


def snapshot(ser):
    """返回 (connected, encrypted, notify_kb, tx_kb)"""
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    ser.write(b"ble\n")
    end = time.time() + 2.5
    buf = []
    while time.time() < end:
        d = ser.read(4096)
        if d:
            buf.append(d.decode("utf-8", "replace"))
    txt = "".join(buf)
    out = {}
    for ln in txt.splitlines():
        l = ln.strip()
        m = re.match(r"^ready=\d+ connected=(\d+) encrypted=(\d+)", l)
        if m:
            out["connected"] = int(m.group(1))
            out["encrypted"] = int(m.group(2))
        m = re.match(r"^notify kb=(\d+) cc=(\d+)", l)
        if m:
            out["notify_kb"] = int(m.group(1))
        m = re.match(r"^tx kb=(\d+) cc=(\d+) shots=(\d+)", l)
        if m:
            out["tx_kb"] = int(m.group(1))
    return out


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
    text = sys.argv[2] if len(sys.argv) > 2 else "Hello"
    wait = int(sys.argv[3]) if len(sys.argv) > 3 else 120

    ser = open_port(port)
    print("等待 iPhone 连接（最多 %ds）..." % wait)
    t0 = time.time()
    st = {}
    while time.time() - t0 < wait:
        st = snapshot(ser)
        if st.get("connected") and st.get("notify_kb"):
            print("  已连接并订阅键盘: %s" % st)
            break
        print("  %s" % (st or "(无响应)"))
        time.sleep(2)
    else:
        print("超时：iPhone 未连接或未订阅键盘通知")
        ser.close()
        return 1

    print("发送文本: %r" % text)
    ser.write(("type %s\n" % text).encode())
    time.sleep(max(1.5, 0.35 * len(text) + 1.0))
    st2 = snapshot(ser)
    print("发送后: %s" % st2)
    ser.close()

    ok = (st2.get("tx_kb", 0) > st.get("tx_kb", 0))
    print("结论: 键盘报告%s" % ("已发出 (tx_kb %s -> %s)" % (st.get("tx_kb"), st2.get("tx_kb"))
                               if ok else "未发出"))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
