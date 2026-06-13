# ESP32-S3 USB Print Bridge for HP LaserJet M1136

把一台只有 USB 接口的 **HP LaserJet M1136 / M1132** 变成 **Wi-Fi 网络打印机**。

ESP32-S3 作为 USB Host 接打印机，并在 Wi-Fi 上开 TCP **:9100 (HP JetDirect / AppSocket)** 端口，把字节流原样转发到打印机的 USB Bulk OUT 端点。

```
┌──────────┐  TCP:9100  ┌──────────────┐  USB Bulk OUT  ┌──────────────┐
│  macOS   │ ─────────► │   ESP32-S3   │ ─────────────► │  HP M1136    │
│ (driver) │            │ (this device)│                │  (printer)   │
└──────────┘            └──────────────┘                └──────────────┘
        ZJStream/PJL              透明字节管道                    出纸
```

---

## ⚠️ 重要前置条件：主机驱动必须能产 ZJStream

HP M1136 是 **主机端渲染（host-based / GDI）** 打印机，USB 端口**只接受 ZJStream**，不认 PostScript / PCL / PWG-Raster。

因此**主机侧必须装到能输出 PJL+ZJStream 的驱动**，否则 ESP32 这一侧再完美也不会出纸。

### 在 macOS 上验证驱动是否正确（必做）

1. 临时本地抓包：
   ```bash
   nc -l 9101 > /tmp/job.bin
   ```
2. 系统设置 → 打印机 → 添加：
   - 协议：**HP JetDirect — Socket**
   - 地址：`127.0.0.1`，端口：`9101`
   - 驱动：**"HP LaserJet Professional M1130 MFP Series"**（M1132/M1136 都用这个 PPD）
3. 打一张测试页，按 Ctrl-C 停止 `nc`。
4. 检查头部：
   ```bash
   xxd /tmp/job.bin | head -10
   ```

期望看到：
```
1B 25 2D 31 32 33 34 35 58       ← HP UEL
@PJL ... ENTER LANGUAGE=ZJS      ← PJL 序言切到 ZJStream
JZJZ ...                         ← ZJStream magic
```

如果头部是 `%!PS-Adobe`、`52 61 53 32` (RaS2，PWG-Raster) 或别的——**你装的不是正确的 ZJStream 驱动**，本项目对你不工作，需要在主机/中转设备上换驱动后再继续。

---

## 硬件

| 项 | 说明 |
|---|---|
| **主控** | ESP32-S3-N16R8（16MB Flash + 8MB Octal PSRAM，双 USB） |
| **USB-OTG 口（PHY0）** | 接打印机的 USB-B，做 USB Host |
| **USB-Serial-JTAG 口（PHY1）** | 接电脑，做日志/烧录 |
| **打印机** | HP LaserJet M1136 / M1132（USB Printer Class 0x07, subclass 0x01） |

注意事项：
- ESP32-S3 OTG 口需要 **USB-A 母转接线**接打印机的 USB-B 线，板上 VBUS 由打印机/电源 hub 供电，**ESP32 自己不能给 5V**。
- 烧录时插 USB-Serial-JTAG 口，运行时两个口可同时使用。
- 工作期间 OTG 口由打印机一直占用。

---

## 软件栈

- **Framework**：PlatformIO + `arduino + espidf`（Arduino 作为 ESP-IDF 组件）
- **ESP-IDF**：4.4.7
- **Arduino-ESP32**：3.20017
- **USB Host**：`usb_host_*`（ESP-IDF 原生 API）
- **Wi-Fi 协议**：HP JetDirect / AppSocket（TCP:9100）
- **服务发现**：mDNS（`_pdl-datastream._tcp` + `_printer._tcp` + `_http._tcp`）
- **状态页**：HTTP :80

---

## 目录结构

```
.
├── CMakeLists.txt              # IDF 顶层 (arduino-as-component)
├── platformio.ini              # 板子/USB/PSRAM/分区配置
├── sdkconfig.defaults          # USB Host / PSRAM / FreeRTOS / 日志路由
├── sdkconfig.esp32-s3-n16r8    # 由 PlatformIO 生成（首次 build 后产生）
├── huge_app.csv                # 16MB Flash 分区表（6.25MB app + 9.5MB SPIFFS）
├── include/
│   ├── secrets.example.h       # Wi-Fi/mDNS 模板（提交到 git）
│   └── secrets.h               # 你的实际凭据（**不提交**，已在 .gitignore）
├── src/
│   ├── CMakeLists.txt          # IDF component register
│   └── main.cpp                # 所有逻辑（~1100 行单文件）
└── lib/, test/                 # 未使用
```

