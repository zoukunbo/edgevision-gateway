# 从 STM32 温湿度到可靠 MQTT：Modbus、SQLite 与 Outbox 初学者教程

- 适用工程：`/home/zoukunbo/project/edgevision-gateway`
- 编写日期：2026-09-02
- 面向读者：已经接触 C、串口和 CMake，但还没有系统使用 SQLite/Outbox 的初学者
- 学习目标：看完后能说清一条温湿度数据怎样从 STM32 到 MQTT，以及 SQLite/Outbox 为什么存在、现有代码做到哪一步、正式模块应该怎样接入主工程

## 1. 先建立整条链路

这个项目要做的事情，可以先用一句话描述：

> Linux 网关向 STM32 发起 Modbus 请求，收到温湿度后形成统一的 Measurement，先可靠保存到 SQLite，再由 Outbox 投递到 MQTT Broker，订阅端最终收到消息。

整条链路如下：

```text
DHT11
  │  STM32 读取传感器
  ▼
STM32 寄存器
  │  Modbus RTU 响应
  ▼
RS485 总线
  │  UART 字节流
  ▼
Linux 串口模块
  │  收完整帧、校验
  ▼
Modbus 数据源
  │  寄存器 → 温度/湿度
  ▼
Measurement
  │  一个数据库事务
  ├──────────────┐
  ▼              ▼
measurements   outbox(pending)
                  │
                  ▼
             MQTT 发布器
                  │ QoS 1 PUBACK
                  ▼
             outbox(sent)
                  │
                  ▼
             独立订阅端
```

这里每一层只负责一类问题：

| 层 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| RS485 | 电气信号、A/B 差分、收发方向 | 不知道温度是什么 |
| UART/termios | 波特率、8N1、读写字节、超时 | 不判断寄存器含义 |
| Modbus RTU | 地址、功能码、长度、CRC、异常码 | 不保存数据库 |
| Measurement | 用统一结构表达设备、指标、值、单位、时间 | 不关心串口字节 |
| SQLite | 本机持久保存和事务 | 不把消息送过网络 |
| Outbox | 记录“哪些消息还没可靠送达” | 不读取传感器 |
| MQTT | 把消息发到 Broker 和订阅端 | 不保证数据库里的业务双写 |
| systemd | 开机启动、重启策略、日志和服务生命周期 | 不替代程序内部业务逻辑 |

分层的价值是：一个错误出现时，你能判断它属于哪一层。比如 CRC 错误属于 Modbus 层，`SQLITE_BUSY` 属于存储层，Broker 连接失败属于 MQTT 层。

## 2. RS485、UART 和 Modbus 不是同一件事

初学时最容易把这三个词混在一起。

### 2.1 RS485 是电气层

RS485 规定的是电线上的差分信号。它关心：

- A/B 线怎么连接；
- 两端是否共地；
- 收发器电压是否安全；
- 是否需要终端电阻和偏置；
- 半双工时什么时候 TX，什么时候 RX。

本项目板端用 GPIO 控制 SP3485 一类收发器的方向：

- 发送前切到 TX；
- UART 真正发完后才能切回 RX；
- 如果切回太晚，STM32 已经回复，Linux 仍在驱动总线，就会漏掉响应。

这正是此前 `tcdrain` 路径暴露的问题。后来增加 `TIOCOUTQ=0` 和 `TEMT` 检查，是为了确认 UART 队列和移位寄存器都已经空了，再释放总线。

注意：串口工具能看到字节，不等于电气安全已经验收。电压、共地、接线和终端仍需单独检查。

### 2.2 UART/termios 是字节流层

Linux 打开的 `/dev/ttyS5` 是串口设备。termios 用来设置：

- 波特率：115200；
- 数据位：8；
- 校验位：无；
- 停止位：1；
- 原始模式，不让终端驱动修改字节。

串口是“流”，没有“这一读恰好是一帧”的保证。例如设备要回 9 个字节：

- 第一次 `read()` 可能只得到 2 个；
- 第二次得到 5 个；
- 第三次才得到剩余 2 个。

所以代码需要“读满指定长度”，而不能假定一次 `read()` 就拿到全部响应。

### 2.3 Modbus RTU 是字节含义

Modbus RTU 规定这些字节怎样解释：

- 第 1 字节：从站地址；
- 第 2 字节：功能码；
- 中间字节：寄存器地址、数量或数据；
- 最后 2 字节：CRC16，低字节先发送。

它运行在 UART 字节流上，但 UART 本身并不知道这些含义。

## 3. 用真实请求理解 Modbus RTU

本项目曾使用请求：

```text
01 04 00 01 00 02 20 0B
```

逐字段解释：

| 字节 | 含义 |
| --- | --- |
| `01` | 从站地址 1，也就是目标 STM32 |
| `04` | 功能码 04，读取输入寄存器 |
| `00 01` | 起始地址 0x0001 |
| `00 02` | 读取 2 个寄存器 |
| `20 0B` | 前 6 字节的 Modbus CRC，低字节在前 |

为什么是 2 个寄存器？因为一个 16 位寄存器保存温度原始值，另一个保存湿度原始值。

真实正常响应曾为：

```text
01 04 04 01 05 01 18 EB E3
```

逐字段解释：

| 字节 | 含义 |
| --- | --- |
| `01` | 响应来自从站 1 |
| `04` | 响应功能码 04 |
| `04` | 后面有 4 个数据字节 |
| `01 05` | 第一个寄存器，十进制 261 |
| `01 18` | 第二个寄存器，十进制 280 |
| `EB E3` | 响应 CRC，低字节在前 |

STM32 约定原始值扩大了 10 倍，因此：

```text
temperature = 261 / 10.0 = 26.1 °C
humidity    = 280 / 10.0 = 28.0 %RH
```

### 3.1 功能码 03 和 04 的区别

- 03：读取 Holding Registers，通常表示可保存或可配置的数据。
- 04：读取 Input Registers，通常表示只读输入或测量值。

