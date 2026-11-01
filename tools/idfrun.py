# -*- coding: utf-8 -*-
"""
idfrun.py — 免 export 环境的 idf.py 包装器（Windows / PowerShell 友好）

本机 ESP-IDF 工具链固定安装在 D:\\ESP8266\\toolChian，但当前 PowerShell
会话通常没有 IDF_PATH / PATH 注入，直接 idf.py 会报 "command not found"。
本脚本用固定路径拼出完整环境后调用 idf.py，并把输出写入 build/idf_logs/。

用法（在工程根目录或任意位置）：
  python tools\\idfrun.py build
  python tools\\idfrun.py -p COM5 flash
  python tools\\idfrun.py -p COM5 flash monitor
  python tools\\idfrun.py fullclean

约定：工程根目录由脚本所在位置的上一级自动推导，换机器只需改 TC 常量。
"""
import datetime
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TC = r"D:\ESP8266\toolChian"          # ESP-IDF + 工具链根目录
PY = os.path.join(TC, r".espressif\python_env\idf5.2_py3.11_env\Scripts\python.exe")
IDF = os.path.join(TC, r"esp-idf")


def build_env():
    env = dict(os.environ)
    # Git Bash / MSYS2 会注入 MSYSTEM。ESP-IDF v5.x 的 idf.py 一旦检测到该变量，
    # 只会打印 "MSys/Mingw is no longer supported" 告警然后跳过 main() 直接退出，
    # 表现为"构建看似成功(RC=0)但什么都没编译"。这里显式剔除，保证任意 shell 下都真正执行。
    env.pop("MSYSTEM", None)
    # 部分精简/沙箱 shell 未注入 PROCESSOR_ARCHITECTURE，platform.machine() 会返回空串，
    # 使 idf_tools.py 判定 "unknown platform" 而中止构建。这里补一个默认值。
    if not env.get("PROCESSOR_ARCHITECTURE"):
        env["PROCESSOR_ARCHITECTURE"] = "AMD64"
    # 精简 shell（如 Git Bash / 沙箱终端）常缺 SystemRoot 等 Windows 基础变量，
    # 会让 cmake、ninja 启动时报 STATUS_DLL_NOT_FOUND (0xC0000135)。这里补齐默认值。
    for k, v in (("SystemRoot", r"C:\Windows"),
                 ("SystemDrive", "C:"),
                 ("COMSPEC", r"C:\Windows\System32\cmd.exe"),
                 ("TEMP", r"C:\Windows\Temp"),
                 ("TMP", r"C:\Windows\Temp"),
                 ("PATHEXT", ".COM;.EXE;.BAT;.CMD;.VBS;.JS;.WSF;.WSH;.MSC")):
        if not env.get(k):
            env[k] = v
    # ccache 靠 LOCALAPPDATA 定位配置文件，缺失时报
    # "could not find configuration file and the LOCALAPPDATA environment variable is not set" 并中断编译。
    if not env.get("LOCALAPPDATA"):
        prof = env.get("USERPROFILE") or env.get("HOME") or os.path.expanduser("~")
        if prof:
            env["LOCALAPPDATA"] = os.path.join(prof, "AppData", "Local")
    env["IDF_PATH"] = IDF
    env["IDF_TOOLS_PATH"] = os.path.join(TC, ".espressif")
    env["IDF_PYTHON_ENV_PATH"] = os.path.join(TC, r".espressif\python_env\idf5.2_py3.11_env")
    env["OPENOCD_SCRIPTS"] = os.path.join(
        TC, r".espressif\tools\openocd-esp32\v0.12.0-esp32-20230921\openocd-esp32\share\openocd\scripts")
    env["ESP_ROM_ELF_DIR"] = os.path.join(TC, r".espressif\tools\esp-rom-elfs\20230320") + "\\"
    env["IDF_CCACHE_ENABLE"] = "1"
    prefix = [
        r"D:\esp\mingit\cmd",
        os.path.join(TC, r".espressif\tools\riscv32-esp-elf-gdb\12.1_20231023\riscv32-esp-elf-gdb\bin"),
        os.path.join(TC, r".espressif\tools\riscv32-esp-elf\esp-13.2.0_20230928\riscv32-esp-elf\bin"),
        os.path.join(TC, r".espressif\tools\cmake\3.24.0\bin"),
        os.path.join(TC, r".espressif\tools\openocd-esp32\v0.12.0-esp32-20230921\openocd-esp32\bin"),
        os.path.join(TC, r".espressif\tools\ninja\1.11.1"),
        os.path.join(TC, r".espressif\tools\idf-exe\1.0.3"),
        os.path.join(TC, r".espressif\tools\ccache\4.8\ccache-4.8-windows-x86_64"),
        os.path.join(TC, r".espressif\python_env\idf5.2_py3.11_env\Scripts"),
        os.path.join(IDF, "tools"),
    ]
    env["PATH"] = ";".join(prefix) + ";" + env.get("PATH", "")
    return env


