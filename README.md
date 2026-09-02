# EdgeVision Gateway

EdgeVision Gateway 是一个面向嵌入式 Linux 的 C11 边缘数据网关学习项目。项目以
`Measurement V1` 为统一数据契约，逐步验证设备采集、协议校验、可靠传输、离线
持久化和进程生命周期，并保留 Linux 主机自动测试与 OK1126B-S ARM64 目标板路径。

当前仓库同时包含“可复用模块”“Gateway 主程序”和“阶段性独立示例”。默认 Gateway
已经具备模拟 Measurement、TCP 整链自检、异步日志、信号退出以及可选 MQTT 发布；
真实 STM32/DHT11 的 Modbus RTU 采集和 SQLite Outbox 已完成独立验证，但尚未接入
默认 Gateway 常驻主链。因此它是可验证的学习工程，不应直接视为完整生产网关。

## 项目全景

```text
模拟源 ───────────────────────────────────────────────┐
                                                       v
STM32/DHT11 -- RS485 -- Modbus RTU 04 -- 寄存器映射 -> Measurement V1
                                                       |
                         +-----------------------------+------------------+
                         |                                                |
                         v                                                v
               自定义 TCP 帧 / MQTT QoS 1                     SQLite measurements
               网络发送、接收与确认                              + pending Outbox
                                                                          |
                                                                          v
                                                        MQTT PUBACK -> sent

Gateway Core 负责编排正式主链的日志、信号、数据源、协议和网络生命周期；
Modbus 与 SQLite Outbox 当前仍是独立示例/部署验证，尚未组合成常驻流水线。
```

项目目前验证了以下能力：

- `Measurement V1` 字段校验与 JSON 编解码。
- 统一 `measurement_source_t` 抽象、确定性模拟源和真实串口原始数据适配。
- IPv4 TCP/UDP、完整发送、epoll 服务和带超时/退避/心跳的后台客户端。
- 自定义 TCP 帧的增量解析、噪声重同步与 CRC16-Modbus 校验。
- Modbus RTU 03/04 请求构造、正常/异常响应校验，以及 STM32 输入寄存器映射。
- UART5 + GPIO22 半双工 RS485；发送方向切换前同时检查软件队列和 UART TEMT。
- 有界队列异步日志、轮转、gzip 压缩与可选逐条 `fsync`。
- `SIGINT`/`SIGTERM` 优雅退出，按顺序排空日志并释放资源。
- 可选 libmosquitto MQTT QoS 1 发布、PUBACK 等待与有限重连。
- SQLite Measurement 持久化、Measurement/Outbox 原子提交及确认后标记 `sent`。
- CMake/CTest 单元、集成、异常路径、退出、冒烟和压力测试。

## 当前边界

- 默认 `gateway` 的数据源仍是模拟源；真实 Modbus 查询位于
  `examples/serial/modbus_rtu_demo.c`，尚未实现为正式常驻 Source。
- SQLite 示例使用独立的 `examples/storage/CMakeLists.txt`，尚未成为
  `edgevision_core` 的存储模块。
- Outbox 发布遵循 at-least-once 方向：PUBACK 后、数据库更新前崩溃可能重复发布；
  当前没有消费者去重、并发领取、退避调度或 exactly-once 保证。
- `edgevision-outbox.service` 是一次处理一条 pending 记录的 `oneshot` 服务，不是
  持续运行的发送守护进程。
- PTY、GPIO 和 ioctl 替身测试只能证明软件状态机；RS485 电气时序以目标板记录为准，
  现有记录不等同于示波器波形验收。

## 代码结构

```text
apps/gateway/             CLI 入口
core/                     Gateway 编排与 measurement_source_t 抽象
domain/                   Measurement V1、校验和 JSON 转换
modules/blocking_queue/   通用有界阻塞队列
modules/log/              异步日志、轮转与压缩
modules/net/              TCP/UDP、epoll 和 NetworkClient
modules/protocol/         TCP 帧、CRC16-Modbus、Modbus RTU 03/04 校验
modules/runtime/          SIGINT/SIGTERM 优雅退出
modules/source/           模拟源与真实串口原始数据适配
modules/serial/           UART/RS485、GPIO 方向与精确收发辅助
modules/mqtt/             可选 MQTT QoS 1 发布适配层
examples/net/             网络 API 学习示例
examples/serial/          RS485 原始收发和 Modbus/Measurement 示例
examples/storage/         SQLite 与 Outbox 渐进式独立示例
examples/mqtt/            可选 MQTT 学习示例
hardware/rs485/           接线、设备参数卡和实板证据
deploy/edgevision-outbox/ NFS/systemd 部署脚本与模板
tests/                    单元、集成、异常路径与压力测试
docs/                     设计、教程、进度和目标板验证记录
third_party/cjson/        内置 cJSON
```

正式依赖方向由 CMake 目标约束：`apps/gateway` 只依赖 `edgevision_core`，核心层再
编排领域、数据源、协议、网络、日志和运行时。独立示例用于先验证边界，不代表已经
进入正式主链。

## 构建要求

默认工程需要：

- CMake 3.16 或更高版本；
- 支持 C11 的 GCC 或 Clang；
- POSIX Threads；
- zlib。

启用 MQTT 时还需要 `pkg-config` 和 libmosquitto 开发包。构建 SQLite 独立示例还
需要 SQLite3 开发包；该子工程当前也会查找 libmosquitto。cJSON 已随仓库提供。

## 构建与测试

默认构建不启用 MQTT：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