协议在字节结构上很相似，但寄存器类别不同。本项目真实 STM32 温湿度使用 04。

### 3.2 正常响应和异常响应

正常 04 响应的前 3 个字节是：

```text
slave function byte_count
```

读到 `byte_count` 后，程序才知道还要读多少数据和 2 字节 CRC。

异常响应会把功能码最高位置 1。例如请求 04，异常功能码是 84：

```text
01 84 04 xx xx
```

其中 `04` 是异常码，最后两个字节是 CRC。异常帧总长度通常是 5 字节。

因此收帧可以分两段：

1. 先读 3 个字节；
2. 检查功能码：
   - 正常：根据 byte_count 再读数据和 CRC；
   - 异常：再读异常码后的 2 字节 CRC。

### 3.3 为什么两段读取必须共享一个截止时间

假设整个响应预算是 1000 ms。错误做法是：

- 读头部允许 1000 ms；
- 读剩余部分又允许 1000 ms。

这样总时间可能接近 2000 ms。

正确做法是在发送完成后计算一次绝对截止时间：

```text
deadline = CLOCK_MONOTONIC 当前时间 + 1000 ms
```

每次等待都计算“离同一个 deadline 还剩多少”。这样无论分几次读取，总预算仍是 1000 ms。

`CLOCK_MONOTONIC` 不受用户修改系统日期影响，适合超时计算。

## 4. 一帧数据要经过三层校验

收到字节不等于得到有效温度。建议按三层理解。

### 4.1 通信有效

检查：

- 是否在截止时间内收齐；
- 是否出现系统读写错误；
- 串口是否被关闭或取消；
- 是否发生设备断开。

### 4.2 协议有效

检查：

- 从站地址是否等于请求目标；
- 功能码是否匹配，或是否为对应异常码；
- byte_count 是否符合读取数量；
- CRC 是否正确；
- 响应长度是否正确。

CRC 正确只表示传输后的字节彼此一致，不能证明温度在合理范围。

### 4.3 业务有效

检查：

- 温度、湿度是否落在项目允许范围；
- 负温度是否按 int16_t 补码解释；
- STM32 是否报告采样失败或数据过期；
- quality 应该是 good、uncertain 还是 bad；
- 时间戳是否合理。

这三层要给出不同错误，排错时才能知道是线路、协议还是传感器数据问题。

## 5. 从寄存器变成 Measurement

协议层得到的是“两个 16 位数”。主工程不应该让 MQTT、SQLite 都重新理解寄存器地址，所以需要统一领域结构 Measurement。

一个简化结构可以是：

```c
typedef struct {
    char device_id[64];
    char metric[32];
    double value;
    char unit[16];
    char quality[16];
    int64_t timestamp_ms;
} measurement_t;
```

同一次 Modbus 响应形成两条记录：

```json
{
  "device_id": "stm32-dht11-01",
  "metric": "temperature",
  "value": 26.1,
  "unit": "C",
  "quality": "uncertain",
  "timestamp_ms": 1788179459579
}
```

```json
{
  "device_id": "stm32-dht11-01",
  "metric": "humidity",
  "value": 28.0,
  "unit": "%RH",
  "quality": "uncertain",
  "timestamp_ms": 1788179459579
}
```

两条记录最好使用相同 `timestamp_ms`，因为它们来自同一次设备响应。

Measurement 的作用是隔离变化：

- 将来 STM32 地址或寄存器变化，只改 Modbus 映射；
- 将来 MQTT JSON 改格式，不改串口模块；
- SQLite 只保存 Measurement，不需要懂 CRC；
- 回放数据源也能产生相同 Measurement，复用后续主链。

## 6. SQLite 是什么，为什么网关适合用它

SQLite 是一个嵌入式关系数据库。它没有单独的数据库服务器进程，程序通过 `libsqlite3` 直接读写一个数据库文件。

例如：

```text
/userdata/edgevision-gateway/gateway.db
```

这个文件可以包含多张表、索引和事务日志。

它适合网关的原因：

- 部署简单，不需要安装 MySQL 服务；
- 断电或进程重启后数据仍在；
- 支持事务，能把多个 SQL 操作作为一个整体；
- C 接口成熟；
- 数据量中小时容易查看和排错。

SQLite 不是“把结构体直接写进文件”。程序仍要定义表、列、约束和 SQL。

## 7. SQLite 最基本的使用步骤

C 程序通常按下面顺序工作：

```text
sqlite3_open()
    │
    ├─ 建表或迁移 schema
    │
    ├─ sqlite3_prepare_v2()
    │       │
    │       ├─ sqlite3_bind_*()
    │       ├─ sqlite3_step()
    │       └─ sqlite3_finalize()
    │
sqlite3_close()
```

### 7.1 打开数据库

```c
sqlite3 *db = NULL;

int rc = sqlite3_open(db_path, &db);
if (rc != SQLITE_OK) {
    fprintf(stderr, "open failed: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return -1;
}
```

如果文件不存在，SQLite 会尝试创建它。路径的父目录必须存在，而且运行用户需要写权限。

在板端，程序本体可以位于只读 NFS，但数据库不能放在只读 NFS。当前选择 `/userdata/edgevision-gateway/gateway.db`，就是把“只读程序”和“可写运行数据”分开。

### 7.2 执行固定 SQL

建表或 PRAGMA 可以用 `sqlite3_exec()`：

```c
const char *sql =
    "CREATE TABLE IF NOT EXISTS measurements ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  device_id TEXT NOT NULL,"
    "  metric TEXT NOT NULL,"
    "  value REAL NOT NULL,"
    "  unit TEXT NOT NULL,"
    "  quality TEXT NOT NULL,"
    "  timestamp_ms INTEGER NOT NULL"
    ");";

char *errmsg = NULL;
int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
if (rc != SQLITE_OK) {
    fprintf(stderr, "sql failed: %s\n", errmsg);
    sqlite3_free(errmsg);
}
```

这里：

