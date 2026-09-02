# W06（D36–D42）完成情况严格审计与后续计划

- 审计日期：2026-09-02
- 工程：`/home/zoukunbo/project/edgevision-gateway`
- 审计范围：D36～D42 的 Notion 原始任务、现有源码、构建目标、测试结果、现场日志和部署文档
- 本文口径：以每个任务原始 DoD（完成定义）和可复查证据为准，不把“示例跑通”直接写成“任务验收完成”

## 1. 先说结论

这周不是“什么都没做”。真实 RS485、Modbus、Measurement、MQTT、SQLite、Outbox、NFS 和 systemd 都产生了有价值的结果，多个最小链路已经跑通。

但是，“最小学习闭环跑通”和“原任务严格验收完成”是两种状态。按原始 DoD 重新审计后：

- D36～D42 **全部应处于“进行中”**。
- D36 的 Notion“验收通过”被错误勾选，应取消。
- D39、D40、D41、D42 虽然各有最小闭环证据，但仍缺原始 DoD 中的关键项，不能保持“完成”。
- D37、D38 原本就是“进行中”，这个状态正确。
- SQLite/Outbox 目前是独立示例程序，还没有形成主工程可复用模块。
- 当前 systemd 启动的是一次性 Outbox 投递示例，不是常驻的正式 Gateway 主程序。
- 默认 `gateway` 仍使用 `simulated_source`，真实 STM32 Modbus 数据源没有接入默认主链。

因此，本周正确的表述应当是：

> 已完成多个最小可运行行为并留下证据；D36～D42 严格验收均未完成，后续按缺口逐项收口。

## 2. 为什么之前会出现“完成但未验收”

此前记录混用了四个层级：

1. **知识已学习**：理解了某个概念或读懂了一段代码。
2. **最小行为已验证**：某个演示程序在特定条件下跑通一次。
3. **工程能力已接入**：功能被封装成稳定接口，并由主程序调用。
4. **任务已验收**：原始 DoD 中要求的功能、可靠性、故障场景和证据全部满足。

例如，“一条历史 Measurement 经 SQLite Outbox 发到 Broker，并在 PUBACK 后改为 sent”证明第 2 层成立；它并不能自动证明可复用模块、主工程接入、并发处理、重启恢复、100 条消息、故障注入等第 3、4 层也成立。

今后统一使用下面的规则：

- Notion“状态=完成”：只有原始 DoD 全部满足时才能设置。
- “验收通过”：只有验收证据可复查、缺口清零时才能勾选。
- 最小闭环跑通：写在任务正文或证据栏中，不用“完成”状态代替。
- 示例程序与主程序必须分开描述。
- PC 模拟、PTY 测试和真实硬件证据必须分开描述。

## 3. 状态校正表

| 任务 | 审计前状态 | 严格审计后状态 | 验收 | 核心原因 |
| --- | --- | --- | --- | --- |
| D36 真实 RS485 设备接入与安全基线 | 完成 | **进行中** | **未通过** | 有通信证据，但缺完整电气安全记录、20 组真实事务和断电复现 |
| D37 termios 串口模块接入 Gateway | 进行中 | **进行中** | 未通过 | 串口能力和测试较完整，但真实可靠性统计、完整发送取消和主程序真实源仍缺 |
| D38 Modbus RTU 顺序事务与 Measurement 映射 | 进行中 | **进行中** | 未通过 | 一次真实 STM32→MQTT 成功，但缺 50 次统计、错误分类和正式数据源接入 |
| D39 Measurement 入库：SQLite WAL 与最小 Outbox | 完成 | **进行中** | 未通过 | 原子事务示例成立，但 WAL 未实现，存储层未模块化、未接主工程 |
| D40 Outbox 驱动 MQTT 与网关线程模型 | 完成 | **进行中** | 未通过 | PUBACK 后 sent 示例成立，但没有正式 worker/队列、100 条和故障窗口验收 |
| D41 Edge Gateway Lite v0.1 发布 | 完成 | **进行中** | 未通过 | ARM64、NFS、systemd 示例部署成立，但不是集成 Gateway，缺 30 分钟和故障注入 |
| D42 真实数据回放与 W07 可靠性基线 | 完成 | **进行中** | 未通过 | 一条历史 JSON 可审计回放成立，但四类 corpus、ReplayDataSource 和对照未完成 |

