---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 9ecf476f186a4e61c8d9e63e76c2d774_cf6a337dd2fb11f196c5525400248c00
    ReservedCode1: ql8gY9BurP6bjDq7y8DijGn9XiKnTMnjqG6nj0vFW/x/M/NCmaZIE6d8vjlqPW84+01NQisgeavKO6w9WS93udnF7AwV9yWmZ2i+lcajie6k/1bEi32jHo9iCt6RuEgn1HSGiVZp0m0qFyGkqzAkotaSUYXeMfnLE36MrT8UOzQA+GX9Xamtb2cuV54=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 9ecf476f186a4e61c8d9e63e76c2d774_cf6a337dd2fb11f196c5525400248c00
    ReservedCode2: ql8gY9BurP6bjDq7y8DijGn9XiKnTMnjqG6nj0vFW/x/M/NCmaZIE6d8vjlqPW84+01NQisgeavKO6w9WS93udnF7AwV9yWmZ2i+lcajie6k/1bEi32jHo9iCt6RuEgn1HSGiVZp0m0qFyGkqzAkotaSUYXeMfnLE36MrT8UOzQA+GX9Xamtb2cuV54=
---

# ESP32-C3 RID 固件 —— 交接文档

> 更新时间：2026-10-29
> 适用固件：`esp32_crid.bin`（实测 `0x1521b0` 字节，占 app 分区 0x200000 的 66%，余 34%）
> 交接范围：`main/` 主固件 + Web 前端重构成果 + 构建烧录流程 + 已知坑

---

## 1. 当前状态（一句话）

固件已编译通过并在 **COM5** 烧录成功、可正常启动运行；Web 前端完成「侦听 / 键盘 / 中继 / 设置 / API」五页签重构并生效；
源码已整理（历史日志与旧备份归档至 `archive/2026-10-29/`，Web 页面构建流程工具化为
`tools/html_tool.py`）；**唯一未完成的验收项是浏览器端到端复验**（人工或自动化点一遍五个页签）。

---

## 2. 环境与构建

| 项 | 值 |
|----|----|
| 芯片 / 开发板 | ESP32-C3（SuperMini） |
| ESP-IDF | v5.2.x，工具链根目录固定 `D:\ESP8266\toolChian` |
| Python 环境 | `D:\ESP8266\toolChian\.espressif\python_env\idf5.2_py3.11_env` |
| 烧录端口 | `COM5`（USB 串口，GPIO18/19） |
| AP | SSID `rid` / 密码 `12345678`，本机地址 `192.168.4.1` |
| 调试串口 | UART1，TX=GPIO4，RX=GPIO5，115200 |
| 板载灯 | GPIO8 蓝色 LED（低电平点亮，心跳指示） |

### 2.1 编译 / 烧录（推荐用封装脚本）

PowerShell 里通常没有 `IDF_PATH` 与工具链 PATH，直接 `idf.py` 会 "命令不存在"。
用 `tools/idfrun.py` 自动拼环境，日志自动写到 `build/idf_logs/idf_<时间戳>.log`（不再污染工程根目录）：

```powershell
python tools\idfrun.py build
python tools\idfrun.py -p COM5 flash
python tools\idfrun.py -p COM5 monitor        # Ctrl+] 退出
```

### 2.2 Web 页面构建（重要）

固件没有文件系统，页面以 C 字符串常量 `INDEX_HTML` 编进固件：
`build_src/index.html`（可编辑）→ `main/index_html.h`（生成物）。

```powershell
python tools\html_tool.py unpack    # index_html.h -> build_src/index.html
# 编辑 build_src/index.html
python tools\html_tool.py embed     # 回写（自动备份 index_html.h.bak，并做 roundtrip 校验）
python tools\html_tool.py check     # 只自检：roundtrip + 内联 JS 括号/字符串平衡 + 五个页签存在性
```

规则：**`main/index_html.h` 视为生成物，不手改**；改完必须 `build` + `flash` 才在设备上生效。

### 2.3 分区 / 镜像

- app 分区 `0x200000`（2 MB），当前占用约 1.32 MB。
- 量产合并镜像（如需）：

```bash
esptool.py --chip esp32c3 merge_bin -o esp32-c3-rid-combined.bin \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32_crid.bin
```

---

## 3. 源码结构

