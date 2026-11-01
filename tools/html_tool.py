# -*- coding: utf-8 -*-
"""
html_tool.py — main/index_html.h 与纯 HTML 源码之间的互转工具

背景
----
固件不经过文件系统，Web 页面以 C 字符串常量 INDEX_HTML 的形式编译进固件
（main/index_html.h）。直接编辑该文件可读性极差，早期版本还存在
"超长行被折成多个字面量"导致内联 JS 被截断的缺陷（表现为
Uncaught ReferenceError: switchTab is not defined）。

本工具保证：
  1. unpack 能把 index_html.h 无损还原为单份纯 HTML，便于用编辑器/VSCode 修改；
  2. embed 能把纯 HTML 回写为 C 字符串，按物理行拆分，且**每一行末尾的 \\n
     只加一次**，不会把一行 HTML 折成多段（这是历史缺陷的根因）；
  3. embed 后自动做 roundtrip 校验（unpack(embed(x)) == x），不一致则报错退出。

用法
----
  python tools/html_tool.py unpack                 # index_html.h -> build_src/index.html
  python tools/html_tool.py embed                  # build_src/index.html -> index_html.h（自动备份）
  python tools/html_tool.py check                  # 只做 roundtrip 自检，不写任何文件
  python tools/html_tool.py jscheck               # 抽取内联 JS 做括号/引号平衡检查

说明
----
  - 备份写入 main/index_html.h.bak，覆盖写前由脚本自动生成；
  - 文件编码统一 UTF-8（无 BOM），与 target_compile_options 的 -finput-charset=utf-8 一致。
"""
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "main", "index_html.h")
SRC_DIR = os.path.join(ROOT, "build_src")
HTML = os.path.join(SRC_DIR, "index.html")

HEADER = "// Auto-generated embedded HTML\nstatic const char INDEX_HTML[] =\n"


# ---------------- C 字符串 <-> 纯文本 ----------------

def unpack_c_strings(path):
    """把 C 源文件里所有独立成行的字符串字面量拼接成纯文本。"""
    text = open(path, encoding="utf-8", errors="replace").read()
    lits = re.findall(r'^[ \t]*"((?:[^"\\]|\\.)*)"[ \t]*$', text, re.M)
    if not lits:
        raise SystemExit("未在 %s 中找到字符串字面量" % path)
    out = []
    for body in lits:
        # 只解析 C 里合法且本工程用到的转义，未知转义按原样保留，避免二次改写
        buf = []
        i = 0
        while i < len(body):
            c = body[i]
            if c != "\\":
                buf.append(c)
                i += 1
                continue
            nxt = body[i + 1] if i + 1 < len(body) else ""
            mapping = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}
            if nxt in mapping:
                buf.append(mapping[nxt])
            else:
                buf.append("\\" + nxt)
            i += 2
        out.append("".join(buf))
    return "".join(out)


def embed_html(html):
    """纯 HTML -> 逐行 C 字符串字面量。

    每行末尾恰好一个 \\n，最后一行同样带 \\n，结尾以独占一行的 ";" 收束，
    这样任何单行 HTML 都不会被拆成多个字面量（历史缺陷的根因）。
    """
    lines = html.split("\n")
    # HTML 以换行结尾时会多出空元素，去掉避免生成空字面量
    if lines and lines[-1] == "":
        lines.pop()
    body = []
    for line in lines:
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        body.append('"%s\\n"' % esc)
    return HEADER + "\n".join(body) + "\n;\n"


# ---------------- 校验 ----------------

