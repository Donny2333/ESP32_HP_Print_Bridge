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

| 模块 | 行号 | 说明 |
|---|---|---|
| `usbHostLibTask` | 162 | 跑 USB Host 库事件循环 |
| `usbClientTask` + `clientEventCb` | 209 / 189 | 处理 NEW_DEV / DEV_GONE |
| `descFindPrinterInterface` | 241 | 在 config descriptor 里找 Printer Class 接口 + bulk OUT/IN endpoint |
| `usbTryAttachDevice` | 320 | 打开设备、claim 接口、读 Device ID、读 Port Status |
| `usbCtrlGetDeviceId` / `usbCtrlGetPortStatus` / `usbCtrlSoftReset` | 539 / 576 / 589 | 三个 USB Printer Class control 请求 |
| `usbWriterTask` + `usbBulkOutSync` | 660 / 618 | 从 stream buffer 取数据，1024B 一块送 bulk OUT |
| `usbReaderTask` | 724 | 持续从 bulk IN 收打印机回包（PJL USTATUS） |
| `rawServerTask` | 789 | TCP:9100 接收，推 stream buffer |
| `handleRoot` / `handleReset` | 912 / 954 | HTTP 状态页 + 软复位入口 |
| `startWifi` / `startMdns` / `startUsbHost` | 971 / 996 / 1024 | 启动各子系统 |
| `setup` / `loop` | 1065 / 1100 | 入口 |

**任务/核心分配：**
- Core 0：USB lib（pri 5）、USB client（pri 4）、USB writer（pri 4）、USB reader（pri 3）
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

代码已经能跑通主路径，但深入审查后有以下值得修复的点（按重要性排序）。详情见对话/审查记录或源码注释。

| # | 问题 | 位置 | 修复建议 |
|---|---|---|---|
| 1 | bulk OUT 每次都 `usb_host_transfer_alloc`，1MB 作业 alloc 千次 | `usbBulkOutSync` (618) | writer 任务里**只 alloc 一次** transfer，循环复用 |
| 2 | reader 任务每次读完 `vTaskDelay(50ms)`，IN endpoint 可能阻塞 | `usbReaderTask` (781) | 不要 sleep，始终保持 IN transfer pending |
| 3 | `GET_PORT_STATUS` / `SOFT_RESET` 的 `wIndex` 没左移 8 位 | (579, 591) | 改为 `((uint16_t)ifNumber << 8)` 同 `GET_DEVICE_ID` |
| 4 | `printer not ready` 时丢弃流缓冲数据 | `usbWriterTask` (672–685) | 改为阻塞等待，或 TCP 层先拒绝连接 |
| 5 | 控制传输跨任务调用（HTTP `/reset` vs USB client task）| `handleReset` (954) | 用 queue 把请求送到 client task 处理 |
| 6 | `descFindPrinterInterface` 只接受 `alt == 0` | (274) | 若 MFP 在 alt 1/2 才有正确 endpoint 需要兼容 |
| 7 | bulk OUT 超时 8s，大栅格页可能不够 | (692) | 改成 30s |
| 8 | TCP idle 超时 10s，大作业可能误断 | (858) | 改成 30–60s |
| 9 | Wi-Fi 重连后 mDNS 不重启 | `startWifi` (971) | 监听 WiFi event 自动 `MDNS.end()` + `startMdns()` |
| 10 | 没有 OTA（huge_app.csv 单分区） | partitions | 长期使用建议改 dual OTA 分区表（牺牲 SPIFFS） |

---

## 故障排查

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
