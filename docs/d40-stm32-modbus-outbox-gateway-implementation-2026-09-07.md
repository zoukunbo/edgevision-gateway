# D40：STM32 Modbus、SQLite Outbox 与 MQTT Gateway 实现复盘

日期：2026-09-07
项目：EdgeVision Gateway
硬件链路：DHT11 → STM32 → SP3485 → OK1126B-S UART5

## 1. 最终目标与结果

本阶段要解决的不是“读到一次串口数据”，而是把真实设备数据接入一个断网不丢、重启可补发、重复可识别、能够安全停止的常驻 Gateway：

```text
STM32 Modbus RTU
    ↓
MeasurementSource
    ↓ measurement queue
SQLite measurements + Outbox（同一事务）
    ↓ delivery queue
MQTT QoS 1 / PUBACK
    ↓ result queue
Outbox pending → sent
```

最终取得的证据：

- 正式 ARM64 Gateway 在 OK1126B-S 上读取 `/dev/ttyS5`，通过 `/dev/gpiochip0` line 22 控制 RS485 收发方向。
- 实板连续产生 102 条 Measurement：温度 51 条、湿度 51 条，序列 1–102 连续且唯一。
- 中断 Broker 10 秒期间，数据库从 31 条增长到 42 条，新增 11 条保持 pending，证明采集和落盘不被 MQTT 阻塞。
- Broker 恢复后 pending 清零，102 条全部 sent，SQLite `integrity_check=ok`。
- 102 个 Outbox 记录拥有 102 个不同的持久化 `message_id`。
- Gateway 收到 SIGTERM 后干净退出；主机完整回归测试 26/26 通过。

## 2. 为什么要分层，而不是把所有代码写进 main

`main.c` 只应该解析参数并组装配置。设备协议、业务对象、存储可靠性和网络投递分别属于不同责任：

- 串口层解决字节收发、方向控制、总超时和取消。
- Modbus 层解决请求构造、CRC、站号、功能码、字节数和异常帧校验。
- Source 层把设备寄存器转换为统一的 `measurement_t`。
- Storage 层保证 Measurement 与待投递记录同成同败。
- MQTT 层只在收到对应 PUBACK 后报告成功。
- Gateway Core 负责对象生命周期、线程编排和停止顺序。

这样设计后，模拟源和 STM32 源都实现同一个 `measurement_source_t` 接口，Storage/MQTT worker 不需要知道数据来自真实串口还是测试数据。

## 3. 三 worker 与三条有界队列

### 3.1 线程职责

1. Source worker 独占 `MeasurementSource`，顺序执行设备事务。
2. Storage worker 独占 SQLite Store，保存 Measurement、读取最早 pending、处理投递结果。
3. MQTT worker 独占 Publisher，发布一条消息并同步等待对应 PUBACK。

Mosquitto 自己的网络线程只负责协议收发和回调，不直接访问 SQLite。

### 3.2 队列职责

- measurement queue：Source → Storage，元素是可按值复制的 `measurement_t`。
- delivery queue：Storage → MQTT，传递 Outbox 投递任务。
- result queue：MQTT → Storage，传递成功或失败结果。

容量采用 32/1/1。第一版只允许一条投递 inflight，避免 Storage 在结果返回前重复装载同一个 pending。批量并发需要 lease/inflight 状态，不应在基础语义未稳定时提前引入。

### 3.3 所有权原则

`measurement_t` 内部是固定值字段，可以由队列浅拷贝。`outbox_item_t` 含动态字符串：

- 入 delivery queue 成功后，所有权从 Storage worker 转给 MQTT worker。
- 入队失败时，仍由 Storage worker 释放。
- MQTT worker 完成后只释放一次。

这条规则避免泄漏、重复释放和悬空指针。

## 4. SQLite Outbox 的可靠性语义

一次采集不能先写 Measurement、稍后再尝试写 Outbox，否则进程可能在两步之间崩溃，留下永远不会发送的数据。因此两条记录必须在同一个事务中保存：

```text
BEGIN
  INSERT measurements(payload_json)
  INSERT outbox(measurement_id, message_id, topic, state='pending')
COMMIT
```

发送流程是：

```text
读取最早 pending
→ MQTT QoS 1 publish
→ 等待与本次 mid 匹配的 PUBACK
→ Storage worker 将该 outbox 标为 sent
```

网络失败只更新 `attempt_count` 和 `last_error`，状态继续保持 pending。数据库使用 WAL、foreign key 和 busy timeout，并在启动时执行 schema 版本检查和迁移。

## 5. ACK 崩溃窗口与 at-least-once

QoS 1 不等于 exactly-once。关键窗口是：

```text
Broker 已返回 PUBACK
→ 进程崩溃
→ SQLite 尚未执行 mark_sent
```

重启后这条记录仍是 pending，所以会再次发送。通过 GDB 在 `outbox_store_mark_sent()` 入口断住并终止测试进程，已经观察到相同消息重放。