## 4. 逐项核查

### D36｜真实 RS485 设备接入与安全基线

已经完成的行为：

- PC 与板端真实 RS485 双向字节通信已有日志。
- UART5、115200、8N1 和 GPIO0_C6 方向控制已经实际使用。
- 已验证取消、关闭和重新打开串口的基本行为。
- 后续通过修正 TX 完成判断，真实 STM32 的 04 响应已被板端成功接收。
- 证据包括：
  - `hardware/rs485/2026-08-30-source-stop.log`
  - `hardware/rs485/2026-08-31-stm32-temt-no-delay-200506.log`
  - `docs/d36-closeout.md`

仍未完成的严格验收项：

- 没有形成完整的 VCC、电平、共地、A/B、终端电阻、偏置电阻安全检查表。
- 没有从断电接线开始留下照片或测量记录。
- 没有连续 20 组真实请求/响应统计。
- 没有完成断电、重新上电后的完整复现。
- 早期额外收到的 `01 83 03 01 31` 尚未独立解释。

结论：D36 有重要进展，但不能算完成，更不能勾选验收。

### D37｜termios 串口模块接入 Gateway

已经完成的行为：

- 串口打开、termios 配置、短读短写、`poll` 等基础能力已有实现。
- 收帧使用共享的单调时钟绝对截止时间，避免“读一段就重新计时”。
- EINTR、取消、关闭和重新打开已有测试。
- TX 方向切换不再只依赖 `tcdrain`，而是检查 `TIOCOUTQ` 与 `TEMT`。
- 2026-09-02 当前主机回归测试：**24/24 通过**。
- 证据包括：
  - `hardware/rs485/2026-08-31-tx-complete-ctest.log`
  - `hardware/rs485/2026-08-30-source-stop.log`
  - `docs/stm32-dht11-modbus-read.md`

仍未完成的严格验收项：

- `write_full` 仍可能阻塞，没有统一的完整发送截止时间和取消语义。
- 缺少至少 100 次真实串口事务的成功率、超时分布和错误统计。
- 设备拔出/断开后的 FD、线程和重新打开基线证据不完整。
- 默认 Gateway 没有使用真实串口数据源。

结论：底层串口能力比 D36 前明显成熟，但还没有达到原 D37 的完整验收。

### D38｜Modbus RTU 顺序事务与 Measurement 映射

已经完成的行为：

- 03/04 请求构造、正常响应长度和异常响应长度处理已有代码。
- CRC、从站地址、功能码、字节数和寄存器映射已有校验。
- 已建立真实 STM32 规则：从站 1，04，从地址 `0x0001` 读取 2 个输入寄存器。
- 请求 `01 04 00 01 00 02 20 0B` 曾收到响应 `01 04 04 01 05 01 18 EB E3`。
- 原始值 261/280 被映射为温度 26.1°C、湿度 28.0%RH，并形成两条 Measurement。
- 真实数据经现有 MQTT 出口到独立订阅端的最小验证已经成功。
- 证据包括：
  - `hardware/rs485/2026-08-31-modbus-pc.log`
  - `hardware/rs485/2026-08-31-modbus-live-mqtt.log`
  - `hardware/rs485/2026-08-31-stm32-temt-no-delay-200506.log`
  - `hardware/rs485/2026-08-31-stm32-live-mqtt-203058.log`

仍未完成的严格验收项：

- 缺少连续 50 次真实轮询、成功率不低于 98% 的统计证据。
- timeout、坏 CRC、异常响应、设备断开等错误分类没有完整现场证据。
- Modbus 查询和映射仍是 `examples/serial/modbus_rtu_demo.c` 内部的静态函数。
- 默认 Gateway 未接入正式 Modbus 数据源。
- 负温度补码、quality 和无效/过期数据规则还没有形成完整主工程行为。

结论：真实端到端最小链路成立，D38 的正式源模块和可靠性验收仍未完成。

### D39｜Measurement 入库：SQLite WAL 与最小 Outbox

已经完成的行为：