```
esp32-c3-rid-main/
├── main/                    # 唯一参与编译的组件（其余目录为历史/旁路）
│   ├── app_main.c           # 入口：AP 初始化、模拟器周期任务、状态日志
│   ├── crid_sniffer.c       # promiscuous 嗅探
│   ├── crid_parser_common.c / _astm.c / _gb42590.c / _gb46750.c  # 多协议解析
│   ├── crid_rx_types.h      # 接收端类型与常量
│   ├── crid_tracker.c       # 无人机追踪表（堆分配，可随模式切换释放/重建）
│   ├── crid_web.c           # HTTP server + 核心 API 路由（约 47 KB，路由集中在文件尾 uri_handlers[]）
│   ├── crid_ext.c           # STA 联网 + /api/ext/* 外部接口（GET/POST 双注册）
│   ├── crid_relay.c         # 网络中继（AP + STA + NAPT）
│   ├── crid_shutter.c       # BLE HID 键盘 / 快门（拍照模式，含 NimBLE）
│   ├── crid_mode.c          # RID / CAMERA / RELAY 三模式切换（NVS pending + 失败重启回退）
│   ├── crid_led.c           # 心跳灯
│   ├── crid_json.c          # 串口 JSON 输出
│   ├── crid_display.c       # 解析字段整理（JSON 与 Web 共用）
│   ├── crid_enum_names.h    # 枚举→名称 X-Macro
│   ├── crid_config.c        # 模拟器参数
│   ├── crid_messages.c      # RID 报文构建
│   ├── crid_wifi.c          # 模拟发送
│   ├── crid_patrol.c        # 巡游模拟
│   ├── crid_nvs.c           # 无人机数据缓存
│   ├── crid_serial.c        # UART 调试输出
│   ├── crid_ota.c / crid_ota_web.h   # OTA 预留，**未加入 CMakeLists SRCS，当前不编译**
│   └── index_html.h         # 生成物：Web 页面 C 字符串
├── build_src/index.html     # Web 页面可编辑源（675 行左右）
├── tools/idfrun.py          # 免 export 的 idf.py 封装
├── tools/html_tool.py       # 页面双向转换 + JS 自检
├── docs/EXT_API.md          # 对外 REST 接口说明（面向外部系统对接）
├── docs/HANDOVER.md         # 本文件
├── components/opendroneid/  # 解码库
├── main_rx/ main_tx/        # 早期双板（接收/发送）拆分方案，独立 CMakeLists，不影响本工程
├── rid_server/              # PC 端 Python 接收服务（SQLite），独立运行
├── env/                     # 本地 venv，不参与编译
└── archive/2026-10-29/      # 本次整理归档：旧构建/烧录日志、sdkconfig.bak2mb、index_html.h.bak(.bak2)、旧 web/index.html
```

---

## 4. 固件运行模型

### 4.1 三种模式（互斥，切换需重启）

| 模式 | 枚举 | 说明 | 切换代价 |
|------|------|------|----------|
| `rid` | `CRID_MODE_RID` | Wi-Fi RID 侦听（默认），promiscuous 固定信道 | 默认模式 |
| `camera` | `CRID_MODE_CAMERA` | BLE HID 键盘 / 快门拍照，释放追踪表把 RAM 让给 NimBLE | 需重启 |
| `relay` | `CRID_MODE_RELAY` | AP + STA 上游 + NAPT，AP 信道跟随上游 | 需重启（信道冲突） |

切换流程（`crid_mode.c`）：写 NVS `pending` → 后台任务执行 → 成功才落盘为正式模式；任一步失败 `esp_restart()`；
连续失败超上限自动回退另一模式，避免死循环。切换期间 HTTP 仍可访问。

### 4.2 WiFi

- 始终保留 AP（`rid` / `12345678`，`192.168.4.1`），网页入口不依赖外部路由。
- STA 由 `crid_ext.c` 状态机管理：开机主动扫描命中已保存 SSID 才连接；失败 3 次转 AP 待机，
  每 5 分钟重扫一次；掉线延迟重连，防抖动。
- RID 模式稳态空闲堆约 77–115 KB（`/api/status` 可查）。

---

## 5. Web 前端（本次重构成果）

### 5.1 结构

- **顶部状态栏**（`id="topbar"`）：可用内存 / 当前模式 / LED 状态 / STA IP。
  由 `updateTopbar()` 周期刷新，任何页签都可见。
- **五个主页签**：`dashboard`（侦听）、`shutter`（键盘）、`relay`（中继）、`settings`（设置）、`api`（API）。
- **两个二级页**：`drones`（无人机列表）、`simulator`（轨迹模拟器）+ `drone-detail`（详情，含高德导航）。
- 启动路由 `bootRoute()`：先查 `/api/mode`，camera 落 `shutter`、relay 落 `relay`、其余落 `dashboard`。

### 5.2 页签职责

| 页签 | 内容 |
|------|------|
| 侦听 | 在线数/总数、无人机列表、详情、轨迹模拟器、画板（上北下南左西右东）、KMZ 上传 |
| 键盘 | BLE 快门连接状态、连拍参数（间隔/首张延时/广播名）、虚拟键盘 `KB_ROWS`、快捷键列表与增删 |
| 中继 | 中继状态、上游 WiFi 扫描与连接、AP 名称/密码（`relay-ssid` / `relay-pass`） |
| 设置 | 模式切换按钮、设备信息、Captive Portal 跳转地址（`portal-url`）、蓝牙广播名、清除配对（`/api/ble_forget`） |
| API | 由 `API_DOC` 渲染的四组接口表格（系统/数据/拍照/中继），随页面发布 |