- `INTEGER PRIMARY KEY AUTOINCREMENT`：自动生成唯一 id；
- `TEXT NOT NULL`：文本列不能为空；
- `REAL`：保存浮点值；
- `INTEGER`：可保存毫秒时间戳。

### 7.3 带变量的 SQL 要 prepare 和 bind

不要用 `sprintf()` 把 device_id 或 JSON 直接拼进 SQL。引号和特殊字符很容易造成错误，也会带来 SQL 注入风险。

正确方式：

```c
const char *sql =
    "INSERT INTO measurements"
    "(device_id, metric, value, unit, quality, timestamp_ms)"
    "VALUES (?, ?, ?, ?, ?, ?);";

sqlite3_stmt *stmt = NULL;
int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
if (rc != SQLITE_OK) {
    return -1;
}

sqlite3_bind_text(stmt, 1, m->device_id, -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 2, m->metric, -1, SQLITE_TRANSIENT);
sqlite3_bind_double(stmt, 3, m->value);
sqlite3_bind_text(stmt, 4, m->unit, -1, SQLITE_TRANSIENT);
sqlite3_bind_text(stmt, 5, m->quality, -1, SQLITE_TRANSIENT);
sqlite3_bind_int64(stmt, 6, m->timestamp_ms);

rc = sqlite3_step(stmt);
sqlite3_finalize(stmt);

if (rc != SQLITE_DONE) {
    return -1;
}
```

理解这四步：

1. `prepare`：让 SQLite 编译 SQL，产生 statement；
2. `bind`：把 C 变量绑定到问号；
3. `step`：真正执行；
4. `finalize`：释放 statement。

即使执行失败，也必须 `finalize`，否则会泄漏资源或保留锁。

### 7.4 查询数据

查询时，`sqlite3_step()` 可能返回：

- `SQLITE_ROW`：当前有一行；
- `SQLITE_DONE`：没有更多行；
- 其他：发生错误。

例如取最早一条 pending：

```sql
SELECT id, topic, payload
FROM outbox
WHERE state = 'pending'
ORDER BY id
LIMIT 1;
```

`ORDER BY id LIMIT 1` 表示按产生顺序只取最早一条。当前演示程序就是这种单条行为。

## 8. 事务：让多个写操作一起成功或一起失败

假设网关收到一条 Measurement，需要：

1. 在 measurements 表保存测量值；
2. 在 outbox 表保存将来要发送的 MQTT 消息。

如果不使用事务，可能出现：

- measurements 插入成功；
- 程序在写 outbox 前崩溃；
- 重启后数据库里有测量值，却没有任何待发消息；
- 这条数据永远不会发送。

也可能反过来只产生 outbox，却找不到对应测量记录。

事务的目标是原子性：这两步要么都生效，要么都不生效。

### 8.1 基本事务结构

```sql
BEGIN IMMEDIATE;

INSERT INTO measurements (...);

INSERT INTO outbox (..., 'pending');

COMMIT;
```

任一步失败：

```sql
ROLLBACK;
```

对应的 C 伪代码：

```c
if (exec_sql(db, "BEGIN IMMEDIATE;") != 0) {
    return -1;
}

if (insert_measurement(db, m, &measurement_id) != 0) {
    exec_sql(db, "ROLLBACK;");
    return -1;
}

if (insert_outbox(db, measurement_id, topic, payload) != 0) {
    exec_sql(db, "ROLLBACK;");
    return -1;
}

if (exec_sql(db, "COMMIT;") != 0) {
    exec_sql(db, "ROLLBACK;");
    return -1;
}
```

注意：

- 只有 `COMMIT` 成功后，业务才算保存成功；
- 不能忽略 `COMMIT` 的错误；
- 失败路径都要回滚；
- 序列化 JSON 最好在进入事务前完成，减少持锁时间；
- 如果 JSON 序列化失败，就根本不要开始写数据库。

## 9. WAL 是什么，当前项目有没有做到

SQLite 常见的日志模式有 rollback journal 和 WAL。

WAL 是 Write-Ahead Logging，直译为“预写日志”。开启后，修改先追加到 `gateway.db-wal`，之后再通过 checkpoint 合并回主数据库文件。

可以把它想成：

```text
读者 ───────────────→ 读取稳定快照
写者 ─→ gateway.db-wal ─→ checkpoint ─→ gateway.db
```

它的常见好处：

- 一个写者写入时，读者通常仍可读取旧快照；
- 顺序追加日志通常适合网关这种持续小写入；
- 崩溃恢复由 SQLite 处理。

WAL 不代表：

- 可以无限多个写线程同时写；
- 再也不会出现 `SQLITE_BUSY`；
- 不需要事务；
- 数据库文件可以随意放在不可靠的网络文件系统上；
- 只要文档写了 WAL，代码就自动启用。

### 9.1 真正启用 WAL

连接打开后，需要实际执行：

```sql
PRAGMA journal_mode=WAL;
```

并检查返回结果是否真的是 `wal`。还应按需要设置：

```sql
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=3000;
```

其中：

- `foreign_keys=ON`：启用外键约束；它通常需要每个连接单独设置；
- `busy_timeout=3000`：遇到短暂锁竞争时最多等待约 3 秒，而不是立即失败。

### 9.2 当前项目的真实状态

当前 D39 示例完成了事务性 Measurement+Outbox 写入和回滚演示，但代码中没有 `PRAGMA journal_mode=WAL`。

因此当前准确表述是：

- SQLite 事务示例：已实现；
- 最小 Outbox：已实现；
- WAL：**尚未实现和验证**；
- 正式 storage 模块：尚未实现。

不能因为 D39 标题带 “WAL” 就认定代码已经启用 WAL。

## 10. Outbox 解决什么问题

### 10.1 直接“先入库、再发 MQTT”的问题

最直观的代码是：

```text
保存 Measurement 成功
发布 MQTT
```

如果程序在两步之间崩溃，数据库有数据但 MQTT 没发，消息丢失。

反过来：

```text
先发布 MQTT
再保存 Measurement
```

如果 MQTT 已发出，程序随后崩溃，订阅端收到消息，但本地没有记录，审计不一致。

