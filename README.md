# ESP32-C3 RID Scanner + Simulator

基于 [luolitao/esp32-crid](https://github.com/luolitao/esp32-crid) 项目，为 ESP32-C3 SuperMini 开发板定制的合并固件。

## 功能

- **C-RID 嗅探与解码** — 接收并解码周围所有无人机 Remote ID 广播信号（支持 GB 42590-2023 / ASTM F3411-22a）
- **NVS 缓存** — 自动将接收到的无人机数据缓存到 Flash，直至存储空间用尽
- **C-RID 模拟发送** — 模拟无人机发送 RID 信息，让周围接收设备可以接收和解码
- **WiFi AP + Captive Portal** — 开发板发出名为 `rid` 的 WiFi，密码 `12345678`，手机连接后自动跳转到管理后台
- **Web 管理后台** — 仪表盘、无人机列表、详情页（含高德地图一键导航）、模拟器参数配置
- **串口调试输出** — UART1 (GPIO4 TX) 输出 JSON 格式调试信息，波特率 115200

## 硬件连接

| 功能 | GPIO |
|------|------|
| USB 串口 (内置) | GPIO18/19 (USB_D-/D+) |
| 调试串口 TX | GPIO4 |
| 调试串口 RX | GPIO5 |
| 调试串口 波特率 | 115200 |

## 快速开始

### 前置条件

安装 ESP-IDF v5.2.1 或更高版本：
```bash
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c3
. ./export.sh
```

### 编译（本机推荐）

本机工具链固定在 `D:\ESP8266\toolChian`，PowerShell 默认没有注入 `IDF_PATH`，直接 `idf.py` 会找不到命令。
统一用封装脚本（自动拼环境，日志写 `build/idf_logs/`）：

```powershell
python tools\idfrun.py build
python tools\idfrun.py -p COM5 flash
python tools\idfrun.py -p COM5 monitor
```

下面是通用 IDF 流程，换机器时参考：

### 编译

```bash
cd esp32-rid
idf.py set-target esp32c3
idf.py build
```

### 烧录

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

(Windows 上使用 COM 端口，如 `idf.py -p COM3 flash monitor`)

### 合并固件（用于量产烧录）

```bash
esptool.py --chip esp32c3 merge_bin \
  -o esp32-c3-rid-combined.bin \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0 build/bootloader/bootloader.bin \
  0x10000 build/esp32_crid.bin \
  0x8000 build/partition_table/partition-table.bin
```

## 使用说明

1. 开发板上电后，会发出 WiFi 热点：
   - **SSID**: `rid`
   - **密码**: `12345678`

2. 手机连接该 WiFi 后，浏览器会自动跳转到管理后台（Captive Portal）

3. 管理后台（`http://192.168.4.1`）为五页签结构，顶部状态栏常驻显示可用内存 / 当前模式 / LED / STA IP：
   - **侦听**: 在线数、总接收数、无人机列表与详情（含高德导航）、轨迹模拟器、画板与 KMZ 上传
   - **键盘**: BLE 快门连接状态、连拍参数、虚拟键盘、快捷键（编辑模式下才可增删）
   - **中继**: 中继状态、上游 WiFi 扫描与连接、热点 SSID/密码
   - **设置**: 模式切换、设备信息、Captive Portal 跳转地址、蓝牙广播名、清除配对
   - **API**: 内置接口文档表格（`API_DOC` 渲染）

4. 串口调试：将 USB-TTL 模块连接到 GPIO4(TX) + GPIO5(RX) + GND，波特率 115200

## 项目结构

```
esp32-c3-rid-main/
├── main/                      # 主固件（唯一参与编译的组件）
│   ├── app_main.c             # 入口：WiFi AP / 模拟器任务 / 状态输出
│   ├── crid_sniffer.c         # Wi-Fi 嗅探（promiscuous）
│   ├── crid_parser_*.c        # RID 协议解析（ASTM / GB 42590 / GB 46750）
│   ├── crid_tracker.c         # 无人机追踪表（堆分配，模式切换时释放）
│   ├── crid_web.c             # HTTP 服务 + 静态页 + 核心 API 路由
│   ├── crid_ext.c             # STA 联网 + /api/ext/* 对外 REST 接口
│   ├── crid_relay.c           # 中继模式（AP + STA + NAPT）
│   ├── crid_shutter.c         # BLE HID 键盘 / 快门（拍照模式）
│   ├── crid_mode.c            # 三模式（RID / CAMERA / RELAY）切换与 NVS 记录
│   ├── crid_led.c             # 板载蓝灯心跳（GPIO8）
│   ├── crid_json.c            # 串口 JSON 输出
│   ├── crid_display.c         # 解析结果字段整理（供 JSON / Web 复用）
│   ├── crid_config.c          # 模拟器参数
│   ├── crid_messages.c        # RID 报文构建
│   ├── crid_wifi.c            # 模拟发送
│   ├── crid_patrol.c          # 巡游模拟
│   ├── crid_nvs.c             # 无人机数据缓存
│   ├── crid_serial.c          # UART1 调试输出
│   ├── crid_ota.c             # OTA（预留，当前未加入 CMakeLists SRCS）
│   └── index_html.h           # 编译进固件的 Web 页面（自动生成，勿手改）
├── build_src/index.html       # Web 页面的可编辑源文件（由 tools/html_tool.py 解出）
├── tools/
│   ├── idfrun.py              # 免 export 的 idf.py 包装器（固定工具链路径）
│   ├── html_tool.py           # index_html.h <-> index.html 互转 + JS 校验
│   └── ...                    # 串口监视 / OTA 工具等（详见 tools/README.md）
├── docs/                      # EXT_API.md（对外接口）、HANDOVER.md（交接文档）
├── components/opendroneid/    # OpenDroneID 解码库
├── partition_table/           # 分区表
├── archive/                   # 历史日志与旧版本备份（已 gitignore）
├── main_rx/ main_tx/          # 早期双板（收/发）方案，不参与本工程编译
├── rid_server/                # PC 端 Python 接收服务，独立运行
└── env/                       # 本地 venv，不参与编译
```

## Web 前端维护

页面以 C 字符串常量编译进固件，`main/index_html.h` 是生成物，不要直接手改：

```bash
python tools/html_tool.py unpack   # index_html.h -> build_src/index.html
# 编辑 build_src/index.html
python tools/html_tool.py embed    # 回写 index_html.h（自动备份 + roundtrip 校验）
python tools/html_tool.py check    # 只校验，不写文件
```

> 历史坑：旧脚本把超长行折成多个字面量，且中间段漏掉 `\n`，导致内联 JS 被截断
> （`Uncaught ReferenceError: switchTab is not defined`）。`html_tool.py` 已按物理行
> 逐行生成并做 roundtrip 校验，不会再出现该问题。

## API 接口

完整清单见 [docs/HANDOVER.md](docs/HANDOVER.md) 第 6 节；对外接口说明见 [docs/EXT_API.md](docs/EXT_API.md)。

| 分类 | 方法 | 路径 | 说明 |
|------|------|------|------|
| 状态 | GET | /api/status | 在线数、总数、内存、模式 |
| 数据 | GET | /api/drones | 无人机列表 |
| 数据 | POST | /api/trajectory | 画板/轨迹点上传 |
| 数据 | POST | /api/clear_cache | 清空缓存 |
| 模拟 | GET/POST | /api/sim_config | 读取 / 保存模拟器参数 |
| 模拟 | POST | /api/sim_toggle | 开关模拟发送 |
| 拍照 | GET | /api/photo_status | BLE 快门状态 |
| 拍照 | POST | /api/photo, /api/photo_config, /api/photo_stop | 单拍 / 参数 / 停止 |
| 拍照 | GET/POST | /api/key, /api/key/text | 按键、文本输入 |
| 拍照 | POST | /api/ble_forget | 清除蓝牙配对 |
| 中继 | GET | /api/relay | 中继状态 |
| 中继 | POST | /api/relay_config | 中继 SSID / 密码配置 |
| 模式 | GET/POST | /api/mode | 查询 / 切换 RID·CAMERA·RELAY |
| 对外 | GET/POST | /api/ext/* | STA 联网后的外部系统接口（同时支持 query 与 JSON body） |

## 固件下载

在 GitHub Actions 中自动构建，可在 [Releases](../../releases) 页面下载最新固件。

## 许可

基于原项目 [luolitao/esp32-crid](https://github.com/luolitao/esp32-crid) 修改。