### 5.3 编辑模式与快捷键（用户明确要求的交互规则）

- 默认**正常模式**：快捷键按钮不可删除、不可拖拽，避免误触。
- 点「✏️ 编辑」进入**编辑模式**（`document.body.classList.toggle('edit-mode')`，`toggleEdit()`），
  才出现删除按钮与新增入口；点「✅ 完成」立即回到正常模式。
- 快捷键默认带 `📸 拍照`（`{l:'📸 拍照', k:'photo', a:'photo'}`）。
- 触发方式三种：`manual`（点击）/ `timer`（定时间隔）/ `random`（随机间隔），
  由 `autoTick()` 驱动，`autoStop()` 终止。
- 持久化：`localStorage`，键为 `kb_sc`（快捷键数组）、`portal_url`（Portal 跳转地址）。

> 注意：`portal_url` 目前只保存在浏览器本地，是否下发到固件（影响 Captive Portal 跳转目标）
> 尚未接后端接口，接手时如需生效需补一个 POST 接口 + NVS 存储。

---

## 6. HTTP 接口清单

路由分两处注册：`main/crid_web.c` 的 `uri_handlers[]`（核心）与 `main/crid_ext.c` 的 `routes[]`（对外）。

### 6.1 核心接口

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/`、`/index.html` | 返回内置页面 `INDEX_HTML` |
| GET | `/api/status` | 在线数、总数、内存、模式、模拟状态 |
| GET | `/api/drones` | 无人机列表 |
| POST | `/api/trajectory` | 画板/轨迹点上传（批量经纬度） |
| POST | `/api/clear_cache` | 清空 NVS 缓存 |
| GET/POST | `/api/sim_config` | 读取 / 保存模拟器参数 |
| POST | `/api/sim_toggle` | 开 / 关模拟发送 |

### 6.2 拍照（BLE HID）相关

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/photo_status` | 连接状态、剩余连拍、当前参数 |
| POST | `/api/photo` | 触发一次拍照 |
| POST | `/api/photo_config` | 连拍间隔、首张延时、广播名 |
| POST | `/api/photo_stop` | 停止连拍 |
| GET/POST | `/api/key` | 读按键定义 / 发送按键 |
| POST | `/api/key/text` | 发送文本（HID 键盘输入） |
| POST | `/api/ble_forget` | 清除蓝牙配对信息 |