NINJA = os.path.join(TC, r".espressif\tools\ninja\1.11.1\ninja.exe")


def run_ninja(env):
    """直接跑 ninja 做增量编译。

    本机某些会话里 cmake.exe 无法启动（idf.py 会报 '"cmake" must be available on
    the PATH'），但 build/ 已用 Ninja 配置好，此时绕过 cmake 直接 ninja 即可完成
    增量编译。为避免 ninja 触发重新 configure，这里把坏掉的 cmake 从 PATH 摘掉。
    """
    e2 = dict(env)
    e2["PATH"] = ";".join(p for p in e2["PATH"].split(";") if "cmake" not in p.lower())
    return subprocess.run([NINJA], cwd=os.path.join(ROOT, "build"),
                          env=e2, capture_output=True, text=True, errors="replace")


def parse_port(argv):
    """从命令行里取出 -p / --port 指定的串口，取不到则回退 COM5。"""
    for i, a in enumerate(argv):
        if a in ("-p", "--port") and i + 1 < len(argv):
            return argv[i + 1]
        if a.startswith("--port="):
            return a.split("=", 1)[1]
    return "COM5"


def run_esptool_flash(env, port):
    """直接用 esptool 烧录（build/flash_args 里已写好三个分区的偏移）。"""
    args = [PY, "-m", "esptool", "--chip", "esp32c3", "-p", port, "-b", "460800",
            "--before", "default_reset", "--after", "hard_reset", "write_flash", "@flash_args"]
    return subprocess.run(args, cwd=os.path.join(ROOT, "build"),
                          env=env, capture_output=True, text=True, errors="replace")


def main():
    logdir = os.path.join(ROOT, "build", "idf_logs")
    os.makedirs(logdir, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    argv = sys.argv[1:]
    args = [PY, os.path.join(IDF, r"tools\idf.py")] + argv
    env = build_env()
    r = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors="replace")
    out = (r.stdout or "") + (r.stderr or "")

    # cmake 不可用时自动退化：build -> ninja 增量编译；flash -> esptool 直接烧录
    if r.returncode != 0 and "must be available on the PATH" in out:
        if "build" in argv:
            print("[警告] idf.py 无法调用 cmake，改用 ninja 增量编译")
            r2 = run_ninja(env)
            out += "\n===== ninja fallback =====\n" + (r2.stdout or "") + (r2.stderr or "")
            r = r2
        elif "flash" in argv:
            port = parse_port(argv)
            print("[警告] idf.py 无法调用 cmake，改用 esptool 直接烧录 (%s)" % port)
            r2 = run_esptool_flash(env, port)
            out += "\n===== esptool fallback (%s) =====\n" % port + (r2.stdout or "") + (r2.stderr or "")
            r = r2

    log = os.path.join(logdir, "idf_%s.log" % stamp)
    open(log, "w", encoding="utf-8").write(out)
    print("RC =", r.returncode)
    print("log =", log)
    print(out[-3000:])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