- 已演示创建 SQLite 数据库和表。
- 已演示在一个事务中同时插入 Measurement 和 pending Outbox。
- 已演示中途失败后回滚，避免只写入其中一张表。
- pending 记录可以跨进程、跨程序重新打开后继续存在。
- 证据：`hardware/storage/d39-transaction-verification-2026-09-01.log`。

仍未完成的严格验收项：

- 标题中的 **WAL 实际没有实现**。现有代码没有执行 `PRAGMA journal_mode=WAL`。
- 存储函数都位于 `examples/storage/*.c` 中，并且多数是 `static`，其他模块无法直接调用。
- 根 `CMakeLists.txt` 没有创建 `edgevision_storage`，主程序也没有链接 SQLite。
- D39 和 D40 示例的表结构存在重复和差异，没有统一 schema。
- 没有 schema 版本与迁移机制。
- 缺少 20 个事务、约束冲突、数据库繁忙/锁冲突和 100 条真实 Measurement 的验收。
- `examples/storage` 没有接入 CTest 的自动测试。

结论：已经理解并验证“事务性双写”的核心概念，但还不是可复用存储模块，也没有接入主工程。

### D40｜Outbox 驱动 MQTT 与网关线程模型

已经完成的行为：

- 可以从 pending 中取出最早一条消息。
- Broker 连接失败或未确认时，消息仍保持 pending。
- Broker 恢复后可以补发。
- QoS 1 的真实 PUBACK 到达后才把该记录更新为 sent。
- 独立订阅端曾收到对应消息。
- 证据包括：
  - `hardware/storage/d40-offline-delivery-verification-2026-09-01.log`
  - `hardware/storage/d40-real-mqtt-outbox-verification-2026-09-01.log`
  - `hardware/storage/d40-real-failure-recovery-verification-2026-09-01.log`

仍未完成的严格验收项：

- 当前代码还是 `sqlite_outbox_mqtt_demo`，没有可复用的 OutboxSender 接口。
- 没有主 Gateway 的常驻 worker 和有界队列。
- 当前一次只按 `ORDER BY id LIMIT 1` 取一条，systemd 也是 oneshot。
- 没有 100 条消息的顺序、吞吐和恢复验证。
- 没有“收到 PUBACK 后、写 sent 前进程崩溃”的重复窗口测试。
- 没有完整的 SIGTERM 小于等于 3 秒退出与 pending 一致性验证。
- 没有 attempt count、last error、退避、死信或稳定消息 ID 设计。

结论：Outbox 的核心可靠性语义已经通过一次真实 Broker 行为说明，但线程模型和正式模块尚未完成。

### D41｜Edge Gateway Lite v0.1 发布

已经完成的行为：

- 已进行干净的 ARM64 交叉构建。
- WSL NFSv4 只读导出、板端挂载和开机自动挂载已经验证。
- systemd 可以开机或手动启动 Outbox 投递服务。
- SQLite 数据库放在 `/userdata/edgevision-gateway/gateway.db`，重启后仍保留。
- 板端通过反向隧道访问 WSL Broker，独立订阅端收到历史数据。
- 板卡重启后挂载、服务和数据库终态曾恢复。
- 证据：
  - `hardware/storage/systemd-nfs-board-deployment-2026-09-01.log`
  - `docs/systemd-nfs-board-deployment-2026-09-01.md`

仍未完成的严格验收项：

- 部署的是 Outbox 单次投递演示，不是集成后的 `gateway`。
- systemd 服务类型是 `oneshot`，执行一次后退出，不是持续采集和投递的常驻服务。
- 缺少连续 30 分钟真实运行。
- 缺少串口设备故障、Broker 故障和 SIGTERM 三类正式故障注入。
- 缺少 schema 迁移、版本输出、健康状态和稳定配置文件。
- 尚未形成可审计的 Git v0.1 版本点；当前工作区还有用户未提交内容，不能擅自提交。

结论：部署方法最小验证成立，但产品形态的 Edge Gateway Lite v0.1 还没有发布完成。

### D42｜真实数据回放与 W07 可靠性基线

已经完成的行为：

