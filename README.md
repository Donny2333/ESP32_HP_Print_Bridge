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
- **OTA**：ArduinoOTA（Wi-Fi 无线升级，双 OTA 分区）
- **远程遥测**：MQTT（ESP-IDF 原生 `mqtt_client`，推送系统日志 + 打印任务审计 JSON）
- **单元测试**：PlatformIO native 环境 + Unity（`pio test -e native`，纯逻辑模块跑在开发机上）

---

## 目录结构

```
.
├── CMakeLists.txt              # IDF 顶层 (arduino-as-component)
├── platformio.ini              # 板子/USB/PSRAM/分区/OTA/native 测试环境
├── sdkconfig.defaults          # USB Host / PSRAM / FreeRTOS / 日志路由
├── sdkconfig.esp32-s3-n16r8    # 由 PlatformIO 生成（首次 build 后产生）
├── sdkconfig.esp32-s3-n16r8-ota # OTA 环境由 PlatformIO 生成
├── ota_16mb.csv                # 16MB Flash 双 OTA 分区表（2x5MB app + SPIFFS）
├── README_mqtt_plan.md         # MQTT 接入计划与风险分析（设计文档）
├── scripts/version.py          # 构建时注入 Git 版本/dirty/构建时间到 FW_* 宏
├── include/
│   ├── secrets.example.h       # Wi-Fi/mDNS/OTA/MQTT 配置模板（提交到 git）
│   └── secrets.h               # 你的实际凭据（**不提交**，已在 .gitignore）
├── src/
│   ├── CMakeLists.txt          # IDF component register（GLOB_RECURSE src/*.*）
│   ├── main.cpp                # 启动/接线层：Wi-Fi、mDNS、OTA、缓冲分配、建任务、setup/loop
│   ├── bridge_config.h         # 编译期配置常量 + USB Printer Class 请求码
│   ├── bridge_state.h/.cpp     # 跨模块共享全局（UsbState、统计量、打印机状态）+ macToString
│   ├── usb_bridge.h/.cpp       # USB Host：attach/detach、控制传输、Bulk OUT writer
│   ├── net_server.h/.cpp       # 原生 lwIP TCP 服务：:9100 打印 + :9101 telnet 日志
│   ├── http_status.h/.cpp      # HTTP :80 状态页 / /jobs / /tail / /reset
│   ├── ota_update.h/.cpp       # ArduinoOTA 封装：otaInit/otaHandle + 进度/状态查询
│   ├── mqtt_logger.h/.cpp      # MQTT 后台发布：系统日志 + 任务审计 JSON
│   ├── job_log.h/.cpp          # 每个打印任务的事后诊断环形缓冲（16 槽）
│   ├── log_sink.h/.cpp         # 日志环形缓冲（供 :9101 / /tail / MQTT 远程旁路）
│   └── version.h               # FW_* 宏的默认值（被 version.py 注入覆盖）
├── test/
│   ├── test_native/            # Unity 单元测试：job_log / log_sink 纯逻辑
│   └── shims/                  # 开发机替身头（FreeRTOS / esp_timer / esp_log / Arduino）
└── lib/                        # 未使用
```

> 早期版本所有逻辑在单个 `main.cpp`（~1100 行）里；现已按子系统拆分为上述多文件。
> 拆分为纯机械搬运（逻辑零改动）：跨文件共享的全局集中到 `bridge_state`，
> 各子系统私有状态仍是文件内 `static`。`src/CMakeLists.txt` 用 `GLOB_RECURSE`
> 自动纳入所有 `src/*.*`，新增文件无需手动登记。

---

## 关键代码模块

各子系统的入口任务与核心函数：

### `usb_bridge.*` — USB Host