### 6.3 中继与模式

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/relay` | 中继状态（上游 SSID、IP、连接结果） |
| POST | `/api/relay_config` | 设置 AP SSID/密码、上游凭据 |
| GET/POST | `/api/mode` | 查询 / 切换 `rid`·`camera`·`relay`（异步，切换后重启） |

### 6.4 对外接口（STA 联网后供外部系统调用）

`/api/ext/` 下为**同一 handler 同时注册 GET 与 POST**，参数优先取 JSON body，回退 URL query（query 自动 URL 解码）：

`/api/ext/status`、`/api/ext/sta`、`/api/ext/wifi`、`/api/ext/scan`、`/api/ext/uavs`、
`/api/ext/clear`、`/api/ext/push_config`、`/api/ext/test_push`、`/api/ext/devices`、`/api/ext/new_devices`

字段与报文格式见 `docs/EXT_API.md`；前端「API」页展示的是 `API_DOC` 数组，**改动接口时需同步这两处**（无自动生成机制）。

---

## 7. 已知坑与注意事项

1. **内联 JS 被截断（已修复，务必用工具）**
   旧脚本把超长行折成多个字符串字面量且中间段漏 `\n`，导致 JS 语句被拼断，
   页面报 `Uncaught ReferenceError: switchTab is not defined`、`sim_running` 等变量未定义、整站失效。
   现由 `tools/html_tool.py embed` 按物理行逐行生成 + roundtrip 校验，本次已把历史残留的多处折行（约 10 行）合并为规范单行。
2. **改页面不重新烧录 = 白改**。`index_html.h` 是编译期常量，必须 build + flash；设备端无文件系统可热更。
3. **浏览器缓存**。烧录后仍看到旧页面时，硬刷新（Ctrl+F5）或换无痕窗口；`location.origin` 相关的 Portal 地址也会缓存到 `localStorage`。
4. **模式切换会重启**。`/api/mode` 是异步的，返回成功只代表已排入切换；切换后 IP / 信道 / 页面默认页签都会变（`bootRoute` 已适配）。切换期间切勿断电。
5. **中继与侦听不能共存**。中继 AP 信道必须跟随上游，与嗅探固定信道冲突，因此设计成独立模式。
6. **camera 模式内存紧张**。进 camera 前会释放追踪表并关 promiscuous，把 RAM 让给 NimBLE；此时无人机列表为空属正常。
7. **构建日志位置已改**。旧 `idfrun.py` 把日志写到工程根目录（`build_log.txt`、`flash_log.txt` 等），已归档；新脚本写 `build/idf_logs/`。
8. **OTA 未接线**。`crid_ota.c`、`crid_ota_web.h` 存在但不在 `CMakeLists.txt` 的 SRCS 中，也没有注册路由，属预留代码。
9. **`main/web/index.html` 已归档**。构建只读 `main/index_html.h`；旧文件在 `archive/2026-10-29/legacy_web_index.html`，不要误改。
10. **`main_rx/`、`main_tx/`、`rid_server/` 不属于本工程构建**。前者是早期双板方案，后者是 PC 端 Python 服务。

---

## 8. 待办 / 建议接手顺序

1. **端到端复验（最高优先）**：连 `rid` 热点 → `192.168.4.1`，依次点五个页签：
   顶栏内存/模式/IP 是否刷新；键盘页快捷键增删与编辑模式收敛；中继页扫描上游；设置页切模式；API 页表格是否完整。
2. **`portal_url` 落固件**：补 `/api/portal_config` + NVS，使 Captive Portal 跳转目标可改（当前仅前端本地保存）。
3. **API 文档单一来源**：`API_DOC`（前端）、`uri_handlers[]`（后端）、`docs/EXT_API.md` 三处手工同步，建议后续抽成一份 JSON 由脚本生成。
4. **OTA 接线或删除**：决定是否启用 `crid_ota.c`，否则从仓库剔除以免误导。
5. **常规清理**：`archive/` 只增不删可以，但建议按日期季度归档一次。

---

## 9. 关键文件索引

| 想改什么 | 去哪 |
|----------|------|
| 页面样式 / 布局 / 交互 | `build_src/index.html` → `tools/html_tool.py embed` |
| 新增 HTTP 接口 | `main/crid_web.c`（核心）或 `main/crid_ext.c`（对外） |
| 模式切换逻辑 | `main/crid_mode.c` |
| BLE 快门 / 键盘 | `main/crid_shutter.c` |
| 中继（NAPT、上游） | `main/crid_relay.c` |
| 解析协议、字段 | `main/crid_parser_*.c`、`main/crid_rx_types.h` |
| 串口 JSON 输出字段 | `main/crid_json.c` |
| 构建 / 烧录 | `tools/idfrun.py` |

---

## 10. 二次重构记录（2026-10-29）

### 10.1 页面信息架构重整

原来 5 个平级页签里塞了 15 个功能块，入口分散。改为 **主分组 + 二级子页** 两级结构：

| 主组 | 子页 | 说明 |
|------|------|------|
| 🚁 侦听 | 概览 / 无人机列表 | 默认页，进设备先看这里 |
| 📡 模拟 | 基本参数 / KMZ 轨迹 / 画板绘制 | 全部模拟器能力归拢 |
| ⌨️ 键盘 | 蓝牙连接 / 键盘输入 / 快捷键 / 拍照连拍 | 原"拍照"页拆分 |
| 🌐 网络 | 中继状态 / 上游 WiFi / 热点设置 | 联网能力集中 |
| ⚙️ 系统 | 模式与设备 / 门户网址 / 接口手册 | 配置与文档 |

视觉风格（深色卡片、青紫配色、布局）保持不变。

### 10.2 懒加载机制（核心改动）

新增 `PAGES` 注册表，每个页面声明自己的生命周期回调，**切到才初始化，离开就停止轮询**：

```js
"listen-home":{g:"listen",sub:"listen-home",tick:async function(){...},iv:5000},
"sim-pad":{g:"sim",sub:"sim-pad",init:simPadInit,leave:simPadLeave},
```

- `init` — 进入时执行一次
- `tick` / `iv` — 周期性刷新（仅当前页运行）
- `leave` — 离开时的清理
- 切换到新页时 `clearInterval(pageTimer)` 停掉上一页轮询

实测：进入设备只发 `/api/status` + `/api/mode` 两个请求，其余功能的数据在自己页面才会拉取；离开后轮询归零。

**新增注册的页面必须写进 `PAGES`，否则 `switchTab` 无法照顾它的定时器。**

### 10.3 修复与优化

| 项 | 位置 | 说明 |
|----|------|------|
| 构建脚本无法在 Git Bash 下运行 | `tools/idfrun.py` | `MSYSTEM` 会让 idf.py 直接退出；补 `PROCESSOR_ARCHITECTURE`、`SystemRoot`、`LOCALAPPDATA`（ccache 依赖）等 Windows 基础环境变量 |
| cmake 不可用时的兜底 | `tools/idfrun.py` | idf.py 失败自动退化为 ninja 增量编译 |
| **模式切换时 HTTP 响应丢失** | `main/crid_mode.c` | `MODE_SWITCH_DELAY_MS` 400 → 800，给足时间把响应发出去再重启 |
| 切换模式后要手动重连刷新 | `build_src/index.html` | 新增 `awaitReboot()`：遮罩 + 倒计时 + 每 2 秒探测 `/api/mode`，设备回来后自动 `location.reload()`；45 秒未恢复才提示手动处理 |
| 中继接口越界轮询 | `build_src/index.html` | 全局 tick 里的 `refreshRelay()` 从每 15 秒降为每 30 秒（进【网络】组仍是 5 秒） |

### 10.4 重大坑：串口工具会复位设备（务必先看）

**用 pyserial 打开 COM5 会触发 `USB_UART_CHIP_RESET`（rst:0x15）把板子硬件复位**，热点随之消失、HTTP 请求超时。

表现为"设备莫名其妙重启、网页打不开"，极易误判成固件崩溃。规避办法：

- 需要看串口日志时，**先看日志再测 HTTP**，且每次开串口后等设备重新起来（约 8 秒）
- 测试脚本目录下有 `watch.py`，同时监听串口 + 打 HTTP 请求，可区分"真崩溃"与"自己复位"

### 10.5 本轮测试方式

用 jsdom 加载真实页面 + 把 fetch 转发到 `192.168.4.1`，跑真实 DOM 交互：

- 15 个页面导航正确性（唯一可见页 + 子导航同步）
- 懒加载边界（未进入页面不发起请求、离开即停）
- 模式三轮切换 + 模拟器开关 + BLE 文本发送 + 中继状态：18 项全通过
- 全键盘 65 键渲染正常

测试脚本在 `../../WorkBuddy/Claw/.workbuddy/ridtest/`（test.js 页面交互 / func.py 模式轮转 / watch.py 串口+HTTP 联合监控）。

---

## 11. 导航与门户配置重构（2026-10-29 第二轮）

### 11.1 顶层入口调整为 4 个

| 顺序 | 顶层入口 | data-mode | 二级内容 |
|---|---|---|---|
| 1 | 🚁 无人机监控 | `rid` | 侦听（概览 / 无人机列表）、模拟（基本参数 / KMZ 轨迹 / 画板绘制） |
| 2 | ⌨️ 键盘 | `camera` | 蓝牙连接 / 键盘输入 / 快捷键 / 拍照连拍 |
| 3 | 🔁 中继 | `relay` | 中继状态 / 已连设备 |
| 4 | ⚙️ 配置 | `all` | 本机热点 / 上游 WiFi / 门户网址 / 指示灯 / 模式与设备 / 接口手册 |

- 原「侦听」改名为**「无人机监控」**，并把原来的「侦听」「模拟」两个平级入口收进去做二级分组；
- **「配置」放在最后且 `data-mode="all"`**，任何模式都可访问，是公用模块；
- 顶部模式切换按钮文案同步改为「🚁 无人机监控 / ⌨️ 键盘 / 🔁 中继」。

### 11.2 前端改为三层导航

```js
var TOP_HOME   = {drone:"listen-home", kb:"kb-link", net:"net-relay", set:"set-ap"};
var GROUP_HOME = {listen:"listen-home", sim:"sim-basic"};
var MODE_TOP   = {rid:"drone", camera:"kb", relay:"net", all:"set"};
function switchTop(t)  { switchTab(TOP_HOME[t]); }
function switchGroup(g){ switchTab(GROUP_HOME[g] || g); }
```

- `PAGES` 每条新增 `top` 字段，`switchTab()` 依次刷新一级 / 二级 / 三级高亮；
- 只有 `drone` 顶层存在真正的第三级（`listen` / `sim` 两个子页面条），其余顶层二级即页面；
- `applyModeNav()` 现在同时按 `data-mode` 隐藏一级、二级、三级导航，并返回当前模式的落地页 id，`bootRoute()` 直接用它决定首屏。

### 11.3 门户重定向地址可编辑并落盘

之前「门户网址」只写浏览器 localStorage，设备侧固定 302 到 `http://192.168.4.1/`，等于不能改。现在：