这叫“双写问题”：一次业务动作要写两个互相独立的系统，无法用一个普通事务同时保证 SQLite 和 MQTT Broker。

### 10.2 Transactional Outbox 的核心

不要在保存 Measurement 的事务中直接要求 MQTT 成功。改成在同一个 SQLite 事务里写两张本地表：

```text
BEGIN
  INSERT measurements
  INSERT outbox(state='pending')
COMMIT
```

只要 COMMIT 成功，系统就明确知道：

- 测量值已经保存；
- 对应消息还需要发送；
- 即使进程立刻退出，pending 仍留在数据库。

另一个 Outbox worker 负责网络投递：

```text
读取 pending
    │
    ├─ Broker 连接失败 ─→ 保持 pending
    │
    ├─ publish 失败 ─────→ 保持 pending
    │
    ├─ 等待 PUBACK 超时 ─→ 保持 pending
    │
    └─ 收到 PUBACK ──────→ 更新为 sent
```

这就是 Outbox 的价值：网络暂时不可用时，本地业务数据和待办消息不会消失。

## 11. 为什么要等 PUBACK 才能标记 sent

MQTT QoS 1 的基本交互是：

```text
Publisher                    Broker
    │                           │
    │──── PUBLISH (QoS 1) ─────>│
    │                           │
    │<──────── PUBACK ──────────│
    │                           │
```

调用 publish API 成功，通常只表示消息被客户端库接受或已开始发送，并不等于 Broker 已确认。

因此 Outbox 的状态转换应是：

```text
pending
   │
   ├─ 连接/发送/等待失败 ──→ pending
   │
   └─ 对应 message id 收到 PUBACK
                         │
                         ▼
                       sent
```

当前真实示例已经验证了“PUBACK 后 sent”这一核心行为。这是 D40 最有价值的成果之一。

### 11.1 QoS 1 仍可能重复

考虑这个时序：

1. Broker 收到消息；
2. Broker 返回 PUBACK；
3. 程序收到 PUBACK；
4. 程序还没来得及把 SQLite 更新为 sent 就崩溃；
5. 重启后数据库仍是 pending；
6. worker 再发一次。

订阅端可能收到两次同一业务消息。这是 at-least-once 语义：尽量不丢，但可能重复。

解决思路不是假装重复不存在，而是：

- 每个业务事件生成稳定的 `event_id`；
- 重试时复用同一个 event_id；
- 订阅端按 event_id 幂等处理或去重；
- 测试这个崩溃窗口；
- 文档明确系统承诺的是 at-least-once。

Outbox 加 QoS 1 不会自动变成 exactly-once。

## 12. 一个更完整的 schema 示例

下面是教学用的 v1 结构，正式实现时仍要结合现有 Measurement 字段核对。

```sql
CREATE TABLE IF NOT EXISTS measurements (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id       TEXT NOT NULL UNIQUE,
    device_id      TEXT NOT NULL,
    metric         TEXT NOT NULL,
    value          REAL NOT NULL,
    unit           TEXT NOT NULL,
    quality        TEXT NOT NULL,
    timestamp_ms   INTEGER NOT NULL,
    created_at_ms  INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS outbox (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    measurement_id   INTEGER NOT NULL,
    event_id         TEXT NOT NULL UNIQUE,
    topic            TEXT NOT NULL,
    payload          TEXT NOT NULL,
    qos              INTEGER NOT NULL DEFAULT 1,
    state            TEXT NOT NULL
                     CHECK (state IN ('pending', 'sent')),
    attempt_count    INTEGER NOT NULL DEFAULT 0,
    last_error       TEXT,
    created_at_ms    INTEGER NOT NULL,
    sent_at_ms       INTEGER,
    FOREIGN KEY (measurement_id)
        REFERENCES measurements(id)
);

CREATE INDEX IF NOT EXISTS idx_outbox_state_id
ON outbox(state, id);

PRAGMA user_version=1;
```

为什么 Outbox 自己保存 payload？

因为投递时应发送“当时事务中已经确定的消息”。如果每次重试都重新从 measurements 拼 JSON，程序升级、格式变化或浮点格式差异可能使同一事件重试出不同内容。

为什么同时保存 measurement_id 和 event_id？

- measurement_id 方便关联本地记录；
- event_id 是跨数据库、MQTT 和订阅端都稳定的业务标识；
- 数据库内部自增 id 不一定适合暴露到多设备系统中。

## 13. Outbox worker 怎样工作

初学阶段先使用单 worker，逻辑更容易正确：

```text
while (!stop_requested) {
    row = load_oldest_pending();

    if (没有 pending) {
        等待新消息或短暂休眠;
        continue;
    }

    if (Broker 未连接) {
        尝试连接;
        失败则记录错误并等待;
        continue;
    }

    发布 row.payload;
    等待该消息对应的 PUBACK;

    if (收到 PUBACK) {
        mark_sent(row.id);
    } else {
        保持 pending;
    }
}
```

生产代码还要回答：

- 一次失败后多久重试；
- 最大退避多久；
- SIGTERM 到来时是否停止领取新消息；
- 正在等待 PUBACK 时怎样退出；
- SQLite 更新失败怎样处理；
- 一条永久错误消息是否会阻塞后面的消息；
- 是否需要 dead letter；
- 日志里怎样关联 outbox_id、event_id 和 MQTT mid。

这些是 D40 后续要实现和验收的内容，当前示例尚未全部具备。

## 14. 多线程使用 SQLite 时先采用简单所有权

不建议让多个线程随意共享同一个 `sqlite3 *`。即使 SQLite 编译模式允许某些线程使用，业务层仍很难管理：

- 谁能开始事务；
- 谁在 finalize statement；
- 关闭时还有没有线程在使用；
- 锁冲突怎样重试；
- SIGTERM 时谁负责最后提交或回滚。

第一版建议：

```text
采集线程 ─┐
          ├─ 有界队列 ─→ Storage/Outbox 线程 ─→ SQLite
控制线程 ─┘
```

