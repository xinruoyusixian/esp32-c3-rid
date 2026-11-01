---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: 9ecf476f186a4e61c8d9e63e76c2d774_7803f98fd28111f196c5525400248c00
    ReservedCode1: VTlVUlubOSSGVaXbqAPM65Ewc5udVNFtftiV0qLhk1nF2qO72GNoHbbBdfspZ0a2Ki6ZJys7ilDxFGuyJ2s0dGvmx+/2sThmyU/4OOIn7O06OhgH+DbWyEXw+W4CZzbsd2S9n3x6EgPeZf3nPjXLdMEJJVlRSk0T8U9uWRD9aWbWWCfuyUdchPOZXpU=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: 9ecf476f186a4e61c8d9e63e76c2d774_7803f98fd28111f196c5525400248c00
    ReservedCode2: VTlVUlubOSSGVaXbqAPM65Ewc5udVNFtftiV0qLhk1nF2qO72GNoHbbBdfspZ0a2Ki6ZJys7ilDxFGuyJ2s0dGvmx+/2sThmyU/4OOIn7O06OhgH+DbWyEXw+W4CZzbsd2S9n3x6EgPeZf3nPjXLdMEJJVlRSk0T8U9uWRD9aWbWWCfuyUdchPOZXpU=
---

# CRID 外部接入 API（/api/ext/*）

适用于 ESP32-C3 RID 固件（IDF v5.2.1）。所有 `/api/ext/*` 接口**同时支持 GET 与 POST**：

- GET：参数放 URL 查询串，值需要 URL 编码（固件已做百分号解码，`+` 视为空格）。
- POST：`Content-Type: application/json`，参数放 JSON body；也接受查询串与 JSON 混合（JSON 优先）。

响应统一为 JSON。错误响应：`{"ok":false,"err":"..."}`，并带对应 HTTP 状态码（400/401/500）。

---

## 0. 前置条件

| 项 | 说明 |
|---|---|
| 访问地址 | AP 模式固定 `192.168.4.1`（SSID `rid` / 密码 `12345678`）；STA 连上路由器后同时可用路由器分配的 IP |
| 设备模式 | 查询设备列表需要设备处于 **RID 模式**（tracker 已初始化）。camera 模式或未开扫描时，`uavs` 会返回 `hint: tracker not ready` |
| 缓存容量 | 追踪表最多 16 台设备在线；新设备事件队列 16 条；单次查询 `limit` 上限 16 |
| 内存 | RID 模式稳态空闲堆约 86–115KB，单次快照走堆分配并分块发送，不会长期占用 |

---

## 1. 状态总览 `GET|POST /api/ext/status`

无参数。用于外部服务健康检查与内存/队列可观测。

```json
{
  "ok": true,
  "uptime_ms": 109781,
  "free_heap_kb": 86,
  "online": 0,
  "cached": 0,
  "new_seq": 0,
  "sta": { "enable": false, "ssid": "NoSuchAP_Test", "connected": false,
           "ip": "0.0.0.0", "rssi": 0, "channel": 0, "ap_ip": "192.168.4.1" },
  "push": { "enable": false, "url": "http://192.168.4.2:8080/api/uav",
            "method": "POST", "queue_free": 16 },
  "stat": { "ok": 0, "fail": 0, "drop": 0, "last_code": 0, "last_ms": 0, "last_err": "" },
  "token_enabled": false
}
```

| 字段 | 含义 |
|---|---|
| `free_heap_kb` | 当前空闲堆（KB），低于 40KB 建议调用 `/api/ext/clear` 或关闭无用功能 |
| `online` / `cached` | 在线设备数 / 追踪表内设备数 |
| `new_seq` | 新设备事件游标（客户端增量拉取的 `since` 基准） |
| `stat` | 主动推送的累计成功/失败/丢弃次数与最后一次 HTTP 结果 |

---

## 2. STA（网络模式）`GET|POST /api/ext/sta`

设备默认 **AP only**（内存紧张时最稳）。配置 STA 后切换为 APSTA，AP 热点保留，设备同时作为客户端连路由器。

| 参数 | 类型 | 说明 |
|---|---|---|
| `enable` | bool / 1\|0 | 是否启用 STA |
| `ssid` | string | 路由器 SSID（≤32） |
| `password` / `pwd` | string | 路由器密码（≤64） |
| `api_token` | string | 可选，设置后所有 `/api/ext/*` 需带 token（见第 7 节） |

```bash
curl "http://192.168.4.1/api/ext/sta?enable=1&ssid=MyAP&password=12345678"
curl -X POST -H "Content-Type: application/json" \
  -d '{"enable":true,"ssid":"MyAP","password":"12345678"}' \
  http://192.168.4.1/api/ext/sta
```

不传 `enable`/`ssid` 即为**纯查询**，不会修改配置：