- 一条固定历史 JSON 已形成可审计输入。
- 输入文件和证据日志有 SHA256。
- 该数据从 pending、PUBACK、sent、独立订阅直到重启终态可以逐项对应。
- 证据：
  - `docs/d42-auditable-replay-2026-09-01.md`
  - `hardware/storage/d42-auditable-replay-2026-09-01.log`

仍未完成的严格验收项：

- 缺少 normal、timeout、CRC 错误、设备断开四类原始 corpus。
- 没有可复用的 ReplayDataSource。
- 回放没有经过与真实源相同的默认 Gateway 主链。
- 没有真实运行与回放运行的对照报告。
- 没有形成 W07 Top-3 可靠性风险及对应基线。
- 缺少一键 smoke 验证。

结论：一条历史 JSON 的审计闭环成立，但 D42 的“数据源回放与可靠性基线”还没有完成。

## 5. 当前代码到底处于什么位置

### 5.1 SQLite/Outbox 现在位于示例层

相关文件位于：

- `examples/storage/sqlite_measurement_demo.c`
- `examples/storage/sqlite_outbox_demo.c`
- `examples/storage/sqlite_outbox_delivery_demo.c`
- `examples/storage/sqlite_outbox_mqtt_demo.c`
- `examples/storage/CMakeLists.txt`

这些程序适合学习和证明一个行为。它们的问题是：

- 数据库函数主要是文件内 `static` 函数；
- 每个示例自行定义表和 SQL；
- 没有公共头文件；
- 根工程没有 storage 库 target；
- `edgevision_core` 没有 SQLite/Outbox 依赖；
- 默认 Gateway 无法调用这些函数。

因此，现在可以说“有 SQLite/Outbox 示例”，不能说“已经形成可复用模块并接入主工程”。

### 5.2 Modbus 现在也主要位于示例层

`examples/serial/modbus_rtu_demo.c` 内包含请求构造、事务收帧、校验和 Measurement 映射。它验证了真实设备，但这些函数没有成为正式 `modules/source` 数据源。

默认 `gateway` 的现状：

- MQTT smoke 路径使用 `simulated_source`；
- 默认 service 路径主要启动日志后等待 SIGINT/SIGTERM；
- `real_serial_source_placeholder()` 仍返回 NO_DATA。

所以“真实 STM32 数据曾成功发布”与“主程序已经持续读取 STM32”不能混为一谈。

### 5.3 systemd 现在启动什么

当前板端 `edgevision-outbox.service` 调用 `run-outbox-once.sh`，脚本再执行：

`sqlite_outbox_mqtt_demo deliver-mqtt`

它会打开数据库，选择最早的一条 pending，尝试投递，并在 PUBACK 后写 sent，然后退出。

这证明了：

- 开机服务能被 systemd 调用；
- NFS 上的程序能运行；
- `/userdata` 的数据库能持久保存；
- Broker 可达时能投递。

它尚未证明：

- Gateway 会常驻；
- Gateway 会持续轮询 STM32；
- 每条 Measurement 都会自动入库；
- Outbox 会持续消费全部 pending；
- 出现故障后会按正式策略恢复。

## 6. 2026-09-02 非硬件复查结果

本次审计没有发送 STM32、UART 或 Broker 请求，只进行了本地可重复检查：

- `build-d36-closeout` 构建成功。
- CTest：**24/24 通过**，总耗时约 10.90 秒。
- `build-d39-sqlite` 中四个 storage 示例目标均能构建：
  - `sqlite_measurement_demo`
  - `sqlite_outbox_demo`
  - `sqlite_outbox_delivery_demo`
  - `sqlite_outbox_mqtt_demo`

这只能证明现有代码仍能构建、现有非硬件回归未退化，不能替代现场验收。

## 7. 可复用模块的落地设计

### 阶段 1：建立正式 Storage/Outbox 库

建议新增：

- `modules/storage/outbox_store.h`
- `modules/storage/outbox_store.c`
- `modules/storage/schema.c`

对外只暴露清楚的接口，例如：

