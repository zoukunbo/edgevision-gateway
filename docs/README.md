# EdgeVision Gateway 文档导航

文档按用途分成三类。查操作步骤进 `operations/`，复习原理进 `tutorials/`，核对某次实际运行结果进 `records/`。历史记录中的路径、IP、测试数量和完成状态只代表当时，不自动等于当前工程状态。

## 我现在想找什么

| 需求 | 唯一入口 |
| --- | --- |
| 配置、挂载或排查 NFS | [NFS 开发环境运维指南](operations/nfs-development.md) |
| 用 ADB、SCP 或 NFS 快速部署 | [ADB、SCP 与 NFS 快速部署](operations/quick-deployment.md) |
| 配置 systemd 开机挂载和服务 | [systemd 板端部署](operations/systemd-board-deployment.md) |
| 接入 Buildroot 库或导出 SDK | [Buildroot 第三方库与 SDK](operations/buildroot-third-party-library-integration.md) |
| 学习 RS485、UART5 和方向控制 | [UART5/RS485 开发教程](tutorials/rs485-uart5.md) |
| 学习 Modbus、SQLite、Outbox、MQTT | [完整数据链路教程](tutorials/modbus-sqlite-outbox.md) |
| 理解 Gateway 主流程 | [Gateway 代码阅读指南](tutorials/gateway-code-reading-guide.md) |
| 查看当前/历史完成度 | [W06 严格审计](records/week06-d36-d42-audit-and-next-plan-2026-09-02.md) |
| 查某次实板或测试结果 | [运行与验证记录](#运行与验证记录-records) |

## 建议阅读顺序

第一次了解项目：

1. [项目 README](../README.md)：当前能力、代码结构和构建入口。
2. [Gateway 代码阅读指南](tutorials/gateway-code-reading-guide.md)：主链如何编排。
3. [Modbus、SQLite 与 Outbox 教程](tutorials/modbus-sqlite-outbox.md)：真实设备到可靠 MQTT 的数据链路。
4. 按需要进入运维、网络、串口或存储专题。

## 运维教程 `operations/`

这些文档回答“现在应该怎么操作”。重复命令只在一个权威入口维护。

| 文档 | 用途 |
| --- | --- |
| [NFS 开发环境运维指南](operations/nfs-development.md) | 主机导出、板端挂载、日常更新和故障定位 |
| [ADB、SCP 与 NFS 快速部署](operations/quick-deployment.md) | 三种方式的选择、命令、产物校验和常见故障 |
| [systemd 板端部署](operations/systemd-board-deployment.md) | NFS 自动挂载、服务托管、日志和验收 |
| [Buildroot 第三方库与 SDK](operations/buildroot-third-party-library-integration.md) | Mosquitto、sysroot、SDK 和交叉编译 |

## 知识教程 `tutorials/`

这些文档用于复习原理、接口和当前可复用做法，不承担历史运行日志的职责。

### 架构、构建与控制

- [Gateway 核心代码阅读](tutorials/gateway-code-reading-guide.md)
- [Measurement V1 数据契约](tutorials/measurement-contract.md)
- [CMake、CTest 与 Sanitizer](tutorials/build-cmake-ctest-sanitizers.md)
- [命令控制中心](tutorials/command-control-center.md)

### 网络与并发

- [NetworkClient 复习与面试](tutorials/network-client-review-interview.md)
- [TCP 帧与 Modbus RTU 对照](tutorials/tcp-framing-modbus-rtu.md)
- [epoll 事件循环](tutorials/epoll-event-loop-notes.md)

### 串口、协议与存储

- [UART5/RS485 开发教程](tutorials/rs485-uart5.md)
- [PC Modbus 到 Measurement 映射](tutorials/pc-modbus-measurement-mapping.md)
- [Modbus、SQLite 与 Outbox 初学者教程](tutorials/modbus-sqlite-outbox.md)
- [面试知识点](tutorials/面试八股文.md)

## 运行与验证记录 `records/`

这些文档回答“当时实际发生了什么”。它们用于审计和复盘，不应作为最新操作教程。

### 网络与板端验证

- [D29 网络入口验收](records/d29-network-validation.md)
- [D29 OK1126B-S 板端证据](records/d29-ok1126b-board-evidence.md)
- [D34 Buildroot/MQTT 验证](records/d34-buildroot-mqtt-validation.md)

### RS485、Modbus 与数据链路

- [D36 收尾记录](records/d36-closeout.md)
- [STM32 DHT11 Modbus 实板记录](records/stm32-dht11-modbus-read.md)
- [D40 正式链路实现复盘](records/d40-stm32-modbus-outbox-gateway-implementation-2026-09-07.md)

### SQLite、Outbox 与部署

- [SQLite/Outbox 阶段总结](records/sqlite-outbox-learning-2026-09-01.md)
- [Storage/Outbox 进度](records/storage-outbox-progress-2026-09-06.md)
- [systemd/NFS 历史部署记录](records/systemd-nfs-board-deployment-2026-09-01.md)
- [D42 可审计回放](records/d42-auditable-replay-2026-09-01.md)

### 状态、审计与交接

- [W06 D36～D42 严格审计](records/week06-d36-d42-audit-and-next-plan-2026-09-02.md)
- [2026-08-31 学习进度](records/learning-progress-2026-08-31.md)
- [历史会话接续说明](records/next-chat-prompt-2026-08-31.md)

## 当前统一口径

- 真实 DHT11 → STM32 → RS485 → OK1126B → Modbus → Measurement → MQTT 的最小链路曾跑通并留有证据。
- Gateway 已支持模拟源或 STM32 Modbus 源，并可通过 Storage/Outbox/MQTT worker 处理正式主链。
- MQTT 可靠性是 at-least-once 方向；PUBACK 后、数据库更新前崩溃仍可能重复发布。
- 正式板端发布骨架位于 `deploy/edge-gateway-lite/`；旧 Outbox oneshot 部署只作为历史验证保留。
- 代码、配置和原始日志是事实来源；文档与实现冲突时，应重新核对 Git 和测试结果。

## 文档维护规则

- 新操作步骤放入 `operations/`，同一主题只保留一个权威操作入口。
- 可长期复习的原理、接口和模式放入 `tutorials/`。
- 带日期的运行结果、验收、进度和复盘放入 `records/`。
- 教程不复制历史日志；记录不冒充当前教程，二者通过链接关联。
- 文档移动或新增后必须更新本页，并检查仓库内 Markdown 链接。
- `superpowers/` 保存设计和实施计划，不并入上述学习分类。