def js_balance(html):
    """轻量 JS 检查：抽出 <script> 内容，检查括号与引号是否平衡。

    不追求完整语法校验（无 JS 引擎），但足以发现"长行被截断"这类历史故障。
    """
    blocks = re.findall(r"<script[^>]*>(.*?)</script>", html, re.S)
    js = "\n".join(blocks)
    pairs = {")": "(", "]": "[", "}": "{"}
    stack = []
    i = 0
    n = len(js)
    while i < n:
        c = js[i]
        if c in "\"'`":
            quote = c
            i += 1
            while i < n:
                if js[i] == "\\":
                    i += 2
                    continue
                if js[i] == quote:
                    break
                i += 1
            if i >= n:
                return "未闭合的字符串字面量（%s）" % quote
        elif c == "/" and i + 1 < n and js[i + 1] == "/":
            while i < n and js[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and js[i + 1] == "*":
            i += 2
            while i + 1 < n and not (js[i] == "*" and js[i + 1] == "/"):
                i += 1
            i += 1
        elif c in "([{":
            stack.append(c)
        elif c in ")]}":
            if not stack or stack.pop() != pairs[c]:
                return "括号不平衡，位置 %d 附近: %s" % (i, js[max(0, i - 40):i + 10].replace("\n", " "))
        i += 1
    if stack:
        return "仍有未闭合的括号: %s" % "".join(stack)
    # 关键符号存在性检查（历史故障的典型症状）
    for token in ("function switchTab", "function updateTopbar", "function loadApi"):
        if token not in js:
            return "缺少关键函数: %s" % token
    return None


# ---------------- 命令 ----------------

def do_unpack():
    html = unpack_c_strings(HDR)
    os.makedirs(SRC_DIR, exist_ok=True)
    open(HTML, "w", encoding="utf-8", newline="\n").write(html)
    print("unpack OK: %s -> %s (%d chars, %d lines)" % (HDR, HTML, len(html), html.count("\n") + 1))
    err = js_balance(html)
    print("js check:", "OK" if err is None else "FAIL - " + err)


def do_embed(backup=True):
    if not os.path.exists(HTML):
        raise SystemExit("缺少源文件 %s，请先 unpack" % HTML)
    html = open(HTML, encoding="utf-8").read()
    # 统一换行，避免 CRLF 带来的 \r 混入固件字符串
    html = html.replace("\r\n", "\n")
    err = js_balance(html)
    if err is not None:
        print("[警告] JS 检查未通过，仍继续写入：%s" % err)

    out = embed_html(html)
    # roundtrip 自检
    back = unpack_c_strings_text(out)
    # 允许末尾换行差异（末行不再追加 \n，HTML 语义不变）
    if back.rstrip("\n") != html.rstrip("\n"):
        raise SystemExit("roundtrip 校验失败，已放弃写入（请检查 HTML 是否含有异常字符）")

    if backup and os.path.exists(HDR):
        # 备份不覆盖历史备份，避免连续 embed 时把旧版本冲掉
        dst = HDR + ".bak"
        n = 2
        while os.path.exists(dst):
            dst = "%s.bak%d" % (HDR, n)
            n += 1
        shutil.copy2(HDR, dst)
        print("backup ->", os.path.basename(dst))
    open(HDR, "w", encoding="utf-8", newline="\n").write(out)
    print("embed OK: %s (%d bytes)" % (HDR, len(out.encode("utf-8"))))


def unpack_c_strings_text(text):
    lits = re.findall(r'^[ \t]*"((?:[^"\\]|\\.)*)"[ \t]*$', text, re.M)
    out = []
    for body in lits:
        buf = []
        i = 0
        while i < len(body):
            c = body[i]
            if c != "\\":
                buf.append(c)
                i += 1
                continue
            nxt = body[i + 1] if i + 1 < len(body) else ""
            mapping = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'"}
            buf.append(mapping.get(nxt, "\\" + nxt))
            i += 2
        out.append("".join(buf))
    return "".join(out)


def do_check():
    html = unpack_c_strings(HDR)
    back = unpack_c_strings_text(embed_html(html))
    print("roundtrip:", "EQUAL" if back == html else "DIFF")
    err = js_balance(html)
    print("js check:", "OK" if err is None else "FAIL - " + err)
    print("tabs:", [t for t in ("listen-home", "sim-basic", "kb-link", "net-relay", "sys-mode")
                    if (('id="%s"' % t) in html)])


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "unpack":
        do_unpack()
    elif cmd == "embed":
        do_embed()
    elif cmd == "check":
        do_check()
    elif cmd == "jscheck":
        html = unpack_c_strings(HDR)
        err = js_balance(html)
        print("js check:", "OK" if err is None else "FAIL - " + err)
        sys.exit(0 if err is None else 1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