这不是 bug，而是 Outbox + QoS 1 的 at-least-once 边界：允许重复，但不能丢。要让业务结果不重复，必须提供稳定消息身份并由消费端幂等处理。

## 6. message_id 的设计与实现

### 6.1 为什么不用数据库自增 ID

`gateway-01:<自增ID>` 在数据库重建后可能从头开始，与消费端历史 ID 冲突。最终选择在 SQLite 事务内生成 128 位随机值：

```sql
lower(hex(randomblob(16)))
```

外部形式是 32 位小写十六进制字符串。

### 6.2 为什么 ID 放在 Outbox 而不是 Measurement

`message_id` 表示“可靠投递身份”，不是传感器测量本身的领域属性。因此：

- 原始 Measurement JSON 保持不变。
- Outbox schema v3 持久化 `message_id`，并用 `NOT NULL + UNIQUE + length=32` 约束。
- MQTT worker 发布前复制 JSON，再注入持久化 ID。
- 同一 Outbox 的所有重试和崩溃重放都使用同一个 ID。

v2→v3 迁移在单个 `BEGIN IMMEDIATE` 事务中重建 Outbox 表，为历史记录生成 ID，同时保留状态、失败次数、错误和发送时间。

## 7. 消费端幂等示例

消费端不能只“先查询再插入”，因为并发消费者可能同时查到不存在。示例使用数据库唯一约束作为最终防线：

```sql
INSERT INTO consumer_inbox(message_id, payload_json, received_at_ms)
VALUES (?, ?, ?)
ON CONFLICT(message_id) DO NOTHING;
```

处理规则：

- 第一次出现：写入 inbox，并在同一事务中写业务表。
- 同 ID、同 payload：判为重复，直接返回成功，不重复执行业务副作用。
- 同 ID、不同 payload：判为冲突并报错，不能静默吞掉数据污染。

因此端到端语义是“投递至少一次，业务结果至多一次”。

## 8. STM32 Modbus 正式数据源

### 8.1 固定设备协议

当前 STM32 协议参数：

- 从站地址：1
- 功能码：04（读输入寄存器）
- 起始寄存器：1
- 寄存器数量：2
- 正常响应长度：9 字节
- register 0：温度 × 10
- register 1：湿度 × 10

真实请求和响应示例：

```text
Request : 01 04 00 01 00 02 20 0B
Response: 01 04 04 01 46 01 5E 9B C5
```

解析结果为温度 32.6°C、湿度 35.0%。

### 8.2 共用截止时间

接收先读 3 字节头，再根据正常帧或异常帧确定剩余长度。两次读取必须使用同一个绝对 deadline，而不是每段重新获得一份 timeout；否则慢速分段响应会突破调用方配置的总时限。

完整响应随后交给已有 Modbus 校验函数，检查 CRC、站号、功能码、寄存器数量和异常响应。超时、坏帧、异常帧、停止取消返回 `NO_DATA`；不可恢复 I/O 错误返回 `ERROR`。

### 8.3 一次事务映射两条 Measurement

一个响应同时携带温湿度。Source 接口每次 `next()` 只返回一条，所以采用单项 pending 缓存：

1. 第一次 `next()` 执行 Modbus 事务，构造同时间戳、相邻序列的温度和湿度。
2. 返回温度，把湿度按值缓存。
3. 第二次 `next()` 直接返回缓存，不再访问串口。
4. 下一次调用才发起新的 Modbus 请求。

两条记录都先在局部数组中完成构造和 `measurement_validate()`，全部有效后才交付，避免只生成半组数据。当前质量标记为 `UNCERTAIN`，因为还没有设备侧质量位或独立校准证据。

### 8.4 资源生命周期

`stm32_modbus_source_open()` 打开 UART、申请 GPIO 并初始化序列和缓存；任一步失败都不留下新资源。`close()` 关闭 UART/GPIO、清空缓存和停止回调，重复调用安全。

Gateway 配置新增 `source_kind` 和串口/GPIO参数。命令：

```sh
./gateway --source stm32 ./gateway.db
```

启动时先打开 Store，再根据 source kind 选择模拟源或 STM32 源；worker 全部退出后先关闭 STM32 资源，再关闭 Store 和日志。

## 9. 测试策略

### 9.1 确定性单元测试

PTY 测试端模拟 STM32：

- 检查 Gateway 发出的 8 字节请求完全正确。
- 返回真实的 9 字节响应样本。
- 模拟 GPIO/发送完成依赖。
- 验证温度 32.6、湿度 35.0、序列 1/2、相同时间戳。
- 验证第二条来自缓存，而不是第二次串口事务。
- 验证 close 释放资源。

这样可以稳定覆盖协议和映射逻辑，不依赖硬件在线状态。

### 9.2 构建矩阵

- Storage + MQTT 联合严格构建。
- Storage-only 构建。
