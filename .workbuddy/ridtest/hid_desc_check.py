#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""解析 crid_shutter.c 里的 HID Report Map，校验每条报告的字节长度与 Report ID。

用途：改描述符后不用连手机也能发现字节级错误（长度算错 / ID 漏写 / 顺序写反）。
"""
import re
import sys

SRC = "main/crid_shutter.c"

TAG_REPORT_ID = 0x08
TAG_REPORT_SIZE = 0x07
TAG_REPORT_COUNT = 0x09
MAIN_INPUT = 0x08
MAIN_OUTPUT = 0x09
MAIN_COLLECTION = 0x0A
MAIN_END_COLLECTION = 0x0C


def extract_map(path):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"static const uint8_t s_report_map\[\] = \{(.*?)\};", txt, re.S)
    if not m:
        raise SystemExit("找不到 s_report_map")
    body = m.group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    return [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)]


def parse(data):
    """返回 {(report_id, type): bits}，type: 'in' / 'out'"""
    i = 0
    size = 0
    count = 0
    rid = 0
    acc = {}
    depth = 0
    while i < len(data):
        b = data[i]
        i += 1
        if b == 0xFE:          # long item，本项目不用
            raise SystemExit("出现 long item，解析器不支持")
        bsize = b & 0x03
        btype = (b >> 2) & 0x03
        btag = (b >> 4) & 0x0F
        n = {0: 0, 1: 1, 2: 2, 3: 4}[bsize]
        val = 0
        for k in range(n):
            val |= data[i + k] << (8 * k)
        i += n

        if btype == 1:         # Global
            if btag == TAG_REPORT_SIZE:
                size = val
            elif btag == TAG_REPORT_COUNT:
                count = val
            elif btag == TAG_REPORT_ID:
                rid = val
        elif btype == 0:       # Main
            if btag == MAIN_INPUT:
                acc[(rid, "in")] = acc.get((rid, "in"), 0) + size * count
            elif btag == MAIN_OUTPUT:
                acc[(rid, "out")] = acc.get((rid, "out"), 0) + size * count
            elif btag == MAIN_COLLECTION:
                depth += 1
            elif btag == MAIN_END_COLLECTION:
                depth -= 1
    return acc, depth


def main():
    data = extract_map(SRC)
    print("Report Map 长度: %d 字节" % len(data))
    acc, depth = parse(data)
    print("集合闭合: %s (depth=%d)" % ("OK" if depth == 0 else "未闭合!", depth))
    print()

    # 从源码里读回固件实际使用的报告长度常量
    src = open(SRC, encoding="utf-8").read()

    def const(name):
        m = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+|\d+)" % name, src)
        return int(m.group(1), 0) if m else None

    expect = {
        (const("HID_REPORT_ID_KEYBOARD"), "in"):  const("SHUTTER_KB_REPORT_LEN"),
        (const("HID_REPORT_ID_KEYBOARD"), "out"): const("SHUTTER_LED_REPORT_LEN"),
        (const("HID_REPORT_ID_CONSUMER"), "in"):  const("SHUTTER_CC_REPORT_LEN"),
    }

    ok = True
    for k in sorted(acc.keys()):
        bits = acc[k]
        got = bits // 8
        rem = " (非整字节!)" if bits % 8 else ""
        want = expect.get(k)
        mark = ""
        if want is None:
            mark = "?? 未预期的报告"
            ok = False
        elif want != got:
            mark = "!= 固件常量 %d  -> 不符!" % want
            ok = False
        else:
            mark = "== 固件常量 %d  OK" % want
        print("Report ID 0x%02X %-3s : %3d bits = %d 字节%s  %s"
              % (k[0], k[1], bits, got, rem, mark))

    print()
    if not any(k[0] != 0 for k in acc):
        print("!! 描述符里没有任何非零 Report ID —— iOS 不会把它当 HID 键盘")
        ok = False
    print("结论:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