- NVS 新增 `crid_nvs_save_portal()` / `crid_nvs_load_portal()`（key `portal_url`，命名空间 `crid_cache`）；
- 新增接口 `GET /api/portal`（返回 `url`、`def`）与 `POST /api/portal`（`{"url":"..."}`，留空即恢复默认）；
- `http_404_error_handler()` 的 302 `Location` 改为读取该值，默认地址跟随 AP 网关实际 IP；
- 只接受 `http://` / `https://` 开头，其余直接拒绝，避免把客户端弹到危险协议；
- 前端「配置 → 门户网址」页面改为异步读写设备，带「保存并生效 / 恢复默认 / 新标签打开验证」。

### 11.4 顺手修掉的上轮遗留

`/api/relay_clients` 的 IP 列一直显示 `--`：原实现仅靠 `esp_netif_dhcps_get_clients_by_mac()` 反查，STA 列表和 DHCP 租约表顺序/条目对不上就查不到。改为在 `crid_relay_clients_init()`（app_main 里 AP 网口创建后立即调用）注册 `IP_EVENT_AP_STAIPASSIGNED`，分配瞬间记下 MAC→IP，查询时优先用这张表，查不到再回落 DHCP 租约表，并在每次查询时清掉已断开的条目。实测已能正确显示 `192.168.4.2`。