---

## 关键代码模块（`src/main.cpp`）

| 模块 | 说明 |
|---|---|
| `usbHostLibTask` | 跑 USB Host 库事件循环 |
| `usbClientTask` + `clientEventCb` | 处理 NEW_DEV / DEV_GONE，延迟调用 `usbTryAttachDevice` |
| `descFindPrinterInterface` | 在 config descriptor 里找 Printer Class 接口 + bulk OUT/IN endpoint |
| `usbTryAttachDevice` | 打开设备、claim 接口、读 Device ID、读 Port Status、预分配 OUT transfer |
| `usbCtrlSync` | 同步控制传输，内部 pump `handle_events` 避免死锁 |
| `usbWriterTask` | 从 stream buffer 取数据，1024B 一块送 bulk OUT（持久化 transfer） |
| `usbReaderTask` | **已禁用**（Bulk IN 在 ESP-IDF v4.4 下不稳定） |
| `rawServerTask` | TCP:9100 接收 → stream buffer → USB OUT；作业结束后注入合成 PJL 状态 |
| `handleRoot` / `handleReset` / `handleJobs` | HTTP 状态页 / 软复位 / 任务历史 |
| `startWifi` / `startMdns` / `startUsbHost` | 启动各子系统 |
| `setup` / `loop` | 入口（loop 仅跑 HTTP server） |

**任务/核心分配：**
- Core 0：USB lib（pri 5）、USB client（pri 4）、USB writer（pri 4）、USB reader（pri 3，已禁用）
- Core 1：raw9100 server（pri 3）、Arduino loop（HTTP server，pri 1）

**数据流缓冲：**
- 256 KB **StreamBuffer**，存储区在 PSRAM（`MALLOC_CAP_SPIRAM`）
- TCP → StreamBuffer → 1024B 块 → bulk OUT

---

## 快速开始

### 1. 准备 Wi-Fi 凭据

```bash
cp include/secrets.example.h include/secrets.h
# 编辑 include/secrets.h 填入 SSID/密码/mDNS 主机名
```

### 2. 编译并烧录

```bash
# 一次性安装
pio platform install espressif32

# 烧录（USB-Serial-JTAG 口接电脑）
pio run -t upload

# 看日志
pio device monitor
```

### 3. 接打印机

把 ESP32-S3 的 **USB-OTG 口** 通过 OTG 线接到 M1136 的 USB-B 口，打印机开机。看日志应有：

```
[usb-cli] NEW_DEV addr=1
[usb-cli] device VID=0x03F0 PID=0x???? ...
[usb-cli]  intf #X alt=0 class=0x07 sub=0x01 proto=0x02 eps=2
[usb-cli]   ep 0x?? (OUT) attr=0x02 mps=64
[usb-cli]   ep 0x?? (IN)  attr=0x02 mps=64
[usb-cli] claiming if=X epOut=0x?? mps=64 epIn=0x?? mps=64
[usb-cli] Device ID: MFG:Hewlett-Packard;CMD:ZJS;MDL:HP LaserJet ...
[usb-cli] printer attached and ready
```

### 4. 在 macOS 添加打印机

- 系统设置 → 打印机 → `+`
- **IP** 标签页
- 协议：**HP JetDirect — Socket**
- 地址：`esp32-printer.local`（或日志里打印的 IP）
- 驱动："**HP LaserJet Professional M1130 MFP Series**"
- 名称随意

### 5. 打印

随便打一张文档。日志应出现：

```
[raw9100] connection from 192.168.x.x
[raw9100] connection closed, job bytes=NNNNNN total in=X out=Y
```

`Bytes out` 应当等于 `Bytes in`。打印机吐纸。

### 6. 看状态页

