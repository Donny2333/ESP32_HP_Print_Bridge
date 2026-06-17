# ESP32_HP_Print_Bridge Bulk IN 接入方案与风险分析

> 目标：让打印机的真实状态回读通道（USB Bulk IN → PJL USTATUS）重新上线，
> 用真实的 `JOB END` / 错误状态取代或补强当前基于"USB 静默 8 秒"的合成注入启发式。

## 一、现状（代码事实）

Bulk IN 的基础设施其实早已铺好，只是被主动关闭：

| 已就绪的基础设施 | 位置 | 当前状态 |
|---|---|---|
| `epIn` / `epInMps` 端点发现 | `descFindPrinterInterface` | ✅ 已捕获（M1136 proto=0x02 双向，mps=64）|
| `usbReaderTask`（Bulk IN 轮询）| `src/usb_bridge.cpp` | ❌ 永久 sleep 桩 |
| `backFwdTask`（back-channel → TCP）| `src/usb_bridge.cpp` | ❌ 永久 sleep 桩 |
| `s_backBuf` 16KB PSRAM 环形缓冲 | `src/main.cpp` | ✅ 已分配、闲置 |
| `s_rawClientFd` / `s_totalBytesBack` / `s_totalBackToClient` | `bridge_state` / `net_server` | ✅ 已布线 |
| `kInErrorRecoveryThreshold=10` | `src/bridge_config.h` | ✅ 保留备用 |
| 合成 PJL 注入（替代方案）| `rawServerTask` | ✅ 当前生效（OUT 静默 8s → 注入 107B `JOB END`）|

**当前完成判定靠启发式**：USB Bulk OUT 流量静默 8 秒后，主动向 TCP 回送通道注入合成
`@PJL USTATUS JOB\r\nEND ...`，让 CUPS 的 readerProcess 认为作业完成。Bulk IN 一旦接入，
目标是用**真实** PJL 回读取代/补强它，拿到真实的 JOB END + 缺纸 / 卡纸 / 门开等状态。

## 二、为什么当年关闭（已记录的 ESP-IDF v4.4 限制）

1. Bulk IN 与 Bulk OUT **并发提交 → USB Host driver 内部 race → ESP32 panic 重启**。
2. Bulk IN `usb_host_transfer_submit` 持续返回 `0x103 (ESP_ERR_INVALID_STATE)`，即便端点已
   claim / flush / clear。
3. 复用持久化 transfer 对象 → `0x10c (ESP_ERR_NOT_ALLOWED)`。
4. M1136 本身 Bulk IN **行为不一致**：同一台机器不同连接周期，有时回 4-5 条 USTATUS，
   有时全 timeout，且打印完成后不保证推送 `USTATUS JOB END` 或 `CODE=10001`（Ready）。

## 三、方案选项

### 方案 A：先升级 ESP-IDF 到 v5.x（README 推荐路径）

- arduino-esp32 2.0.17（IDF v4.4.7）→ 3.x（IDF v5.1）或切纯 ESP-IDF v5.x。
- v5.x USB Host 重写了大量并发 / 状态机问题，#1 / #2 / #3 大概率自然消失。
- 代价：牵动 WiFiServer / `mqtt_client` / ArduinoOTA / sdkconfig 键名 / 分区处理，**全项目回归**。

### 方案 B：在现有 v4.4 上硬做半双工

- 用"USB 事务锁"把 `submit → complete` 整段串行化（而非现在只锁 `submit` 一行），
  OUT 在飞时绝不提交 IN。
- 风险：文档记录即便串行化，IN `submit` 仍返回 `0x103`——**可能在 v4.4 根本无解**。

### 方案 C（推荐）：合成注入保底 + Bulk IN 机会式增强

- 保留 8s 合成注入兜底；空闲窗口尝试读 Bulk IN，**读到真实 `JOB END` 就用真实的，
  读不到就退回合成**。
- 不破坏现有可用行为，最低回归风险，也最契合 M1136"时灵时不灵"的特性。

## 四、关键设计点

1. **互斥模型升级**：`s_usbSubmitMux` 当前仅包住 `usb_host_transfer_submit` 一行；接入 IN 后
   需改成跨整个事务持锁（submit + 等待 complete），或做 writer / reader 的半双工握手
   （writer 发出 "OUT idle" 信号后 reader 才轮询）。