| 函数 | 说明 |
|---|---|
| `usbHostLibTask` | 跑 USB Host 库事件循环 |
| `usbClientTask` + `clientEventCb` | 处理 NEW_DEV / DEV_GONE，延迟调用 `usbTryAttachDevice` |
| `descFindPrinterInterface` | 在 config descriptor 里找 Printer Class 接口 + bulk OUT/IN endpoint |
| `usbTryAttachDevice` | 打开设备、claim 接口、settle 300ms、读 Device ID、读 Port Status（均带一次超时重试），最后才置 `s_printerReady`；Bulk OUT 不预分配，走 per-call alloc |
| `usbCtrlSync` | 同步控制传输，内部 pump `handle_events` 避免死锁；超时则 halt+flush 并吸收回调；由 `s_usbControlMux` 串行化 |
| `usbCtrlGetDeviceId` / `usbCtrlGetPortStatus` / `usbCtrlSoftReset` | Printer Class 控制请求（wIndex 高字节 = 接口号）|
| `usbWriterTask` | 从 stream buffer 取数据，≤1024B 一块送 bulk OUT；每次 `usb_host_transfer_alloc`/`free`（per-call，不复用 transfer）|
| `usbReaderTask` / `backFwdTask` | **已禁用**（Bulk IN 在 ESP-IDF v4.4 下不稳定） |
| `waitForPrinter` | 阻塞等待打印机就绪（供 writer / raw server 复用） |

### `net_server.*` — 原生 lwIP TCP

| 函数 | 说明 |
|---|---|
| `openListenSocket` | 建监听 socket（SO_REUSEADDR），:9100 / :9101 共用 |
| `rawServerTask` | TCP:9100 接收 → stream buffer → USB OUT；作业结束后注入合成 PJL 状态 |
| `tcpLogTask` | TCP:9101 单连接日志流（新连接踢掉旧的） |

### `http_status.*` — HTTP :80

| 函数 | 说明 |
|---|---|
| `handleRoot` | `/` 状态页（Wi-Fi / 打印机 / 字节计数 / 最近任务摘要） |
| `handleReset` | `/reset` 软复位打印机端口 |
| `handleJobs` | `/jobs` 任务历史（JSON，或 `?fmt=html` 带 hex dump 的表格） |
| `handleTail` / `handleTailTxt` | `/tail`、`/tail.txt` 日志快照 |

### 其余

| 模块 | 说明 |
|---|---|
| `main.cpp` (`setup`/`loop`) | 入口：起 log sink、分配缓冲、`startWifi`（带 Wi-Fi event handler 自动重启 mDNS + 启动 MQTT）/`startMdns`/`otaInit`/`startUsbHost`、建各任务；loop 跑 HTTP server + `otaHandle` |
| `ota_update.*` | ArduinoOTA 封装：`otaInit`（设主机名/密码 + start/end/progress/error 回调）、`otaHandle`（loop 内 pump）、`otaInProgress`/`otaProgressPercent`/`otaStatusText`（供状态页展示） |
| `bridge_state.*` | 跨模块共享全局的唯一定义 + `extern` 声明（`s_usb`、`s_usbSubmitMux`、统计量、`s_printerReady`/`s_deviceId`/`s_portStatus`、`s_lastUsbWriteMs` 等） |
| `bridge_config.h` | 全部编译期常量（Wi-Fi/端口/缓冲大小/超时阈值/USB 请求码） |
| `job_log.*` / `log_sink.*` | 诊断环形缓冲（任务历史 16 槽 / 远程日志旁路）；`jobEnd` 还会序列化 JobRecord 为 JSON 交给 MQTT |
| `mqtt_logger.*` | Wi-Fi 获取 IP 后由事件回调启动；后台 `mqttLog` 任务低优先级轮询 log ring 推送到 `…/logs`（QoS 0），任务结束时推送审计 JSON 到 `…/jobs`（QoS 1）；`MQTT_BROKER_URI` 为空则整体禁用 |

**任务/核心分配：**
- Core 0：USB lib（pri 5）、USB client（pri 4）、USB writer（pri 4）、USB reader（pri 3，已禁用）
- Core 1：raw9100 server（pri 3）、telnet 日志 `log9101`（pri 2）、MQTT `mqttLog`（pri 1）、Arduino loop（HTTP server + OTA，pri 1）
- `back-fwd` 转发任务（pri 3，Core 1）随 Bulk IN 一并禁用

