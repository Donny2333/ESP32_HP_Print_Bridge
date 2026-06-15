# ESP32_HP_Print_Bridge MQTT 接入计划与风险分析

## 一、 计划目标
1. **实时日志追踪**：将 ESP32 设备的运行日志（包括系统打印、报错、调试信息）实时推送到 MQTT Broker，解决设备无 USB 连接时难以排查底层错误的问题。
2. **任务状态监控**：将打印任务的详细执行记录（`JobRecord`，包含成功/失败状态、耗时、数据量、错误码等）以结构化数据（如 JSON 格式）推送到专属的 MQTT Topic，实现对打印机工作状态的远程审计。
3. **低侵入与高稳定性**：避免因网络波动或发送日志阻塞核心任务（如 USB 数据传输与 TCP 接收）。将 MQTT 消息发送独立在后台任务中处理。

## 二、 技术选型分析
* 项目使用的是 `framework = arduino, espidf` 混合框架。
* 经编译测试验证，项目可直接使用 ESP-IDF 原生的 `<mqtt_client.h>` 组件，**无需引入第三方 Arduino 库（如 PubSubClient）**。ESP-IDF 的 MQTT 客户端更底层、支持完全的异步事件驱动，且具备更高的稳定性，非常适合当前项目。

## 三、 详细实施步骤计划

### 1. 新增 MQTT 配置项
在 `include/secrets.example.h`（及用户本地的 `secrets.h`）中增加 MQTT 相关的宏定义：
* `MQTT_BROKER_URI` (例如 `"mqtt://192.168.1.100:1883"`)
* `MQTT_USERNAME` 与 `MQTT_PASSWORD`
* `MQTT_TOPIC_LOGS` (例如 `"esp32_printer/logs"`)
* `MQTT_TOPIC_JOBS` (例如 `"esp32_printer/jobs"`)

### 2. 创建独立 MQTT 管理模块 (`src/mqtt_logger.h` / `.cpp`)
* 封装 `esp_mqtt_client_init` 和相关的事件回调函数（`MQTT_EVENT_CONNECTED`, `MQTT_EVENT_DISCONNECTED` 等）。
* 提供通用的发布接口，例如 `mqtt_publish_log(const char* text)` 和 `mqtt_publish_job(const char* json)`。
* 将 MQTT 客户端的启动与当前 `main.cpp` 中的 Wi-Fi 状态解耦：监听到 `ARDUINO_EVENT_WIFI_STA_GOT_IP` 事件时启动 MQTT 客户端。

### 3. 接入现有日志流 (集成与对齐)
* **对接 `log_sink` (系统日志)**：
  为了避免在核心的 `vprintf` 钩子函数中直接发送网络请求导致死锁或性能下降，计划创建一个低优先级的独立 FreeRTOS Task（类似现有的 `tcpLogTask`）。该任务会不断轮询 `log_sink` 环形缓冲区的读取游标 (`logSinkReadFromCursor`)，将新产生的日志按块（Chunk）通过 MQTT 发布出去。
* **对接 `job_log` (打印任务审计)**：
  在 `job_log.cpp` 中的 `jobEnd()` 函数末尾（任务完结时），调用新增的接口，将该条记录序列化为 JSON（逻辑可参考 `http_status.cpp` 中的 `handleJobs` 实现），推送到单独的任务监控 Topic。

## 四、 代码修改可能引入的风险点

1. **内存消耗风险 (RAM 耗尽)**
   * **原因**：ESP-IDF MQTT 客户端启动时会分配内部的发送/接收 Buffer，且开启额外的后台任务进行网络 I/O 轮询。本项目中已有处理打印作业流的极大 Buffer (`256KB PSRAM` 和内部 `SRAM`)，若 MQTT 并发数堆积或数据未能及时发出，可能导致堆内存 (Heap) 耗尽。
   * **应对方案**：将 MQTT 的 Buffer 限制在一个合理且较小的值范围内；限制 MQTT 发送队列深度；遇到资源瓶颈优先丢弃日志而非阻塞。

2. **核心业务阻塞风险 (死锁/卡顿)**
   * **原因**：ESP32 的日志打印 `logTee` / `logSinkWrite` 是通过拦截 `vprintf` 实现的，这些日志打印可能发生在任何任务中，包括高优的 USB 传输任务或中断中。如果在这些上下文中直接调用 MQTT 发送 API，网络阻塞会导致系统雪崩。
   * **应对方案**：严格坚持“异步轮询”策略。核心任务只向原有的无锁（或自旋锁）`log_sink` 内存环形缓冲区写数据。MQTT 发布逻辑置于完全隔离的低优先级后台 Task 中处理。

3. **网络震荡重连引发看门狗复位**
   * **原因**：Wi-Fi 不稳定或 MQTT Broker 不断闪断重连时，底层的重试机制可能抢占 CPU 资源。
   * **应对方案**：由 ESP-IDF 内部机制管理重连，不要在应用层引入同步的 `delay()` 重试阻塞。对 MQTT 模块增加错误限流输出。

4. **日志死循环推送**
   * **原因**：如果在 MQTT 模块自身的回调或发送过程中调用了 `ESP_LOG` 或 `logTee` 进行调试输出，这部分日志又会被投递到 MQTT 队列中，形成无限死循环放大。
   * **应对方案**：在 MQTT 相关的 `ESP_LOG` 前面进行标识过滤，或在 MQTT 模块内部使用标准 `printf` 绕过钩子，并严禁在 MQTT 回调链路上产生业务日志。