浏览器打开 `http://esp32-printer.local/` 看：
- Wi-Fi 状态、MAC、IP
- 打印机连接 / Device ID / Port Status
- 累计字节 / 当前任务字节
- 内存占用
- `/reset` 软复位打印机端口

---

## 配置参考

### `platformio.ini` 要点

| 选项 | 值 | 说明 |
|---|---|---|
| `board` | `esp32-s3-devkitc-1` | 标准 S3 DevKitC |
| `board_upload.flash_size` | `16MB` | 适配 N16R8 |
| `board_build.partitions` | `huge_app.csv` | 自定义 6.25MB app + 9.5MB SPIFFS |
| `board_build.arduino.memory_type` | `qio_opi` | Flash QIO + PSRAM Octal |
| `board_build.psram_type` | `opi` | 8MB Octal PSRAM |
| `-DARDUINO_USB_MODE=0` (build_flags) | | 禁用 Arduino USB 接管 OTG PHY |
| `-DARDUINO_USB_MODE=1` (build_unflags) | | 必须 unflag 板子默认值，否则冲突 |
| `-DARDUINO_USB_SERIAL_JTAG_ON_BOOT=1` | | 日志走 USB-Serial-JTAG 口 |
| `CONFIG_AUTOSTART_ARDUINO=y` (sdkconfig.defaults) | | 让 Arduino 自动起 `app_main` |

### `sdkconfig.defaults` 要点

```
CONFIG_USB_OTG_SUPPORTED=y               # USB Host 总开关
CONFIG_USB_HOST_HW_BUFFER_BIAS_BALANCED=y
CONFIG_SPIRAM_MODE_OCT=y                 # 8MB Octal PSRAM
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y     # 日志走第二个 USB 口
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384    # TCP 缓冲调大
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_TASK_WDT_TIMEOUT_S=15         # 兼容长 USB 传输
CONFIG_AUTOSTART_ARDUINO=y               # Arduino 作为 IDF 组件
```

### `huge_app.csv` 分区表（16MB Flash）

```
nvs       0x9000     20K
otadata   0xE000     8K
app0      0x10000    6.25MB     ← 单 app 槽，不支持 OTA
spiffs    0x650000   9.5MB
coredump  0xFF0000   64K
```

---

## 已知问题与改进方向

| # | 问题 | 状态 | 说明 |
|---|---|---|---|
| 1 | bulk OUT 每次 alloc | **已修复** | 持久化 `s_outXfer`，writer 任务复用 |
| 2 | Bulk IN 与 OUT 并发导致 ESP32 重启 | **已规避** | 禁用 Bulk IN polling（详见下文） |
| 3 | `wIndex` 编码错误 | **已修复** | `GET_PORT_STATUS` / `SOFT_RESET` 已正确左移 8 位 |
| 4 | 打印队列永不结束 | **已修复** | 合成 PJL `USTATUS JOB END` 注入（详见下文） |
| 5 | TCP idle 超时过短 | **已修复** | `SO_RCVTIMEO=2s` + 120s 兜底 |
| 6 | ZJS 签名误报 `PJL_NO_ZJS` | **已修复** | 扫描 incoming chunk 直接检测 ZJS token |
| 7 | `printer not ready` 时丢弃数据 | 未修复 | 改为阻塞等待或 TCP 层拒绝 |
| 8 | 控制传输跨任务调用 | 未修复 | HTTP `/reset` 应通过 queue 送到 client task |
| 9 | Wi-Fi 重连后 mDNS 不重启 | 未修复 | 监听 WiFi event 自动重启 mDNS |
| 10 | 没有 OTA | 未修复 | 长期使用建议改 dual OTA 分区表 |

---

## 打印队列完成机制（关键实现说明）

### 问题背景

macOS 通过 HP JetDirect（TCP:9100）协议打印时，CUPS 的 `rastertozjs` filter 会：
1. 在 PJL 序言中发送 `@PJL USTATUS JOB=ON` 开启状态推送
2. 通过 TCP 回读通道（printer → Mac）监听打印机回送的 PJL 状态
3. 等待收到 `@PJL USTATUS JOB\r\nEND` 才认为作业完成

如果打印机的状态更新从未到达 Mac（因为 Bulk IN 被禁用），CUPS 会永远等待，打印队列永远显示"正在打印"。

### 解决方案：合成 PJL 状态注入