### 11.5 本轮验证

- jsdom 离线导航测试 `nav3.js`：rid / camera / relay 三种模式下顶层可见性、二级、三级高亮全部正确；
- jsdom 全页面遍历 `nav4.js`：18 个页面逐个 `switchTab()`，页面 / 一级 / 二级 / 三级高亮一致，无运行时错误；
- 硬件实测 `verify2.py`：三种模式轮转后，HTML 结构、各模式接口、`GET/POST /api/portal` 持久化、非法协议拒绝、302 Location 跟随自定义地址，全部通过。

---

## 12. ABROBOT 板载 OLED + BOOT 按键适配（2026-10-29 第三轮）

### 12.1 硬件映射（ABROBOT ESP32-C3）

| 外设 | 引脚 / 地址 | 说明 |
|---|---|---|
| OLED (SSD1306) | SDA=GPIO5, SCL=GPIO6, I2C 地址 0x3C | 面板有效分辨率 72×40 |
| BOOT 按键 | GPIO9 | 低电平有效，内部上拉 |
| 板载 LED | GPIO8 | 与项目 `CONFIG_CRID_LED_GPIO` 一致 |

**关键坑**：面板是 72×40，但挂在 SSD1306 的 128×64 GDDRAM 上，可见窗口起点为
**(30, 12)**。U8g2 之类的库要求按 128×64 配置，再把绘制限制在 72×40 内，否则
内容会被裁切。本项目直接自己写驱动，用列地址窗口 `[30, 101]` + 页地址 `[1, 6]`
实现偏移，避开这个坑。

### 12.2 新增模块

| 文件 | 职责 |
|---|---|
| `main/crid_oled.c/.h` | SSD1306 I2C 驱动 + 5×7 字模 + 帧缓冲；`crid_oled_dump()` 导出 ASCII 图 |
| `main/crid_button.c/.h` | BOOT 按键消抖与手势识别（单击 / 双击 / 长按 / 超长按） |
| `main/crid_screen.c/.h` | 模式感知的屏幕编排 + 按键响应 + 刷新任务 |

### 12.3 按键手势（三种模式下语义不同）

| 手势 | rid 无人机监控 | camera 键盘 | relay 中继 |
|---|---|---|---|
| 单击 | 模式内翻页（3 页） | 模式内翻页（2 页） | 模式内翻页（2 页） |
| 双击 | 清空无人机列表 | 触发一次快门 | 重连上游 WiFi |
| 长按 | 切到下一模式（重启生效） | 同左 | 同左 |
| 超长按 | 屏幕开关 | 同左 | 同左 |

### 12.4 屏幕内容

- **rid**：`RID MONITOR` / `UAV 在线/总数` / 最近目标 RSSI 或 `NO SIGNAL` / 时间
- **camera**：`BLE KEYBD`（连接态、设备名、快门计数）/ `BURST`（连拍状态、间隔、按键映射）
- **relay**：`WIFI RELAY`（上游 SSID、信号强度、客户端数与 NAPT 状态）/ `CLIENTS`（设备列表）

底部状态行在「模式信息」与「当前时间」之间每 2.2 秒轮播 —— 72 px 宽只放得下
12 个字符，两者都重要就只能分时显示。

### 12.5 本轮修掉的两个 bug

1. **`crid_scr` 任务栈溢出导致反复重启**
   现象：OLED 初始化成功、按键初始化成功后立刻
   `Guru Meditation Error: Core 0 panic'ed (Stack protection fault)`，
   随后 `RTC_SW_CPU_RST` 循环，表现为空中搜得到 `rid` 热点但连不上、HTTP 全超时。
   根因：`draw_relay()` 在 3072 字节的任务栈上放了 512 字节 JSON 缓冲，还要跑
   `snprintf` + `cJSON_Parse`。
   修复：改为直接读结构化状态（新增 `crid_relay_sta_info()` /
   `crid_relay_client_list()`），完全绕开 JSON；任务栈提到 4096。

2. **页数不一致**：`crid_screen_get_page_count()` 硬编码返回 3，但 camera/relay
   实际只有 2 页，翻到第 3 页会重复第 1 页。改为按当前模式返回
   `PAGES_RID / PAGES_CAMERA / PAGES_RELAY`。

### 12.6 新增接口

| 接口 | 方法 | 说明 |
|---|---|---|
| `/api/oled` | GET | 屏参：ready / on / 偏移 / 当前页 / 总页数 |
| `/api/oled` | POST | `{"page":N}` 翻页；`{"x_offset":N,"y_offset":N}` 现场校准偏移；`{"test":1\|2}` 自检图案 |
| `/api/oled_dump` | GET | 导出 72×40 显存 ASCII 图（排障用，免摄像头） |