2. **轮询节奏**：IN 轮询要把 timeout(NAK) 当正常；打印中只在 OUT 空档短轮询，作业尾部加密
   轮询以抓 `JOB END`。
3. **完成路径去重**：真实 `JOB END` 与合成注入**不能同时触发**（否则 CUPS 收到两次 →
   提前 FIN → 截断，回归已知问题 #16）。需要一个 `s_jobEndSignaled` 门闩互斥两条路径。
4. **超时安全**：IN 的 timeout 必须复刻 OUT 已修复的 halt + flush + 吸收回调模式
   （已知问题 #12 / #14），且 per-call alloc / free（已知问题 #3）。
5. **解析**：在 back-channel 流里扫描 `@PJL USTATUS JOB\r\nEND` 与 `CODE=xxxxx`，
   复用现有签名 / chunk 扫描思路。

## 五、风险点

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R1 | 并发提交 → **panic 重启** | 🔴 高 | 跨事务持锁的严格半双工；先只读诊断模式验证 |
| R2 | v4.4 上 IN `submit=0x103` 可能**根本无解** | 🔴 高 | 先做 Phase-0 探针采数据；不行就转方案 A |
| R3 | 半双工偷走 OUT 时间 → **打印吞吐回退** / stream buffer 积压 → TCP 背压 | 🟠 中 | 仅 OUT 空档轮询；限制单次 IN 时长 |
| R4 | M1136 IN 不稳定，**不能作唯一完成判据** | 🟠 中 | 合成注入永久保底（方案 C）|
| R5 | 真实 + 合成**双 JOB END** → 截断（回归 #16）| 🔴 高 | `s_jobEndSignaled` 门闩互斥两条路径 |
| R6 | reader 任务**抢占 / 被饿**（pri3 core0 与 usb-cli/usb-out）→ 控制回调或 WDT(15s) | 🟠 中 | 维持 reader 低于 usb-cli；监控 WDT |
| R7 | IN 超时 **Use-After-Free**（同 #12 / #14）| 🟠 中 | 复刻 halt + flush + 吸收回调 |
| R8 | transfer 复用 `0x10c` | 🟡 低 | per-call alloc / free |
| R9 | 方案 A 的**全项目回归**（WiFi/mDNS/MQTT/OTA/HTTP/sdkconfig）| 🔴 高 | 当独立 epic，全链路回归测试 |
| R10 | `s_backBuf` 16KB **溢出**（CUPS 读得慢）| 🟡 低 | 满则丢弃 + 用 `s_totalBytesBack/ToClient` 观测 |
| R11 | 真实状态**送达时机 / 帧格式**与 CUPS readThread 不匹配 | 🟠 中 | 对照已验证的 107B 合成格式校准 |
| R12 | **难以单测 / 复现**（依赖硬件 + 非确定性打印机）| 🟠 中 | Phase-0 长跑采样统计 timeout / 有效率 |

## 六、建议推进顺序

- **Phase 0（探针，零风险）**：把 `usbReaderTask` 改成**只读诊断模式**——半双工读 Bulk IN
  仅打日志 / 计数（timeout vs 有效消息、是否出现 `JOB END` / `CODE`），**不接完成逻辑、
  保留合成注入**。在真机上长跑采数据，回答 R2 / R4 是否在 v4.4 可行。
- **Phase 1（决策）**：数据若证明 v4.4 不可行 → 立项方案 A（ESP-IDF v5.x 升级）；可行 → 进 Phase 2。
- **Phase 2（接入，方案 C）**：实现事务锁半双工 + 真实 `JOB END` 解析 + `s_jobEndSignaled`
  去重，合成注入降级为超时兜底。
- **Phase 3**：长期保留合成兜底（因 R4），不建议彻底移除。

## 七、涉及的代码触点（实现时参考）

- `src/main.cpp` · `startUsbHost()`：reader 任务已以 pri 3 / core 0 创建，无需新建。
- `src/usb_bridge.cpp`：实现 `usbReaderTask`、新增 `usbBulkInSync`；把 `s_usbSubmitMux`
  升级为跨事务锁或半双工握手。
- `src/net_server.cpp`：`backFwdTask` 接通 `s_backBuf → s_rawClientFd`；`rawServerTask`
  里给合成注入加 `s_jobEndSignaled` 门闩。
- `src/bridge_config.h`：`kInErrorRecoveryThreshold` 复用，新增 IN 轮询节奏 / 超时常量。