ESP32 在检测到 **USB Bulk OUT 流量已静默 2 秒**（表示打印数据已全部写入打印机）后，**主动向 TCP 回送通道注入** 107 字节的合成 PJL 状态消息：

```
\r\n@PJL USTATUS JOB\r\nEND\r\nNAME="job"\r\nPAGES=1\r\n\x0c
@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY="Ready"\r\nONLINE=TRUE\r\n
```

关键格式要求（经 CUPS debug 日志验证）：
- `@PJL USTATUS JOB\r\nEND` 触发 readerProcess 的 `detected end of job`
- `\x0c`（form feed）作为两段 PJL 消息的分隔符
- `CODE=10001` 清除打印机 `media-needed-error` 等错误状态
- 注入后等待 2 秒让 CUPS 处理，然后发 TCP FIN

### 为什么禁用 Bulk IN

HP M1136 通过 ESP-IDF v4.4 USB Host API 的 Bulk IN 端点存在严重稳定性问题：

- `usb_host_transfer_submit` 对 Bulk IN 持续返回 `ESP_ERR_INVALID_STATE` (0x103)
- Bulk IN 与 Bulk OUT 并发提交触发 USB Host driver 内部 race condition → ESP32 panic 重启
- `usb_host_endpoint_clear/halt/flush` 无法可靠恢复端点状态

因此当前固件**完全禁用了 Bulk IN polling**（`usbReaderTask` 进入永久 sleep），改为合成注入方案。

**影响**：打印机的真实状态（卡纸、缺纸等）不会通过 macOS 打印队列显示。正常打印和队列退出不受影响。

**未来恢复方案**：升级 ESP-IDF 到 v5.x 后重新启用 Bulk IN polling。

### 控制传输的事件循环兼容

USB 控制传输（`GET_DEVICE_ID`、`GET_PORT_STATUS`）的完成回调需要 `usb_host_client_handle_events()` dispatch。为避免死锁：

1. `usbTryAttachDevice()` 不在 `clientEventCb` 回调中直接调用，而是通过 `s_pendingAttachAddr` 标志延迟到 `usbClientTask` 主循环中执行
2. `usbCtrlSync()` 在等待控制传输完成时主动 pump `usb_host_client_handle_events()`

---

## 故障排查

### 远程排错（第一件事就做这个）

固件内置了**任务历史 + 实时日志旁路**，绝大多数"不出纸"问题在 macOS 终端里 30 秒就能定位，不需要 USB-Serial-JTAG 线。

#### 远程"串口"：通过 Wi-Fi 看 ESP32 的全部日志

适合 **物理上 ESP32 OTG 接打印机、Serial-JTAG 口不方便接电脑** 的场景。所有原本写到 USB-Serial-JTAG 的日志会同时镜像到 Wi-Fi 端：

| 用法 | 命令 | 说明 |
|---|---|---|
| **实时滚动**（最像 `screen /dev/cu.*`） | `nc esp32-printer.local 9101` | 连上后先看到 ring 里的近期日志快照 + banner，然后实时追加 |
| **浏览器快照** | 打开 `http://esp32-printer.local/tail` | 8 KB 日志快照，HTML 着色 |
| **纯文本快照** | `curl 'http://esp32-printer.local/tail.txt?n=4096'` | 适合 grep / less |
| **任务历史** | `curl http://esp32-printer.local/jobs \| jq` | 见下文"任务结果" |

约定：
- 远程日志行前会自带毫秒级时间戳 `[12345] ...`，便于排序与对照
- 本地 USB-Serial-JTAG 输出**不变**（保持无时间戳的原样）
- :9101 同一时刻只允许一个连接；新连接会自动踢掉旧的
- 慢/失联的客户端会被立即丢弃，不会拖慢主循环

**SOP（按顺序做）：**

1. **看最近一次任务的体检报告**
   ```bash
   curl -s http://esp32-printer.local/jobs | jq '.[0]'
   # 或浏览器打开 http://esp32-printer.local/jobs?fmt=html
   ```
   关注三个字段：
   - `sig`：数据格式签名（见下表）
   - `bytes_in` vs `bytes_out`：TCP 进的 vs USB 出的，**应相等**
   - `usb_err_count` / `last_usb_err` / `bytes_dropped`：USB 段故障指示