**排障经验**：验证屏幕内容优先用 `/api/oled_dump`（像素级精确，还能看到可见窗口
之外被裁掉的内容），比架摄像头拍照可靠得多 —— 拍照只能证明"亮了"，dump 能证明
"画对了"。配套解码脚本 `ridtest/decode_oled.py` 会用固件的同一张字模表把像素
还原成文本。

---

## 13. 双板兼容 + BLE 键盘 iOS 修复（2026-10-30 第四轮）

### 13.1 无屏幕硬件兼容

同一份固件要同时跑"带 0.42 OLED"和"不带屏"两种板子。`crid_oled_init()` 本来就会
在 I2C 上探测 0x3C，探测不到直接返回错误；这轮把上层也理顺了：

- `crid_screen_init()` 探测失败 → 置 `s_oled_ok=false`，**不建按键、不建刷新任务**，
  并向调用方返回 `ESP_OK`（没屏幕不是错误，只是能力缺失）。
- `screen_task` 里的绘制本来就带 `crid_oled_ready()` 判断，无屏时一次 I2C 都不发。
- `/api/oled` 新增 `present` 字段；Web「屏幕」页据此显示"本板未检测到屏幕"。
- 实测（COM6 无屏板）：
  `W OLED: no SSD1306 at 0x3C (ESP_ERR_NOT_FOUND)` →
  `W SCR: no OLED on SDA=5 SCL=6 addr=0x3C -> screen & button disabled`，
  之后 RID/键盘/中继三种模式都正常，堆 88 KB 以上。

### 13.2 蓝牙配对码上屏幕

以前配对码只出现在 Web 页面，手机配对时人不在电脑前就配不上。现在：

- `shutter_status_t` 增加 `pairing` 标志；`BLE_SM_IOACT_DISP` 时置位，
  加密完成 / 断开 / 120 s 超时自动清除。新增 `crid_shutter_is_pairing()`。
- 屏幕侧新增 `draw_pairing()`：只要处于配对态，**压过菜单和实时页**，全屏放大显示
  6 位配对码（scale=3，每位 9×15 px，6 位正好铺满 72 px），底下提示 `KEY IN PHONE`。
- 新增 `big_digits()`：按字符串逐位画，保证前导 0 不会被 `%d` 吃掉。
- Web 端同步加了一条高亮横幅（`/api/photo_status` 新增 `pairing` 字段）。

### 13.3 HID 报告格式重做（修 iOS 打不出字）

**根因**：描述符里声明了 Report ID 1（消费者）/ 2（键盘），但通知负载没带 ID 前缀。
两家主机对"ID 该不该出现在负载首字节"的要求是互斥的：

| 主机 | 期望 |
|---|---|
| Windows | 走 HOGP：ID 在 Report Reference 描述符里，负载不带前缀（实测带前缀就完全打不出字） |
| iOS | 走 USB 老规矩：描述符里有 ID 时，负载首字节必须是 Report ID |

改哪边都会打断另一边，所以**直接取消 Report ID**：单条 9 字节合并报告，
键盘在前、消费者控制在后，主机解析不存在歧义。

```
[0]     修饰键  [1] 保留  [2..7] 6 个键码  [8] 消费者控制(bit0 VOL+ / bit1 VOL-)
```

顺带改动：

- `appearance` 0x03C0(HID Generic) → **0x03C1(Keyboard)**，iOS 按 appearance +
  报告描述符认设备类别，报成 Generic 时会丢弃键盘输入。
- 新增 **Battery Service (0x180F / 0x2A19)**，HOGP 强制要求，iOS 也会读。
- 两条 Report 特征合并成一条，Report Reference 描述符填 `{0x00, 0x01}`
  （0 = 不使用 Report ID），`s_report_handle == s_kb_report_handle`。

> **升级后必须先在手机/电脑蓝牙里「忽略此设备」再重新配对** —— GATT/描述符缓存
> 变了，系统仍按旧缓存解析会继续打不出字。Web「配对管理」卡片已写明。

### 13.4 USB 串口命令行（调试用）

主控制台从 UART0 切到 **USB Serial/JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），
新增 `crid_cli.c`（CMake 里 `REQUIRES` 加 `console`）。两个板子各占一个 COM 口，
不用切 WiFi 就能分别下命令：

```
stat                       模式/堆/屏幕/快门/配对一屏看完
mode rid|camera|relay      切模式
shot / gap [n|next]        拍照 / 间隔档位
type <text>                键盘输入
pair                       看当前配对码
clearbonds                 清配对
oled on|off|test|clear|dump|offset x y|contrast n
scr page <n>|menu <n>
reboot
```

配套脚本 `.workbuddy/ridtest/cli.py`（`python cli.py COM5 stat`、
`python cli.py COM5 --boot 8` 抓开机日志）。

### 13.5 本轮验证记录