由一个线程拥有连接：

- 打开数据库；
- 处理保存 Measurement 命令；
- 读取和更新 Outbox；
- 退出前 finalize、回滚未完成事务、关闭连接。

有界队列是指容量固定。数据库或 Broker 长时间变慢时，队列不能无限占用内存。队列满时要有明确策略：阻塞一段时间、返回背压错误，或按业务规则处理，不能悄悄丢数据。

以后需要更高吞吐时，可以让每个线程有自己的 SQLite 连接，但仍需统一 schema、busy timeout、重试和关闭规则。

## 15. 当前四个 storage 示例分别教什么

### 15.1 sqlite_measurement_demo

作用：学习最基础的 SQLite 打开、建表、插入和查询 Measurement。

它证明“一个进程能把一条测量记录写入文件数据库”。

它没有证明事务性 Outbox 和 MQTT 投递。

### 15.2 sqlite_outbox_demo

作用：学习 Measurement 与 Outbox 在同一个事务中写入，以及失败回滚。

它证明“两个本地写操作可以一起成功或一起失败”。

### 15.3 sqlite_outbox_delivery_demo

作用：学习 pending 跨进程存在，以及投递后状态变化。

它把“保存”和“稍后处理”分开，帮助理解 Outbox 的持久待办性质。

### 15.4 sqlite_outbox_mqtt_demo

作用：把真实 MQTT QoS 1 确认接进状态机。

它验证：

- Broker 不可用时 pending 不丢；
- Broker 恢复可以补发；
- 收到 PUBACK 后才写 sent；
- 独立订阅端能收到。

这些文件都在 `examples/storage`，是学习用可执行程序。它们还不是正式库。

## 16. 为什么现在还不能叫“可复用模块”

一个可执行示例和一个模块的区别：

| 示例程序 | 可复用模块 |
| --- | --- |
| 有自己的 `main()` | 提供公共头文件和函数 |
| 函数可以都是 `static` | 公开稳定 API，隐藏内部细节 |
| 为演示复制 schema/SQL | 所有调用者共用一套 schema |
| 输入常写死在参数里 | 接受 Measurement、配置和回调 |
| 运行一次即可说明概念 | 要处理生命周期、错误和并发 |
| 不一定有自动测试 | 有独立模块测试 |
| 其他程序难以链接 | CMake target 可被主程序链接 |

当前情况：

- `examples/storage/CMakeLists.txt` 是独立子工程；
- storage 函数主要在各演示文件内部；
- D39/D40 schema 有重复和差异；
- 根工程没有 `edgevision_storage`；
- `gateway` 没有链接 SQLite；
- 默认 Gateway 没有调用 Outbox。

所以你的判断是正确的：SQLite 和 Outbox 尚未形成正式可复用模块，也没有接入主工程。

## 17. 正式模块应该怎样组织

推荐目录：

```text
modules/
  storage/
    outbox_store.h
    outbox_store.c
    schema.c
    schema.h
  source/
    modbus_rtu_source.h
    modbus_rtu_source.c
  net/
    mqtt_publisher.c
    mqtt_publisher.h
```

### 17.1 outbox_store.h 负责定义边界

外部模块只看到类型和 API，不看到具体 SQL：

```c
typedef struct outbox_store outbox_store_t;

typedef struct {
    int64_t id;
    char event_id[64];
    char topic[256];
    char *payload;
    int qos;
    int attempt_count;
} outbox_message_t;

int outbox_store_open(outbox_store_t **out, const char *db_path);
void outbox_store_close(outbox_store_t *store);

int outbox_store_save_measurement(
    outbox_store_t *store,
    const measurement_t *measurement);

int outbox_store_load_oldest_pending(
    outbox_store_t *store,
    outbox_message_t *message);

int outbox_store_mark_sent(
    outbox_store_t *store,
    int64_t outbox_id,
    int64_t sent_at_ms);
```

调用者不需要知道表名和 SQL。这能让将来迁移 schema 时不必修改 Gateway 各处。

### 17.2 outbox_store.c 负责 SQLite 细节

它负责：

- 打开/关闭；
- PRAGMA；
- schema 版本与迁移；
- prepare/bind/step/finalize；
- Measurement+Outbox 事务；
- pending 查询；
- sent/失败更新；
- 把 SQLite 错误转换为模块错误码。

### 17.3 schema.c 负责版本迁移

不要只写 `CREATE TABLE IF NOT EXISTS` 后永远不管。随着版本变化，数据库可能已有旧表。

可用 `PRAGMA user_version`：

```text
user_version = 0 → 创建 v1 表 → 设置为 1
user_version = 1 → 执行 v1 到 v2 迁移 → 设置为 2
```

每次迁移放在事务中，失败就回滚，并留下清楚日志。

## 18. CMake 怎样把 storage 接进主工程

根工程需要创建一个库 target，概念上类似：

```cmake
find_package(SQLite3 REQUIRED)

add_library(edgevision_storage
    modules/storage/outbox_store.c
    modules/storage/schema.c
)

target_include_directories(edgevision_storage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/modules/storage
)

target_link_libraries(edgevision_storage
    PUBLIC
        edgevision_core
    PRIVATE
        SQLite::SQLite3
)
```

然后 Gateway 链接它：

```cmake
target_link_libraries(gateway
    PRIVATE
        edgevision_storage
        edgevision_net
)
```

这里的意义是：

- `add_library` 编译出可复用库；
- `PUBLIC` include 让使用者能包含公共头文件；
- `SQLite::SQLite3` 只作为实现依赖；
- `target_link_libraries(gateway ...)` 才表示主程序真正接入。

只在 `examples/storage` 里有 `add_executable`，并不等于根工程的 `gateway` 会自动使用这些代码。

## 19. Modbus 数据源怎样接进 Gateway

当前 `modbus_rtu_demo.c` 有真实查询和映射能力，但它是单独可执行程序。正式接入应让它实现项目的 Source 接口。

概念流程：