```json
{ "ok": true, "enable": true, "ssid": "MyAP", "connected": true, "ip": "192.168.1.57",
  "rssi": -58, "channel": 6, "connects": 1, "disconnects": 8,
  "ap_ssid": "rid", "ap_ip": "192.168.4.1" }
```

注意：APSTA 时 AP 信道会跟随 STA 信道；STA 断线会自动重连（`connects`/`disconnects` 可观测）。

---

## 3. 拉取设备列表（增量）`GET|POST /api/ext/uavs`

别名：`/api/ext/devices`、`/api/ext/new_devices`（行为完全相同）。

| 参数 | 默认 | 说明 |
|---|---|---|
| `since` | 0 | 增量游标。只返回**新设备事件序号 > since** 的设备；首次对接传 0 |
| `all` | false | `all=1` 返回当前追踪表内全部在线设备（忽略 since） |
| `limit` | 16 | 本次最多返回条数，上限 16 |

请求：

```bash
curl "http://192.168.4.1/api/ext/uavs?since=0&limit=8"
curl "http://192.168.4.1/api/ext/uavs?all=1"
```

响应：

```json
{
  "ok": true,
  "list": [
    {
      "mac": "AA:BB:CC:11:22:33", "seq": 5, "online": true,
      "rssi": -62, "channel": 6,
      "id": "1596F3A2C1D4000001", "id_type": "SerialNumber", "ua_type": 2,
      "latitude": 39.9042, "longitude": 116.4074,
      "altitude_msl": 120.5, "altitude_agl": 60.0,
      "speed_h": 8.5, "speed_v": 0.0, "heading": 245.0,
      "status": 2, "status_text": "Airborne",
      "operator_id": "OP-0001", "operator_lat": 39.9031, "operator_lon": 116.4062,
      "protocol_name": "ASTM F3411 (WiFi Beacon)", 
      "first_seen": 128430, "last_seen": 190233
    }
  ],
  "seq": 5,            // 本次消费到的游标，下次原样作为 since 回传
  "latest_seq": 7,     // 设备侧最新事件序号
  "count": 1,
  "missed": 2          // since 之后产生但已超时回收、无法再取的事件数
}
```

- `seq` 语义：**新设备事件单调递增序号**，设备清空/重启不会回退（重启后从 0 重新计数，建议每次重连先读 `status.new_seq` 校准）。
- `missed > 0` 表示客户端拉取间隔太长，部分设备已超时回收；此时可用 `all=1` 全量对齐。
- 无法定位设备时（未开扫描/非 RID 模式）返回：`{"ok":true,"list":[],...,"hint":"tracker not ready (scan/sim off)"}`。

---

## 4. 清空设备（释放内存）`GET|POST /api/ext/clear`

无参数（可带 `?confirm=1`，可选）。清空追踪表与事件队列，`seq` 不回退。

```bash
curl -X POST http://192.168.4.1/api/ext/clear
```

```json
{ "ok": true, "cleared": 6, "heap_before_kb": 84, "heap_after_kb": 92,
  "freed_kb": 8.2, "online": 0, "cached": 0 }
```

说明：追踪表本身是预分配结构，`clear` 释放的是表内动态字段与事件队列；`freed_kb` 为实际回收量，设备多时更明显。不会清空 WiFi/推送配置。

---

## 5. 主动推送配置 `GET|POST /api/ext/push_config`

配置设备扫描到**新设备**后主动回调外部服务。

| 参数 | 说明 |
|---|---|
| `url` | 回调地址，必须以 `http://` 或 `https://` 开头（≤192 字符，GET 方式传需 URL 编码） |
| `method` | `GET` 或 `POST`（默认 POST）。GET 时设备参数拼在查询串；POST 时发 JSON body |
| `enable` | bool，开启后自动启动推送任务（16 深队列） |

```bash
curl "http://192.168.4.1/api/ext/push_config?url=http%3A%2F%2F192.168.4.2%3A8080%2Fapi%2Fuav&method=POST&enable=1"
```

```json
{ "ok": true, "enable": true, "url": "http://192.168.4.2:8080/api/uav",
  "method": "POST", "token_enabled": false,
  "stat": { "ok": 3, "fail": 0, "drop": 0, "last_code": 200, "last_ms": 248570, "last_err": "" } }
```

只传 `url` 或只传 `method` 会做局部更新，不传则为查询。

### 推送报文（POST）

新设备（`event = new_uav`）：

```json
{
  "event": "new_uav",
  "seq": 5,
  "uptime_ms": 248490,
  "free_heap_kb": 96,
  "device": { "mac": "AA:BB:CC:11:22:33", "seq": 5, "online": true,
              "id": "1596F3A2C1D4000001", "latitude": 39.9042, "longitude": 116.4074,
              "altitude_msl": 120.5, "altitude_agl": 60.0, "speed_h": 8.5,
              "heading": 245.0, "status": 2, "rssi": -62, "channel": 6,
              "first_seen": 128430, "last_seen": 190233 }
}
```

