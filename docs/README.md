# EdgeVision Gateway 文档导航

本目录同时保存当前工程说明、长期学习教程和按日期固化的验证记录。为避免把历史
结论误当成当前状态，阅读时先看本页的“当前统一口径”，再按主题进入具体文档。

## 建议阅读顺序

第一次了解项目：

1. [项目 README](../README.md)：项目能力、代码结构、构建方式和当前边界。
2. [W06 严格审计与后续计划](week06-d36-d42-audit-and-next-plan-2026-09-02.md)：
   当前任务状态和下一阶段优先级，优先级高于早期进度记录。
3. [Modbus、SQLite 与 Outbox 初学者教程](modbus-sqlite-outbox-beginner-tutorial-2026-09-02.md)：
   从真实设备到可靠 MQTT 的完整数据链路。
4. 再从下方主题索引选择协议、串口、网络、存储或部署文档。

接续下一次学习或开发：先读
[会话接续说明](next-chat-prompt-2026-08-31.md)，再核对当前代码和 Git 状态。该文件名
保留首次交接日期，正文已包含 2026-09-02 的严格审计口径。

## 当前统一口径

截至 2026-09-02，以下表述作为文档间出现冲突时的解释基线：

- 真实 DHT11 → STM32 → RS485 → OK1126B → Modbus 04 → Measurement → MQTT 的
  最小链路曾经跑通并留有证据。
- SQLite/Outbox 示例已验证原子提交/回滚、pending 跨进程、发布未确认时保留以及
  QoS 1 PUBACK 后标记 sent。
- 默认 `gateway` 仍使用模拟数据源；真实 Modbus 查询仍在独立示例中。
- SQLite/Outbox 位于 `examples/storage/`，尚未成为 `edgevision_core` 的正式模块；
  WAL、常驻 worker、并发领取和完整重试策略尚未实现。
- 当前板端 systemd 部署运行的是一次处理一条记录的 Outbox `oneshot` 示例，不是
  集成真实采集、存储和补发的常驻 Gateway。
- 当前只具备 at-least-once 方向；PUBACK 后、数据库更新前崩溃仍可能重复发布。
- D36～D42 有多项最小行为已经验证，但按原始 DoD 均为“进行中、严格验收未通过”。

带日期文档中的测试数量、路径、运行状态和“完成”描述是当时证据，不应自动外推为
今天的工程状态。当前构建结果以最新实际运行和根 README 为准。

## 主题索引

### 架构与数据契约

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [Gateway 核心代码阅读指南](gateway-code-reading-guide.md) | 长期指南 | 从 `gateway_run()` 理解正式主链的编排关系 |
| [Measurement V1 数据契约](d33-measurement-contract.md) | 规范 | 字段、范围、JSON 格式及数据源边界 |
| [TCP 帧与 Modbus RTU 对照](d32-tcp-framing-modbus-rtu.md) | 设计说明 | 区分 TCP 字节流分帧与 Modbus RTU 事务 |

### 网络与并发

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [D29 网络入口验收记录](d29-network-validation.md) | 历史验收 | TCP/UDP、异常路径、压力测试与当时结论 |
| [D29 目标板证据](d29-ok1126b-board-evidence.md) | 历史证据 | OK1126B-S 网络、压力和优雅退出记录 |
| [NetworkClient 复习与面试手册](d30-network-client-review-interview.md) | 长期指南 | 非阻塞连接、退避、心跳和停止语义 |
| [epoll 事件循环笔记](epoll-event-loop-notes.md) | 学习笔记 | 监听 FD、就绪数组和非阻塞处理模型 |

### RS485、Modbus 与真实设备

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [D36 收尾与范围校正](d36-closeout.md) | 历史快照 | 2026-08-30 的学习停止点；页首更正优先 |
| [UART5/RS485 与 NFS 开发指南](rs485-uart5-nfs-development-guide.md) | 历史指南 | 接线、串口、GPIO、交叉编译和早期部署；实现细节需结合页首更新 |
| [PC 模拟寄存器映射](pc-modbus-measurement-mapping.md) | 阶段记录 | 模拟 03 响应到两条 Measurement 的映射 |
| [STM32 DHT11 真实读取](stm32-dht11-modbus-read.md) | 实板记录 | 04 事务、寄存器语义、TX 完成修正和实板证据 |

### SQLite、Outbox 与回放

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [SQLite/Outbox 学习总结](sqlite-outbox-learning-2026-09-01.md) | 阶段总结 | 原子事务、pending/sent 和 at-least-once 边界 |
| [D42 最小可审计回放包](d42-auditable-replay-2026-09-01.md) | 历史证据 | 固定输入、哈希、PUBACK、订阅和重启终态 |
| [完整初学者教程](modbus-sqlite-outbox-beginner-tutorial-2026-09-02.md) | 当前教程 | 串起 Modbus、Measurement、SQLite、Outbox、MQTT 和正式模块化方向 |

### 构建、交叉编译与部署

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [CMake、CTest 与 Sanitizer](build-cmake-ctest-sanitizers.md) | 长期指南 | 主机构建、测试、内存检查和常见错误 |
| [Buildroot 第三方库接入](buildroot-third-party-library-integration.md) | 环境指南 | Mosquitto、sysroot、SDK 导出及架构核对 |
| [D34 Buildroot MQTT 验证](d34-buildroot-mqtt-validation.md) | 历史验证 | 2026-08-26～28 的构建与板端 MQTT 证据 |
| [systemd、NFS 与板端持久化部署](systemd-nfs-board-deployment-2026-09-01.md) | 已执行教程 | Outbox 示例的只读 NFS 程序与板端可写状态部署 |

### 状态、审计与交接

| 文档 | 类型 | 用途 |
| --- | --- | --- |
| [W06 D36～D42 严格审计](week06-d36-d42-audit-and-next-plan-2026-09-02.md) | **当前状态基线** | 区分知识、最小验证、主工程接入和严格验收 |
| [2026-08-31 学习进度](learning-progress-2026-08-31.md) | 历史进度 | 保留过程记录；文首 2026-09-02 更正优先于正文旧表述 |
| [下一会话接续说明](next-chat-prompt-2026-08-31.md) | 操作交接 | 快速恢复上下文、约束和下一阶段任务 |

## 文档维护约定

- 不因内容过时直接删除带日期的证据文档；在页首标记历史属性，并链接到最新基线。
- 长期指南描述当前可复用机制；阶段记录只描述当时实际发生的行为。
- “学习过”“最小行为已验证”“已接入主工程”“严格验收完成”必须分开表述。
- 命令默认从仓库根目录执行；硬编码的本机、板端路径应注明环境和日期。
- 新的验证记录使用 `主题-YYYY-MM-DD.md`；持续维护的契约/指南不在文件名中加日期。
- 新增文档后更新本索引；根 README 只保留项目级入口，避免重复维护完整清单。
- 代码、配置和日志才是事实来源。文档与当前实现冲突时，先核对 Git 和测试，再修正文档。