编译默认开启 `-Wall -Wextra -Wpedantic -Werror`。如需临时允许警告：

```bash
cmake -S . -B build -DEDGEVISION_WARNINGS_AS_ERRORS=OFF
```

查看测试或运行特定测试：

```bash
ctest --test-dir build -N
ctest --test-dir build -R '^gateway_smoke$' --output-on-failure
ctest --test-dir build -R '^serial_tx_complete$' --output-on-failure
ctest --test-dir build -R '^net_stress$' --output-on-failure
```

## 运行 Gateway

常驻模式初始化信号处理和异步日志后等待退出：

```bash
./build/gateway
./build/gateway /tmp/edgevision-gateway.log
```

按 `Ctrl+C` 或发送 `SIGTERM` 后，程序会记录退出请求、排空日志队列并释放资源。
默认日志文件为 `gateway.log`。

本机 TCP + Measurement + JSON + 协议帧整链自检：

```bash
./build/gateway --smoke
./build/gateway --smoke /tmp/gateway-smoke.log
```

`--smoke` 连续发送 100 条不同 Measurement；接收端从字节流恢复帧、解析 JSON、
逐字段核对，并按序返回 `MeasurementAck`。成功时标准输出包含 `SMOKE_PASS`。

## 可选 MQTT 构建

```bash
cmake -S . -B build-mqtt \
    -DCMAKE_BUILD_TYPE=Debug \
    -DEDGEVISION_ENABLE_MQTT=ON
cmake --build build-mqtt --parallel
ctest --test-dir build-mqtt --output-on-failure
./build-mqtt/gateway --mqtt-smoke
```

默认 Broker 为 `127.0.0.1:1883`，主题为
`edgevision/v1/devices/<device_id>/measurements`。单条消息只有收到匹配的 QoS 1
PUBACK 才视为成功。目标板 SDK 必须提供对应架构的 libmosquitto 才能启用 MQTT。

## Modbus RTU 与真实串口示例

`modbus_rtu_demo` 支持 PC 模拟从站的 03 查询、STM32/DHT11 的 04 查询，以及可选
MQTT 发布：

```bash
cmake --build build --target modbus_rtu_demo
./build/modbus_rtu_demo --map-sample   # 固定数据映射，不访问硬件
./build/modbus_rtu_demo                # /dev/ttyS5，03/地址0/数量2
./build/modbus_rtu_demo --stm32        # /dev/ttyS5，04/地址1/数量2
```

串口模式固定使用 `/dev/ttyS5`、`/dev/gpiochip0` 的 line 22。请求与响应经过站号、
功能码、长度和 CRC 校验后，才将两个寄存器映射为温度、湿度 Measurement。详细的
接线、参数、交叉编译和实板证据见文档索引。

## SQLite 与 Outbox 示例

SQLite 示例是独立子工程：

```bash
cmake -S examples/storage -B build-storage
cmake --build build-storage --parallel
```

四个程序按学习顺序形成一条离线可靠发送链：

1. `sqlite_measurement_demo`：校验 JSON 后写库，并由新进程读回。
2. `sqlite_outbox_demo`：同一事务提交 Measurement 与 pending Outbox。
3. `sqlite_outbox_delivery_demo`：用离线发布替身验证“确认后再标记 sent”。
4. `sqlite_outbox_mqtt_demo`：真实 QoS 1 PUBACK 后标记 sent。

示例输入 `temperature-replay.json` 是历史实测记录的回放，不代表当前传感器温度。
本地数据库、WAL/SHM 文件和组装后的 `deploy/nfs-root/` 发布包由 `.gitignore` 排除；
部署脚本和 systemd 模板保留在源码仓库中。

## TCP 帧与数据约束

一条自定义 TCP 应用帧格式为：

```text
MAGIC(0xA5 0x5A) + LEN(1 byte) + PAYLOAD + CRC16(2 bytes)
```

单帧 payload 最大 240 字节，一帧承载一条完整 Measurement JSON，不支持跨帧 JSON
分片。Measurement V1 包含设备 ID、设备内递增序号、UTC 毫秒时间戳、指标、数值、
单位和数据质量。

## 文档索引

- [Measurement V1 数据契约](docs/d33-measurement-contract.md)
- [TCP 协议帧与 Modbus RTU 对照](docs/d32-tcp-framing-modbus-rtu.md)
- [STM32/DHT11 Modbus RTU 读取记录](docs/stm32-dht11-modbus-read.md)
- [PC Modbus 到 Measurement 映射](docs/pc-modbus-measurement-mapping.md)
- [SQLite Outbox 初学者教程](docs/modbus-sqlite-outbox-beginner-tutorial-2026-09-02.md)
- [systemd + NFS 目标板部署指南](docs/systemd-nfs-board-deployment-2026-09-01.md)
- [可审计历史回放记录](docs/d42-auditable-replay-2026-09-01.md)
- [D36-D42 审计与下一阶段计划](docs/week06-d36-d42-audit-and-next-plan-2026-09-02.md)
- [最新学习进度](docs/learning-progress-2026-08-31.md)
- [RS485 接线、参数与硬件验收证据](hardware/rs485/README.md)
- [OK1126B-S UART5/RS485 与 NFS 开发指南](docs/rs485-uart5-nfs-development-guide.md)
- [Gateway 核心代码阅读与整链验证指南](docs/gateway-code-reading-guide.md)
- [构建、CTest 与内存安全检查](docs/build-cmake-ctest-sanitizers.md)