**数据流缓冲：**
- 256 KB **StreamBuffer**，存储区在 PSRAM（`MALLOC_CAP_SPIRAM`）
- TCP → StreamBuffer → 1024B 块 → bulk OUT

---

## 单元测试

纯逻辑模块（`job_log` 的签名分类 / 环形缓冲、`log_sink` 的快照与 cursor 流式读取）有一套跑在开发机上的 Unity 测试，**不需要硬件**：

```bash
pio test -e native
```

实现方式：`[env:native]` 用 `platform = native` 把 `src/job_log.cpp` / `src/log_sink.cpp` 编译到本机，依赖项由 `test/shims/` 下的替身头满足（FreeRTOS 临界区→空操作、`esp_timer`→假时钟、`esp_log` / Arduino `Serial`→捕获缓冲）。设备构建会忽略 `test_native`（见 `platformio.ini` 的 `test_ignore`）。

覆盖点：签名识别（`HP_UEL_PJL_ZJS` / `PJL_NO_ZJS` / `POSTSCRIPT` / `PDF` / `PWG_RASTER` / `OTHER_BINARY`）、跨 chunk 边界检测 ZJS、任务环形缓冲 16 槽回绕、字节/错误计数器、日志 ring 回绕与 cursor 追读。

---

## 配置参考

### `platformio.ini` 要点

| 选项 | 值 | 说明 |
|---|---|---|
| `board` | `esp32-s3-devkitc-1` | 标准 S3 DevKitC |
| `board_upload.flash_size` | `16MB` | 适配 N16R8 |
| `board_build.partitions` | `ota_16mb.csv` | 双 OTA app 槽，每槽 5MB |
| `extra_scripts` | `pre:scripts/version.py` | 构建前注入 Git 版本、dirty 状态和构建时间 |
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

### `ota_16mb.csv` 分区表（16MB Flash）

```
nvs       0x9000     20K
otadata   0xE000     8K
app0      0x10000    5MB        ← OTA slot 0
app1      0x510000   5MB        ← OTA slot 1
spiffs    0xA10000   5.875MB
coredump  0xFF0000   64K
```

---

## 已知问题与改进方向