2. **看实时日志**（任何终端，零安装）
   ```bash
   nc esp32-printer.local 9101
   ```
   连接后会先发一段 ring 缓冲里的近期日志，然后流式追加新行。多 client 时新的会踢掉旧的。

3. **看日志快照（浏览器友好）**
   ```bash
   open http://esp32-printer.local/tail              # HTML
   curl http://esp32-printer.local/tail.txt          # 纯文本
   curl 'http://esp32-printer.local/tail.txt?n=4096' # 限定大小
   ```

#### 签名速查表

`/jobs` 里的 `sig` 字段直接告诉你头部是什么协议。

| sig | 含义 | 结论 |
|---|---|---|
| `HP_UEL_PJL_ZJS` | 标准 HP UEL + `@PJL ENTER LANGUAGE=ZJS` + ZJStream | **数据正确**，问题不在主机驱动 |
| `PJL_NO_ZJS` | 有 `@PJL` 但没切到 ZJS | 驱动 PJL 序言不对，M1136 不认 |
| `POSTSCRIPT` | `%!PS-Adobe...` | **PPD 选错**，换 HP M1130/M1132 系列 |
| `PWG_RASTER` | `RaS2` / `RaS3` | **PPD 选错**（AirPrint 路径），换 HP M1130 系列 |
| `PDF` | `%PDF-` | 主机没经过驱动栅格化 |
| `OTHER_BINARY` | 不匹配任何已知签名 | 看 `/jobs?fmt=html` 里的 hex dump 手工判断 |
| `UNKNOWN` | 还没拿到 first64 字节 | 任务太短或 TCP 立即断 |

#### 决策树

```
不出纸
 │
 ├─ /jobs 没有任何 record
 │     → TCP 根本没连上，看 / 状态页 Wi-Fi / IP / mDNS
 │
 ├─ sig=HP_UEL_PJL_ZJS
 │     ├─ in==out 且 usb_err=0   → 打印机收齐了不出纸（硬件/驱动器/PPD 在 PJL 之外的问题）
 │     ├─ bytes_dropped > 0      → "printer not ready" 期间被丢；看实时日志找原因
 │     └─ usb_err_count > 0      → USB 段卡，看 last_usb_err（0x103=TIMEOUT 0x108=STALL）
 │
 ├─ sig ∈ {POSTSCRIPT, PWG_RASTER, PDF, PJL_NO_ZJS}
 │     → 100% 主机端驱动选错；换 "HP LaserJet Professional M1130 MFP Series" PPD
 │
 └─ sig=OTHER_BINARY / UNKNOWN
       → 看 /jobs?fmt=html 的 hex dump 人工判断
```

#### 端口一览

| 端口 | 协议 | 用途 |
|---|---|---|
| `:80` | HTTP | 状态页 `/`、任务历史 `/jobs`、日志快照 `/tail`、`/tail.txt`、软复位 `/reset` |
| `:9100` | TCP raw | HP JetDirect 打印数据（双向：OUT=打印流，IN=合成 PJL 状态） |
| `:9101` | TCP telnet | 实时日志流（`nc esp32-printer.local 9101`） |

> **可达性提示**：以上端口都绑 0.0.0.0，只要 Mac 和 ESP32 在同一局域网（路由器未开启"AP 隔离 / Client Isolation"）就能直连。无需公网、无需 VPN、无需云服务。如 `.local` 不解析，直接用 IP（看状态页或路由器后台获取）。

---

### 看不到日志
- 确认电脑接的是 ESP32-S3 的 **USB-Serial-JTAG 口**（不是 OTG 口）
- 端口名 macOS：`/dev/cu.usbmodem*`，Linux：`/dev/ttyACM*`
- 波特率 115200

### 接打印机没反应（`NEW_DEV` 没出现）
- USB OTG 线方向：板上 OTG → USB-A 母 → USB-B 公到打印机
- VBUS 供电是否到位（万用表测 OTG 口 D+ 旁 5V）
- 打印机自己是否上电、空闲态

### `Device ID` 是空或乱码
- 看 #3 已知问题：可能是控制传输 wIndex 编码 + buffer 偏移问题（GET_DEVICE_ID 当前能拿到，但 Port Status 一定是 0）
- 真正卡这里说明 claim 选错了接口