| 项目 | 结果 |
|---|---|
| 无屏板（COM6）三模式运行 | ✅ 屏幕/按键自动停用，无重启无告警刷屏 |
| 带屏板（COM5）屏幕+按键 | ✅ `SSD1306 ready: 72x40 @0x3C (28,24)`、`button on GPIO9` |
| 串口命令行 | ✅ 两块板 `stat` 均返回；无屏板 `oled: ABSENT` |
| 配对码页面排版 | ✅ 用固件同一份字模离线渲染（`ridtest/render_pair.py`），6 位大号数字正好 72 px，底部提示行不越界 |
| GATT 服务枚举 | ✅ 电池服务 0x180F 可见；0x1812 被 Windows HID 驱动接管，普通 GATT 扫描看不到（正常） |
| iOS 实际输入 | ⏳ **待用户在手机上验证**（PC 侧无 iOS，且 bleak 在 Windows 上无法完成交互式配对） |

---

*本文件为交接文档，随代码一起维护；修改结构性内容后请同步更新第 3、6、7 节。*
*（内容由AI生成，仅供参考）*

---

## 14. OLED 导航重构（2026-10-30 第五轮 · 任务书驱动）

### 14.1 与任务书的冲突（按底层实现为准）
任务书 §8.2 要求"离开中继页去 RID 页但中继不停"。本固件 RID/SHOT/RELAY 是**三互斥固件模式**
（ESP32-C3 单射频，RID 嗅探 / BLE 键盘 / Wi-Fi 中继不能并发），切模式走 `crid_mode_request`
→ 重启。**已与用户确认：该冲突按下层（硬件现实）为准**——同一时刻只处一个模式，进另一模式=
主页双击→确认→重启。所以"离开中继展示页→主页"中继照常跑 ✅；"去 RID 页但中继不停"做不到（硬件限制）。

### 14.2 本次改动
- **重做导航模型**（`main/crid_screen.c` 整体重写状态机）：
  `UI_HOME`（三功能启动器 RID/SHOT/RELAY）↔ `UI_FUNC`（模式根页）↔ `UI_DETAIL`（RID 详情）↔
  `UI_SET_GAP`（间隔设置）↔ `UI_CONFIRM`（切模式确认）。
  - 统一：单击=选择/翻页/执行，双击=进入/确认/开设置，长按=返回上一级（HOME 长按无操作），
    超长按=仅切屏幕亮灭。删除"三击作为返回"（三击仅留作逃生键一键回 HOME）。
  - 熄屏后首次任意按键**只唤醒并消费**，不误触拍照/翻页（任务书 §4.3）。
- **RID 列表页**（`draw_rid_list`）：序号+尾号(前4)+RSSI，3 行窗口以选中项为中心；选择跨刷新保留
  （设备消失后夹到有效项）。双击进详情，长按回 HOME。
- **RID 详情页**（`draw_rid_detail`）：标题=序号+尾号，右上方 RSSI；三行 SPD/ALT/HDG。无数据 `RSSI --`。
- **拍照页**（`draw_camera`）：手动 `GAP MANUAL/1X=SHOT/NO LINK/SHOTS`；定时运行大号倒计时；
  **新增暂停态 `PAUSED`**。
- **间隔设置页**（`draw_set_gap`）：档位严格 `MANUAL/3/5/10/15/20/30/60`。
- **中继页**（`draw_relay`）：3 页自动轮播（连接信息 / 累计流量 / 实时速率），SSID 中文用 `HIDDEN`。
- **快门暂停状态**（`crid_shutter.c`）：新增 `crid_shutter_pause/resume/stop/is_paused` 与
  `status.paused`；节拍在 `shutter_task`（`s_next_at`）内，resume 重排避免补拍。
- 采集/绘图辅助、`collect_uav`、`draw_pairing`、`fmt_*`、`big_*` 全部保留复用。
- 对外接口签名不变（`crid_screen_get/set_*`、`crid_screen_init/is_on/set_on`）；`set_ui` 的数字映射改为
  0 HOME/1 FUNC/2 DETAIL/3 SET_GAP/4 CONFIRM（Web 调试用）。

### 14.3 验证
- `python tools/idfrun.py build` 通过（0x163f50）；已烧 COM5（带屏板）。
- 串口 `oled dump` 逐一核对：HOME、RID LIST（无目标 `NO UAV`）、拍照页（`GAP MANUAL/NO LINK/SHOTS`）、
  中继页（`STA/DEV/NAT`、累计、速率）、配对码页排版均正确，无越界/花屏/栈溢出。
- 注：早期用 HTTP `/api/oled_dump` 抓帧偶发只拿到部分帧（端点偶发超时），改用串口 `oled dump` 稳定；
  非固件 bug。
- iOS 实际键盘输入、物理按键导航仍需**实机/用户验证**（PC 侧无 iOS，且 bleak 在 Windows 做不了交互配对）。