```c
source_result_t modbus_rtu_source_next(
    modbus_rtu_source_t *source,
    measurement_t *out);
```

一次 04 响应同时产生温度和湿度，而 `next()` 一次只交付一条 Measurement。一个简单做法是内部缓存：

```text
第一次 next()
  ├─ 发一次 04 请求
  ├─ 收到温度和湿度
  ├─ 返回温度
  └─ 缓存湿度

第二次 next()
  ├─ 不访问串口
  └─ 返回缓存的湿度
```

第三次再开始新的 Modbus 请求。

这样不会为了两个指标连续向 STM32 发两次相同请求，也能保证它们共享采集时间。

正式数据源需要返回可区分的结果：

- OK：成功得到 Measurement；
- NO_DATA：当前没有数据，但不是故障；
- TIMEOUT：设备未在预算内响应；
- CRC_ERROR：帧完整但 CRC 错；
- PROTOCOL_ERROR：地址、功能码、长度不匹配或异常响应；
- DISCONNECTED：串口设备消失；
- CANCELLED：服务正在退出。

## 20. 主程序接入后的运行过程

完成模块化后，常驻 Gateway 启动应按顺序做：

1. 读取配置。
2. 初始化日志和停止信号。
3. 打开 SQLite，检查或迁移 schema，确认 WAL。
4. 打开串口并创建 Modbus 数据源。
5. 启动 Outbox worker。
6. 进入采集循环。
7. 每得到一条 Measurement，就调用 `outbox_store_save_measurement()`。
8. worker 从 pending 中取消息并通过 MQTT 发布。
9. 收到 PUBACK 后标 sent。
10. 收到 SIGTERM 后停止接收新工作，收尾线程，回滚未完成事务，关闭 SQLite、MQTT 和串口。

数据路径是：

```text
source_next()
    ↓
measurement
    ↓
save_measurement()  ← 此处只依赖本地数据库
    ↓
返回采集循环

Outbox worker 独立地：
pending → publish → PUBACK → sent
```

采集线程不应该因为 Broker 临时离线而丢掉 Measurement。只要本地事务成功，网络投递可以稍后恢复。

## 21. systemd 在这里负责什么

systemd 负责启动和管理进程。正式服务单元大致会表达：

- 程序路径；
- 配置路径；
- 运行用户；
- 依赖的挂载点和网络；
- 退出后是否重启；
- SIGTERM 等待时间；
- 开机是否启用。

两个常见操作不是一回事：

```text
systemctl start edgevision-gateway
```

表示现在启动一次。

```text
systemctl enable edgevision-gateway
```

表示创建开机启动关系，下一次开机由 systemd 自动启动。很多系统可以用 `enable --now` 同时完成两件事。

当前 `edgevision-outbox.service` 是 `Type=oneshot`：

- systemd 启动它；
- 脚本投递一条 pending；
- 程序退出；
- 它不会持续采集 STM32。

正式 Gateway 应是常驻进程。只要主进程仍在运行，systemd 就认为服务处于 active；主程序收到 SIGTERM 后应在规定时间内干净退出。

NFS 只解决“板端从哪里读取程序”，systemd 解决“何时启动程序”，SQLite 解决“状态保存在哪里”，四者职责不能混为一谈。

## 22. 怎样用 sqlite3 命令行观察数据库

下面命令用于理解和排错。执行前应先复制测试数据库，或确认不会与正在运行的服务冲突。

打开数据库：

```sh
sqlite3 /userdata/edgevision-gateway/gateway.db
```

进入后：

```sql
.tables
.schema measurements
.schema outbox
PRAGMA journal_mode;
PRAGMA user_version;
.headers on
.mode column

SELECT id, device_id, metric, value, unit, timestamp_ms
FROM measurements
ORDER BY id DESC
LIMIT 10;

SELECT id, event_id, state, attempt_count, last_error
FROM outbox
ORDER BY id DESC
LIMIT 10;

SELECT state, COUNT(*)
FROM outbox
GROUP BY state;
```

退出：

```sql
.quit
```

如何解释结果：

- `journal_mode` 返回 `wal`，才表示当前数据库真正使用 WAL；
- 大量 pending 表示 Broker 不可达、worker 未运行或某条消息阻塞；
- sent 只说明程序记录了确认结果，还需用日志和订阅端证据审计具体消息；
- measurements 有新增而 outbox 没新增，说明事务设计或调用链有问题；
- 两张表都没有新增，要回到 Source/采集层排查。

不要直接手工把 pending 改成 sent 来“通过测试”，这样会破坏证据。

## 23. 应该怎样测试，而不是只看一次成功

### 23.1 Storage 单元/模块测试

至少包括：

1. 新数据库自动建表；
2. 保存一条 Measurement 后，两张表各有一行；
3. 模拟第二次插入失败，两张表都没有半条数据；
4. 关闭并重新打开，pending 仍存在；
5. 没有 pending 时返回明确结果；
6. mark_sent 只更新指定 id；
7. 重复 event_id 被约束拦截；
8. v0 数据库能迁移到 v1；
9. `PRAGMA journal_mode` 返回 wal；
10. statement、事务和连接在错误路径上都释放。

### 23.2 Outbox 测试

至少包括：

1. Broker 离线，pending 保留；
2. Broker 恢复，消息补发；
3. 未收到 PUBACK，不能 sent；
4. 收到错误 message id 的 PUBACK，不能误标；
5. 收到对应 PUBACK，标 sent；
6. 100 条消息按设计顺序处理；
7. PUBACK 后、mark_sent 前模拟崩溃，重启后允许重复但不丢；
8. SIGTERM 到来后在 3 秒内退出，数据库保持一致。

网络测试需要独立订阅端，因为发布程序自己打印“publish success”不能证明外部订阅者真正收到。

### 23.3 Modbus 数据源测试

不连接真实硬件也应能稳定测试：

- 正常帧；
- timeout；
- CRC 错；
- 异常响应；
- 设备断开；
- 短读；
- 响应属于错误从站；
- 功能码不匹配；
- 负温度和过期数据。