```c
int outbox_store_open(outbox_store_t *store, const char *db_path);
void outbox_store_close(outbox_store_t *store);

int outbox_store_save_measurement(
    outbox_store_t *store,
    const measurement_t *measurement);

int outbox_store_load_oldest_pending(
    outbox_store_t *store,
    outbox_message_t *message);

int outbox_store_mark_sent(
    outbox_store_t *store,
    int64_t outbox_id);

int outbox_store_mark_failed(
    outbox_store_t *store,
    int64_t outbox_id,
    const char *reason);

int outbox_store_get_stats(
    outbox_store_t *store,
    outbox_stats_t *stats);
```

`save_measurement()` 内部统一完成：

1. 开始事务；
2. 插入 Measurement；
3. 序列化 MQTT JSON；
4. 插入 pending Outbox；
5. 提交；任何一步失败则回滚。

根 `CMakeLists.txt` 新增 `edgevision_storage`，示例程序和主程序都链接这一库。先让现有示例改用公共接口，确保模块不是“为了主程序重新复制一份代码”。

### 阶段 2：统一 schema 和迁移

第一版 schema 至少应包含：

- measurements：设备、指标、值、单位、quality、采集时间；
- outbox：measurement_id、topic、payload、state、attempt_count、last_error、created_at、sent_at；
- `PRAGMA user_version`：记录 schema 版本；
- `PRAGMA journal_mode=WAL`：实际启用 WAL，并检查返回值；
- `PRAGMA foreign_keys=ON`；
- 合理的 `busy_timeout`；
- `(state, id)` 索引；
- 稳定的 event_id 或幂等键，帮助订阅端识别重复。

必须测试：提交、回滚、关闭重开、空 pending、PUBACK 前不 sent、迁移和重复键。

### 阶段 3：建立正式 Modbus 数据源

建议新增：

- `modules/source/modbus_rtu_source.h`
- `modules/source/modbus_rtu_source.c`

把示例中的查询、收帧、校验和映射移入正式数据源。一次 04 请求会得到温度与湿度两个指标，而现有 Source 接口一次返回一条 Measurement，可以先使用一个简单的两项缓存：

1. 第一次 `next()` 执行一次 Modbus 事务；
2. 立即返回温度；
3. 把同一响应的湿度暂存；
4. 下一次 `next()` 返回暂存的湿度，不再次查询设备。

这样两条 Measurement 共享同一个采集时间，也不会为了两个指标重复发送请求。

需要使用 PTY、fake transport 或 replay 做确定性测试：正常响应、timeout、坏 CRC、异常响应和设备断开。真实硬件只用于最终验收，不应成为每次编译后的唯一测试手段。

### 阶段 4：接入 Gateway 主链

第一版采用容易理解和排错的单存储线程模型：

```text
ModbusRtuSource
      │
      ▼
 Measurement
      │
      ▼
outbox_store_save_measurement()
  ├─ measurements 行
  └─ outbox pending 行
      │
      ▼
 Outbox worker
      │
      ▼
mqtt_publisher_publish(QoS 1)
      │
      ▼
   PUBACK
      │
      ▼
outbox_store_mark_sent()
```

建议一个 Storage/Outbox worker 独占一个 SQLite 连接，其他线程通过有界队列提交命令。这样初学阶段不需要处理多个线程同时使用同一个 `sqlite3 *` 的生命周期和锁问题。

如果以后确实需要多连接，再遵守：

- 每个线程使用自己的连接；
- 每个连接单独设置 foreign_keys、busy_timeout；
- WAL 允许读者与一个写者较好地并行，但不意味着支持任意多个写者；
- 业务层仍要处理 SQLITE_BUSY 和重试上限。

### 阶段 5：把 systemd 从演示切换到正式 Gateway

正式 Gateway 完成后，systemd 应启动常驻 `gateway`，配置至少包括：

- 数据库路径；
- 串口设备和波特率；
- RS485 GPIO；
- 从站地址、功能码、寄存器；
- Broker 地址、端口、topic、QoS；
- 轮询周期、超时和重试；
- 日志级别。

现在的 oneshot 示例可以保留，名字明确写成 diagnostic 或 deliver-once，用于迁移和手工排错；它不再代表正式服务。

## 8. 后续学习与实现顺序

每天主动学习与实现控制在 6 小时以内。一次只关闭一个明确风险，避免同时铺开多个模块。

### 第 0 步：D36 严格收口（约 2～3 小时现场时间）

目标：