字段与 `/api/ext/uavs` 的 list 元素一致。

### GET 方式推送的查询串

```
?event=new_uav&seq=5&mac=AA:BB:CC:11:22:33&id=1596F3A2C1D4000001
&lat=39.9042000&lon=116.4074000&alt_msl=120.5&alt_agl=60.0
&speed_h=8.5&speed_v=0.0&heading=245.0&rssi=-62&status=2
&op_id=OP-0001&first_seen=128430&last_seen=190233
```

### 推送可靠性

- 新设备入队后延迟 `PUSH_SETTLE_MS` 再取快照，避免只收到半条报文（无 BasicID/Location 时不会推空数据）。
- 队列 16 深，满则丢弃并累加 `stat.drop`。
- 单次超时 8s，失败累加 `stat.fail` 并记录 `last_err`；不做重试，需外部服务幂等并配合 `/api/ext/uavs` 兜底补齐。
- 设备在延迟窗口内消失（超时回收）则跳过该条推送。

---

## 6. 联调自测 `GET|POST /api/ext/test_push`

立刻按当前推送配置发一条 `event=sync` 的报文（内容是当前追踪表快照，可能为空），用于验证网络与回调地址是否可达。

```bash
curl "http://192.168.4.1/api/ext/test_push"
```

```json
{ "ok": true, "sent": 0, "http_code": 200, "url": "http://192.168.4.2:8080/api/uav",
  "method": "POST", "err": "", "stat": { "ok": 1, "fail": 0, "drop": 0, "last_code": 200 } }
```

`http_code` 为对端返回码；`999`/`-1` 之类表示连接失败（如本机未监听、URL 写错）。

---

## 7. 鉴权（可选）

设置 `api_token`（通过 `/api/ext/sta`）后，所有 `/api/ext/*` 需带：

- GET：`?token=xxxx`
- POST：请求头 `X-API-Token: xxxx`

未带或错误返回 `401 {"ok":false,"err":"invalid token"}`。`status.token_enabled` 可用于确认是否已启用。

---

## 8. 典型对接流程

### A. 外部服务定时轮询（拉模式）

```python
import requests, json

BASE = "http://192.168.4.1"
cursor = 0

def poll():
    global cursor
    r = requests.get(f"{BASE}/api/ext/uavs", params={"since": cursor, "limit": 16}, timeout=5)
    d = r.json()
    for dev in d.get("list", []):
        handle(dev)              # 入库/告警
    cursor = d.get("seq", cursor)
    if d.get("missed"):
        full = requests.get(f"{BASE}/api/ext/uavs", params={"all": 1}, timeout=5).json()
        sync(full["list"])       # 游标落后，做一次全量对齐
    return d
```

建议：轮询周期 1–5s；设备重启后 `cursor` 归零，先读 `/api/ext/status` 的 `new_seq` 校准，若 `cursor > new_seq` 则重置为 0。

### B. 设备主动推送（推模式）

```python
from http.server import BaseHTTPRequestHandler, HTTPServer
import json

class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        data = json.loads(self.rfile.read(n) or b"{}")
        if data.get("event") == "new_uav":
            print("新设备:", data["device"]["id"], data["device"]["latitude"], data["device"]["longitude"])
        self.send_response(200); self.end_headers(); self.wfile.write(b'{"ok":true}')
    def do_GET(self):  # method=GET 模式
        from urllib.parse import urlparse, parse_qs
        q = parse_qs(urlparse(self.path).query)
        print("新设备(GET):", q.get("id"), q.get("lat"), q.get("lon"))
        self.send_response(200); self.end_headers(); self.wfile.write(b'{"ok":true}')
    def log_message(self, *a): pass

HTTPServer(("0.0.0.0", 8080), H).serve_forever()
```

配好回调后：

```bash
curl "http://192.168.4.1/api/ext/push_config?url=http%3A%2F%2F192.168.4.2%3A8080%2Fapi%2Fuav&method=POST&enable=1"
curl "http://192.168.4.1/api/ext/test_push"     # 先自测通路
```

> 注意：AP 模式下外部服务器的 IP 是连在该热点上的地址（PC 通常是 `192.168.4.2`）。设备走 STA 后，回调地址应改成能被路由器网段访问的地址。

---

## 9. 接口速查

| 接口 | 方法 | 用途 |
|---|---|---|
| `/api/ext/status` | GET/POST | 运行状态、内存、STA/推送状态 |
| `/api/ext/sta` | GET/POST | 配置/查询 STA 网络、设置 api_token |
| `/api/ext/uavs`（`devices`/`new_devices`） | GET/POST | 增量或全量拉取设备 |
| `/api/ext/clear` | GET/POST | 清空设备与事件队列，释放内存 |
| `/api/ext/push_config` | GET/POST | 配置主动推送地址/方法/开关 |
| `/api/ext/test_push` | GET/POST | 立即发一条测试推送 |
*（内容由AI生成，仅供参考）*