| # | 问题 | 状态 | 说明 |
|---|---|---|---|
| 1 | bulk OUT transfer 复用触发 `0x10c` | **已规避** | 当前使用 per-call alloc/free；持久化 `s_outXfer` 在大图/AirPrint 作业后会卡住 |
| 2 | Bulk IN 与 OUT 并发导致 ESP32 重启 | **已规避** | 禁用 Bulk IN polling（详见下文） |
| 3 | `wIndex` 编码错误 | **已修复** | `GET_PORT_STATUS` / `SOFT_RESET` 已正确左移 8 位 |
| 4 | 打印队列永不结束 | **已修复** | 合成 PJL `USTATUS JOB END` 注入（详见下文） |
| 5 | TCP idle 超时过短 | **已修复** | `SO_RCVTIMEO=2s` + 120s 兜底 |
| 6 | ZJS 签名误报 `PJL_NO_ZJS` | **已修复** | 扫描 incoming chunk 直接检测 ZJS token |
| 7 | `printer not ready` 时丢弃数据 | **已修复** | 改为阻塞等待，超时则直接切断 TCP 让对端感知失败重试 |
| 8 | 控制传输跨任务并发 | **已修复** | 新增 `s_usbControlMux`（`usb_bridge.cpp` 文件内 static），彻底串行化 `GET_PORT_STATUS`、`SOFT_RESET` 和 attach 阶段的控制端点请求，防止驱动死锁 |
| 9 | Wi-Fi 重连后 mDNS 不重启 | **已修复** | 监听 Wi-Fi 事件并在重新获取 IP 时自动重启 mDNS 服务 |
| 10 | 没有 OTA | **已修复** | 使用 `ota_16mb.csv` 双 OTA 分区表 + ArduinoOTA |
| 11 | macOS HP PPD 默认手动进纸 | **已修复** | `esp32_printer` 队列 PPD 增加 `InputSlot Auto` 并设为默认，避免打印机闪灯等待手动进纸 |
| 12 | Bulk OUT 超时后 Use-After-Free | **已修复** | `usbBulkOutSync` 超时不再立刻 free，强制执行 `halt` 和 `flush` 终结 in-flight 状态，安全吸收回调防止 Panic |
| 13 | Web 页面高频刷新致内存碎片化 | **已修复** | 状态页增加 `html.reserve(4096)`，并限制 `GET_PORT_STATUS` 降频至最多 10 秒刷新一次，防止卡死核心队列 |
| 14 | 控制传输超时后 Use-After-Free | **已修复** | `usbCtrlSync` 发生超时后也会使用 `halt` 与 `flush` 安全挂起，并等待吸收异步回调，防止指针悬空与内存越界访问 |
| 15 | 日志任务错误重置打印业务 FD | **已修复** | `tcpLogTask` 断开连接时不再修改 `s_rawClientFd`，已完全解决双任务竞争引发的真实业务数据流断连隐患 |
| 16 | 打印长篇文档/高分辨率图片被截断 | **已修复** | 合成 `JOB END` 注入的 **USB 静默判定**从 2 秒延长到 8 秒（`net_server.cpp` 中 `s_lastUsbWriteMs` 空档阈值），显著兼容 Mac 处理大照片时缓慢光栅化产生的空档期，杜绝提前注入断连。注意此值与 drain 阶段的 `kDrainQuiescentMs=1.5s` 是两个独立计时器 |

---

## 打印队列完成机制（关键实现说明）

### 问题背景

macOS 通过 HP JetDirect（TCP:9100）协议打印时，CUPS 的 `rastertozjs` filter 会：
1. 在 PJL 序言中发送 `@PJL USTATUS JOB=ON` 开启状态推送
2. 通过 TCP 回读通道（printer → Mac）监听打印机回送的 PJL 状态
3. 等待收到 `@PJL USTATUS JOB\r\nEND` 才认为作业完成

如果打印机的状态更新从未到达 Mac（因为 Bulk IN 被禁用），CUPS 会永远等待，打印队列永远显示"正在打印"。

### 解决方案：合成 PJL 状态注入

ESP32 在检测到 **USB Bulk OUT 流量已静默 8 秒**（表示打印数据已全部写入打印机且打印机处于空闲）后，**主动向 TCP 回送通道注入** 107 字节的合成 PJL 状态消息，并在注入后再等待 2 秒让 CUPS 消化：

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

并发安全由**两把互斥锁**保证：
- `s_usbControlMux`（`usb_bridge.cpp` 文件内 static，首次调用 `usbCtrlSync` 时惰性创建）：串行化所有控制端点请求（`GET_DEVICE_ID` / `GET_PORT_STATUS` / `SOFT_RESET` 及 attach 阶段），防止驱动死锁
- `s_usbSubmitMux`（`bridge_state` 全局，在 `startUsbHost()` 创建）：串行化 Bulk OUT 提交，规避 ESP-IDF v4.4 USB Host driver 非重入问题

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

### 3. 复用 USB transfer 时 `usb_host_transfer_submit` 返回 0x10c (ESP_ERR_NOT_ALLOWED)

**现象**：使用持久化 transfer 对象（预分配不释放）时，submit 返回 `ESP_ERR_NOT_ALLOWED`。Bulk OUT 在大图 / AirPrint 照片作业后也观察到同类卡死。
**原因**：driver 认为该 transfer 仍处于 in-flight 状态（前一次的 callback 可能未被 dispatch）。
**规避方式**：Bulk OUT 当前也改为 per-call alloc/free 模型（每次提交新对象），避免复用 `s_outXfer`。

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