- 完成电气安全检查表；
- 从断电接线开始记录；
- 连续 20 次真实事务，记录成功、timeout、CRC 和异常帧；
- 断电重启后复现；
- 若额外 `01 83 03 01 31` 再出现，记录时间、总线参与者和原始上下文。

完成标准：D36 原始 DoD 逐项有证据后，才把状态改为完成并勾验收。

### 第 1 步：收口 D37/D38 的正式串口和 Modbus 源

目标：

- 给写操作补统一超时/取消语义；
- 抽取 `modbus_rtu_source`；
- 建立五类确定性测试；
- 做真实 50～100 次事务统计；
- 接入 Gateway 的 Source 接口。

完成标准：主程序可选择真实 Modbus 源，且非硬件测试与真实可靠性证据都满足 DoD。

### 第 2 步：优先完成 D39 正式 Storage/Outbox 模块

这是当前最适合先编码的部分，因为不依赖硬件，且是 D40/D41 的基础。

目标：

- 统一 schema；
- 真正启用 WAL；
- 增加 schema 版本和迁移；
- 建立 `edgevision_storage`；
- 把四个示例改用公共接口；
- 加入有意义的事务、重开、迁移和失败测试。

完成标准：主工程和示例只通过公共 API 使用数据库，不再复制 SQL 和状态机。

### 第 3 步：完成 D40 常驻 Outbox worker

目标：

- 有界队列；
- 单 SQLite 所有者；
- pending 顺序投递；
- PUBACK 后 sent；
- Broker 断开恢复；
- SIGTERM 安全退出；
- 100 条消息和 ACK 崩溃窗口测试；
- 订阅端按 event_id 去重的设计说明。

完成标准：Outbox 行为由正式模块和自动测试保证，而不是只靠一次演示。

### 第 4 步：完成 D41 集成部署

目标：

- ARM64 构建正式 `gateway`；
- systemd 启动常驻服务；
- NFS 继续作为开发期程序载体，`/userdata` 保存可写状态；
- 补 30 分钟运行；
- 补串口、Broker、SIGTERM 三类故障；
- 输出版本、配置和健康状态；
- 在用户明确决定版本策略后建立 Git v0.1 版本点。

完成标准：板端开机后无需手工运行演示程序，真实采集、入库和补发链持续工作。

### 第 5 步：完成 D42 回放与 W07 基线

目标：

- 建立 normal、timeout、CRC error、disconnect 四类 corpus；
- 实现 ReplayDataSource；
- 让回放与真实源经过同一条 Gateway 主链；
- 对比真实与回放输出；
- 形成一键 smoke；
- 总结 W07 Top-3 风险及下一周计划。

完成标准：故障能在没有硬件时稳定复现，且结果可与真实现场证据对应。

## 9. 推荐的最近一步

先不要直接扩大到常驻 worker 或全面部署。最近一次编码应从 **D39 的正式 Storage/Outbox 模块** 开始，原因是：

- 它不依赖现场硬件；
- 它能消除当前重复 schema 和静态函数；
- D40 worker、D41 主程序部署都依赖它；
- 可以用自动测试给可靠性打基础。

与此同时，D36 的严格验收需要下一次具备硬件条件时按第 0 步完成。这样软件工作可以继续推进，但不会再把 D36 误记为完成。

## 10. 本周真正学到的内容

本周已经形成的能力包括：

- 能分清 RS485 电气层、UART 字节流和 Modbus RTU 协议层；
- 能解释正常帧、异常帧、CRC、共享截止时间和短读；
- 能把两个寄存器映射成两条领域 Measurement；
- 能理解 SQLite 事务如何避免 Measurement 与 Outbox 半写入；
- 能解释 pending、PUBACK、sent 的可靠投递关系；
- 能认识 at-least-once 仍可能重复；
- 能完成 ARM64 交叉构建，并理解 NFS 只读程序与 `/userdata` 可写状态的分工；
- 能理解 systemd 的开机启用、手动启动、oneshot 和常驻服务差别；
- 能区分“示例跑通”“模块可复用”“主工程接入”“严格验收”四种工程状态。

这些学习成果应保留；状态修正是为了让证据和任务定义一致，不是否定已经完成的学习。