### TCP 连上了但打印机不出纸
1. 看状态页 `Bytes in / out` 是否相等
   - 不相等 → USB 写卡了，看日志 `[usb-out] bulk write err=0x?`
   - 相等且数 KB 以上 → 打印机收到了但不认数据
2. 用 macOS 本地抓包（参见顶部"前置条件"段），确认头部是 `1B 25 2D 31 32 33 34 35 58` + `@PJL ENTER LANGUAGE=ZJS`
3. 如果抓包发现是 PostScript / PWG-Raster → **驱动问题**，不是 ESP32 问题

### `Port status = 0x00`
- 大概率是上面已知问题 #3 导致的误诊断，不一定真离线

---

## ESP-IDF v4.4 USB Host 已知限制

以下问题在开发/调试过程中被发现，属于 ESP-IDF v4.4 的 USB Host 子系统限制，无法在用户层完全绕开。记录于此供后续升级参考。

### 1. Bulk IN 与 Bulk OUT 并发提交导致 panic 重启

**现象**：当 `usbReaderTask`（Bulk IN）和 `usbWriterTask`（Bulk OUT）同时向 USB Host driver 提交 transfer 时，ESP32 发生 panic 重启。
**错误码**：无明确 assert 消息（重启后 logSink ring 被清零）。
**规避方式**：完全禁用 Bulk IN polling。

### 2. Bulk IN `usb_host_transfer_submit` 返回 0x103 (ESP_ERR_INVALID_STATE)

**现象**：即使端点已 claim、已 flush/clear，Bulk IN submit 持续返回 `ESP_ERR_INVALID_STATE`。
**尝试过的恢复方式**（均无效）：
- `usb_host_endpoint_halt` + `usb_host_endpoint_flush` + `usb_host_endpoint_clear`
- 延长 / 缩短 transfer timeout
- 每次 alloc 全新 transfer（排除 reuse 问题）

### 3. Bulk IN `usb_host_transfer_submit` 返回 0x10c (ESP_ERR_NOT_ALLOWED)

**现象**：使用持久化 transfer 对象（预分配不释放）时，submit 返回 `ESP_ERR_NOT_ALLOWED`。
**原因**：driver 认为该 transfer 仍处于 in-flight 状态（前一次的 callback 可能未被 dispatch）。
**规避方式**：改为 per-call alloc/free 模型（每次提交新对象）。

### 4. Control transfer callback 需要 handle_events 驱动

**现象**：在 `clientEventCb` 回调中同步调用 `usbTryAttachDevice` → 其中 `usbCtrlSync` 提交 control transfer → 等待 semaphore → 永不完成（因为 `handle_events` 被回调本身阻塞）。
**规避方式**：
- `usbTryAttachDevice` 通过 `s_pendingAttachAddr` 延迟到主循环执行
- `usbCtrlSync` 内部主动 pump `usb_host_client_handle_events`

### 5. HP M1136 Bulk IN 行为不一致

**现象**：同一台打印机在不同的连接周期中，Bulk IN 有时能返回 PJL USTATUS 数据（4-5 条消息），有时完全不响应（全部 timeout）。即使在能响应的连接中，打印完成后也不一定推送 `USTATUS JOB END` 或 `CODE=10001`（Ready）。
**结论**：即使 Bulk IN 偶尔可用，也不能作为"作业结束"的唯一判据。合成注入是必须的兜底机制。

---

## License & Credits

私人项目，无 license 声明。基于：
- Espressif ESP-IDF (`usb_host_*` API)
- Arduino-ESP32 core
- foo2zjs 项目对 ZJStream 协议的逆向资料（参考用，本项目不解析 ZJStream）

---

## 相关参考

- USB Printer Class Specification 1.1（class 0x07, GET_DEVICE_ID / GET_PORT_STATUS / SOFT_RESET）
- HP PJL Technical Reference（`@PJL ENTER LANGUAGE=ZJS`）
- ESP-IDF USB Host Library 文档：
  https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32s3/api-reference/peripherals/usb_host.html
- HP JetDirect / AppSocket 协议：raw bytes over TCP:9100，无握手
