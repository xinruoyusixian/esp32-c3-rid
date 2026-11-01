# -*- coding: utf-8 -*-
"""三种模式屏幕渲染验证：抓 /api/oled_dump 并解码成文本"""
import io, json, os, sys, time, urllib.request, urllib.error

HOST = "192.168.4.1"
BASE = "http://" + HOST
DUMP_DIR = os.path.dirname(os.path.abspath(__file__))
SRC = "C:/Users/Alan/Downloads/esp32-c3-rid-main/main/crid_oled.c"

sys.path.insert(0, DUMP_DIR)


def get(path, timeout=8, retry=3):
    last = None
    for _ in range(retry):
        try:
            with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except Exception as e:
            last = "%s: %s" % (type(e).__name__, str(e)[:60])
            time.sleep(1.5)
    return 0, last


def post(path, body, timeout=10, retry=3):
    data = json.dumps(body).encode()
    last = None
    for _ in range(retry):
        try:
            req = urllib.request.Request(BASE + path, data=data, method="POST")
            req.add_header("Content-Type", "application/json")
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except Exception as e:
            last = "%s: %s" % (type(e).__name__, str(e)[:60])
            time.sleep(1.5)
    return 0, last


def dump(name):
    st, d = get("/api/oled_dump", timeout=10)
    if st != 200:
        print("  dump failed: %s" % d[:80])
        return None
    with io.open(os.path.join(DUMP_DIR, name + ".txt"), "w") as f:
        f.write(d)
    return d


def show(name, d):
    """把 72x40 的 ASCII 图按行打印（压缩空白，方便肉眼确认）"""
    if not d:
        return
    rows = [l for l in d.replace("\r", "").split("\n") if l]
    print("  -- %s --" % name)
    for i, r in enumerate(rows):
        print("  %2d|%s|" % (i, r.replace(".", " ")))


def decode(d):
    """用 5x7 字模把 header/row0/1/2 解成文本"""
    import re
    text = io.open(SRC, encoding="utf-8").read()
    m = re.search(r"static\s+const\s+uint8_t\s+font5x7\s*\[\]\s*=\s*\{(.*?)\};", text, re.S)
    vals = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
    font = {}
    for code in range(0x20, 0x7F):
        base = (code - 0x20) * 5
        if base + 5 > len(vals):
            break
        font[code] = vals[base:base + 5]
    rows = [l for l in d.replace("\r", "").split("\n") if l]
    out = []
    for label, x0, y0, inv, ncol in (("hdr", 2, 1, True, 11),
                                     ("r0", 0, 12, False, 12),
                                     ("r1", 0, 20, False, 12),
                                     ("r2", 0, 28, False, 12)):
        chars = []
        for slot in range(ncol):
            cx = x0 + slot * 6
            if cx + 5 > 72:
                break
            best, bd = " ", None
            for code, cols in font.items():
                delta = 0
                for c in range(5):
                    for r in range(7):
                        on = (cols[c] >> r) & 1
                        px = (0 if on else 1) if inv else on
                        want = 1 if rows[y0 + r][cx + c] == "#" else 0
                        if px != want:
                            delta += 1
                if bd is None or delta < bd:
                    bd, best = delta, chr(code)
                    if delta == 0:
                        break
            chars.append(best)
        out.append((label, "".join(chars).rstrip()))
    return out


def main():
    st, mode = get("/api/mode")
    print("mode:", mode)
    st, oled = get("/api/oled")
    print("oled:", oled)

    if len(sys.argv) > 1 and sys.argv[1] == "shots":
        # 连续抓若干帧，观察轮播
        n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
        gap = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
        for i in range(n):
            d = dump("frame%d" % i)
            if d:
                print("frame%d:" % i, decode(d))
            time.sleep(gap)
        return 0

    d = dump("dump_now")
    if d:
        print(decode(d))
        show("dump_now", d)
    return 0


if __name__ == "__main__":
    sys.exit(main())