可以使用 PTY、fake transport 或 ReplayDataSource。真实硬件测试用来验证电气和现场时序，不能替代确定性回归测试。

### 23.4 集成验收

最终应观察同一个 `event_id` 穿过：

```text
Modbus 原始响应
→ Measurement 日志
→ measurements 行
→ outbox pending 行
→ MQTT mid
→ PUBACK
→ outbox sent 行
→ 独立订阅端 payload
```

这样才能回答“这条数据从哪里来、何时保存、是否确认、订阅端收到什么”。

## 24. 常见误区

### 误区 1：CMake 构建 storage 示例，就等于 Gateway 接入 storage

不是。CMake 可以在同一项目生成多个彼此独立的可执行文件。只有 Gateway 链接 `edgevision_storage` 并在运行路径中调用它，才算接入。

### 误区 2：执行 publish 返回成功，就可以标 sent

不是。QoS 1 要等对应 PUBACK。

### 误区 3：SQLite 有一个文件，所以每次 INSERT 天然都是整体事务

单条 INSERT 自己有原子性，但 Measurement 与 Outbox 是两条 INSERT。要显式放入同一个事务，才能一起成功或失败。

### 误区 4：用了 Outbox 就不会重复

Outbox 主要避免丢失。PUBACK 与数据库写回之间仍有崩溃窗口，需要 event_id 和订阅端幂等。

### 误区 5：WAL 让任意线程随便共享一个连接

WAL 改善读写并发，不替代线程所有权、事务边界和 `SQLITE_BUSY` 处理。

### 误区 6：systemd 显示成功，就代表 Gateway 持续运行

oneshot 成功只说明命令执行完且退出码正常。要看单元类型、主进程是否仍在、日志和实际业务状态。

### 误区 7：NFS 是把程序“安装进板端”

NFS 是远程挂载。板端从 WSL 导出目录读取程序；WSL 不可用时，文件可能无法访问。正式固化还需决定把二进制打入 rootfs、复制到持久分区，或保留 NFS 作为开发部署方式。

## 25. 当前工程文件与学习顺序

建议按下面顺序阅读，不要一次把所有代码混在一起：

1. `examples/serial/modbus_rtu_demo.c`
   - 找请求字节；
   - 找两段收帧；
   - 找 CRC 和字段校验；
   - 找寄存器到 Measurement 的映射。
2. `examples/storage/sqlite_measurement_demo.c`
   - 看 open、建表、prepare、bind、step、finalize。
3. `examples/storage/sqlite_outbox_demo.c`
   - 看 BEGIN、两次 INSERT、COMMIT、ROLLBACK。
4. `examples/storage/sqlite_outbox_delivery_demo.c`
   - 看 pending 怎样跨进程被取出。
5. `examples/storage/sqlite_outbox_mqtt_demo.c`
   - 看 MQTT mid、PUBACK 和 sent 的关系。
6. `examples/storage/CMakeLists.txt`
   - 看这四个是独立 executable。
7. 根 `CMakeLists.txt` 与 `core/gateway.c`
   - 核对默认 Gateway 尚未链接 storage，也尚未使用正式 Modbus 源。
8. `docs/systemd-nfs-board-deployment-2026-09-01.md`
   - 理解板端文件、挂载、脚本和 oneshot 服务的关系。
9. `docs/week06-d36-d42-audit-and-next-plan-2026-09-02.md`
   - 对照严格缺口和后续实现顺序。

## 26. 下一步怎样安排

推荐顺序不是直接把所有示例复制进 Gateway，而是逐层收口：

### 第一步：把 D39 变成正式模块

- 建立 `edgevision_storage`；
- 统一 schema；
- 真正启用 WAL；
- 加 user_version 和迁移；
- 提供公共 API；
- 让现有四个 storage 示例改用该 API；
- 先通过无硬件测试。

### 第二步：把 D40 变成正式 worker

- 单 SQLite 所有者；
- 有界队列；
- pending 顺序投递；
- PUBACK 后 sent；
- 失败重试与 SIGTERM；
- event_id 和重复语义；
- 100 条与故障测试。

### 第三步：抽取正式 ModbusRtuSource

- 从 demo 移出请求、收帧、校验、映射；
- 使用内部两项缓存交付温湿度；
- 五类确定性测试；
- 再做真实硬件统计。

### 第四步：接入主 Gateway

- Source 产生 Measurement；
- Storage 原子保存；
- worker 投递；
- 配置化串口、数据库和 Broker；
- 统一停止顺序和日志关联。

### 第五步：部署正式服务

- 交叉编译集成后的 `gateway`；
- systemd 改为启动常驻主程序；
- 继续把数据库放在 `/userdata`；
- 补 30 分钟运行和三类故障；
- 完成 D36～D42 对应的严格证据后逐个验收。

这个顺序的依赖关系是：

```text
D39 Storage 基础
      ↓
D40 Outbox worker
      ↓
D41 集成部署

D37 串口基础
      ↓
D38 ModbusRtuSource
      └──────────────→ D41 集成部署

D42 ReplayDataSource 用同一主链做可靠性回归
```

## 27. 初学者可以动手完成的练习

这些练习按风险从低到高排列，不需要先接真实硬件。

### 练习 1：只观察数据库

目标：能查看表、schema、journal_mode、user_version 和 pending 数量。

完成标准：你能解释每个查询结果，不手工修改业务状态。

### 练习 2：跟踪一条事务

在 `sqlite_outbox_demo.c` 中找出：

- BEGIN 在哪里；
- Measurement INSERT 在哪里；
- Outbox INSERT 在哪里；
- 哪些错误路径执行 ROLLBACK；
- 何时 COMMIT。

完成标准：能用自己的话解释“为什么不会只写一张表”。

### 练习 3：跟踪一条 MQTT 确认

在 `sqlite_outbox_mqtt_demo.c` 中找出：

- pending 怎样取出；
- publish 返回的 message id 保存在哪里；
- PUBACK 回调怎样匹配 message id；
- mark_sent 何时调用；
- timeout 时为什么不 sent。

完成标准：能画出 pending→PUBACK→sent 状态图。

### 练习 4：验证 WAL，而不是只写配置

在未来正式模块测试数据库中：

1. 打开连接；
2. 执行 `PRAGMA journal_mode=WAL`；
3. 检查返回值；
4. 关闭重开；
5. 用命令行查询 `PRAGMA journal_mode;`；
6. 记录结果。

完成标准：返回 `wal`，并理解 `-wal`、`-shm` 文件可能在运行时出现。

### 练习 5：构造回滚测试

给 Outbox 的某个必填字段制造约束失败，验证：

- API 返回失败；
- measurements 没有孤立的新行；
- outbox 没有半成品；
- 连接仍可继续使用。

不要用删除用户数据库来代替回滚测试，应使用独立临时数据库。

### 练习 6：把示例改为调用公共 API

当 `edgevision_storage` 建立后，四个示例不再直接包含 SQL，只做：

- 构造输入；
- 调用公共 API；
- 打印结果。

完成标准：修改 schema 只需改 storage 模块和迁移，不需要在四个示例重复修改。

## 28. 排错时从哪里开始

### 订阅端没有消息

按顺序检查：

1. Source 有没有产生 Measurement；
2. measurements 是否新增；
3. outbox 是否新增 pending；
4. worker 是否运行；
5. Broker 是否连接；
6. publish 是否得到 mid；
7. 是否收到对应 PUBACK；
8. outbox 是否变 sent；
9. topic 是否与订阅端一致。

不要一开始就同时修改串口、SQLite 和 MQTT。

### measurements 有记录，outbox 没记录

优先怀疑：

- 当前调用路径仍使用了单表 demo；
- Measurement 和 Outbox 没走统一事务 API；
- Outbox INSERT 失败后错误被忽略；
- 旧 schema 与新 SQL 不一致。

正式设计中，如果 Outbox INSERT 失败，Measurement INSERT 也应回滚，所以不应留下新孤立记录。

### outbox 长期 pending

检查：

- systemd 服务是否只是 oneshot 且已经退出；
- 是否只处理了一条；
- Broker 地址/隧道是否可达；
- MQTT 事件循环是否运行；
- PUBACK 回调是否匹配正确 mid；
- mark_sent 的数据库更新是否失败；
- 最早一条是否永久错误并阻塞后续。

### 数据库出现 busy

检查：

- 是否多个线程共享连接；
- 是否有 statement 未 finalize；
- 是否有事务长期不提交；
- 每个连接是否设置 busy_timeout；
- 写事务内是否做了耗时网络操作。

不要在 SQLite 写事务里等待 MQTT PUBACK。网络等待可能很长，会长期占用数据库写锁。

## 29. 关键术语表

| 术语 | 简单解释 |
| --- | --- |
| RS485 | 一种差分串行电气接口 |
| UART | 按字节发送和接收的串口控制器 |
| termios | Linux 配置终端/串口的接口 |
| Modbus RTU | 在串口字节上定义请求、响应和 CRC 的协议 |
| CRC | 检查帧在传输中是否损坏的校验值 |
| deadline | 整个操作不能超过的绝对截止时间 |
| Measurement | 主工程统一的测量数据结构 |
| SQLite | 程序内直接使用的文件数据库 |
| schema | 表、列、约束和索引的结构定义 |
| transaction | 一组一起提交或一起回滚的数据库操作 |
| COMMIT | 让事务中的修改正式生效 |
| ROLLBACK | 撤销当前事务中的修改 |
| WAL | SQLite 的预写日志模式 |
| Outbox | 本地保存“仍需发送”的可靠消息表 |
| pending | 还没有可靠确认送达 |
| sent | 已按本系统规则收到确认并写回 |
| MQTT Broker | 接收发布并转发给订阅者的服务 |
| QoS 1 | MQTT 至少一次投递等级 |
| PUBACK | Broker 对 QoS 1 PUBLISH 的确认 |
| at-least-once | 尽量不丢，但允许重复 |
| idempotent | 同一事件处理多次，最终效果仍与一次相同 |
| worker | 后台持续处理队列任务的线程或进程 |
| oneshot | 执行一次任务后退出的服务 |
| systemd | Linux 服务与开机启动管理器 |
| NFS | 通过网络挂载远程目录 |
| cross compile | 在 WSL PC 上生成 ARM64 板端程序 |

## 30. 看完后应能回答的自检题

1. 为什么 RS485 收发方向切回太晚会漏掉 STM32 响应？
2. 为什么一次 `read()` 不能假定返回完整 Modbus 帧？
3. 请求 `01 04 00 01 00 02 20 0B` 每个字段是什么？
4. 正常响应和异常响应的长度怎样判断？
5. 为什么两段读取要共享一个绝对截止时间？
6. CRC 正确以后，为什么还要做业务范围和 freshness 检查？
7. 一次响应为什么形成两条 Measurement？
8. 为什么 Measurement 与 Outbox 必须在同一个 SQLite 事务中插入？
9. `prepare/bind/step/finalize` 分别做什么？
10. 怎样证明 WAL 真的启用？
11. Broker 离线时，Outbox 为什么能避免消息丢失？
12. 为什么 publish API 返回成功仍不能立即标 sent？
13. QoS 1 + Outbox 为什么仍可能重复？
14. event_id 怎样帮助订阅端幂等？
15. 为什么当前 storage demo 还不是可复用模块？
16. 哪一行 CMake 关系才表示 Gateway 真正链接 storage？
17. 为什么不建议多个线程随意共享同一个 `sqlite3 *`？
18. systemd 的 start、enable 和 oneshot 分别是什么意思？
19. NFS、systemd、SQLite、Gateway 各自负责什么？
20. 当前下一步为什么先做 D39 Storage 模块，而不是直接宣称 D41 发布完成？

如果这些问题能用自己的话讲清，并能在代码中找到对应位置，就已经建立了这一周最核心的知识框架。
