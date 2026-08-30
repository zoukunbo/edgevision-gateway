# EdgeVision Gateway

EdgeVision Gateway 是一个面向嵌入式 Linux 的边缘数据网关学习项目，使用 C11
实现。项目围绕“采集设备数据、形成统一 Measurement、可靠传输并安全退出”逐步
搭建可复用模块，同时保留主机自动测试和 OK1126B-S 目标板交叉编译路径。

当前仓库处于持续迭代阶段：Measurement 数据链路、TCP 本地整链自检、异步日志、
网络基础设施和可选 MQTT 发布已经可以验证；默认常驻模式目前主要验证进程生命周期
与优雅退出，还不是完整的生产网关服务。

## 项目能力

- 定义并校验版本化的 `Measurement V1` 领域模型，支持 JSON 编解码。
- 通过统一 `measurement_source_t` 接口隔离数据源，已提供确定性模拟数据源。
- 提供 IPv4 TCP/UDP 封装、完整发送、异常路径处理和 epoll 诊断服务。
- 提供带连接超时、指数退避、心跳和可中断停止的后台 `NetworkClient`。
- 使用自定义 TCP 帧解决粘包、拆包问题，并支持增量解析、噪声重同步和
  CRC16-Modbus 校验。
- 使用有界阻塞队列和独立写线程记录日志，支持文件轮转、gzip 压缩和可选逐条
  `fsync`。
- 捕获 `SIGINT`、`SIGTERM`，按顺序排空日志并释放资源。
- 可选集成 libmosquitto，以 QoS 1 发布 Measurement，等待 PUBACK 并自动重连。
- 使用 CMake/CTest 覆盖单元测试、TCP/UDP 异常路径、优雅退出、整链冒烟和
  100 个连接、100 MiB 数据的压力测试。

## 当前数据链路

项目以 `Measurement` 作为模块之间的统一业务对象：

```text
数据源
  └─> measurement_source_t
        └─> Measurement V1 校验与 JSON 编解码
              ├─> 自定义 TCP 帧 -> 网络收发 -> 增量解析 -> MeasurementAck
              └─> MQTT QoS 1 发布 -> <topic_prefix>/<device_id>/measurements

Gateway Core 同时编排：异步日志 + 信号处理 + 各模块生命周期
```

当前可以直接验证两条路径：

1. 默认构建的 `--smoke` 在本机回环 TCP 连接上连续发送 100 条不同的
   Measurement。接收端从连续字节流中恢复帧、解析 JSON、逐字段校验，并返回
   100 条按序的 `MeasurementAck`。
2. 启用 MQTT 后，`--mqtt-smoke` 从模拟数据源取得 100 条 Measurement，发布到
   本机 Broker，并为每条 QoS 1 消息等待 PUBACK。

## 代码结构

```text
apps/gateway/           命令行入口，只解析参数并调用 Gateway Core
core/                   网关编排和数据源抽象
domain/                 Measurement 领域模型、校验和 JSON 转换
modules/blocking_queue/ 通用有界阻塞队列
modules/log/            异步日志、轮转与压缩
modules/net/            TCP/UDP、epoll 和可重连网络客户端
modules/protocol/       自定义 TCP 帧、增量解析和 CRC16-Modbus
modules/runtime/        SIGINT/SIGTERM 优雅退出
modules/source/         模拟Measurement源、真实串口原始数据适配（Measurement仍占位）
modules/serial/         UART5/RS485 和 GPIO 半双工基础封装
hardware/rs485/         D36 接线、设备参数卡及原始字节证据
modules/mqtt/           可选的 MQTT QoS 1 发布适配层
third_party/cjson/      内置 cJSON 源码
examples/               网络和 MQTT 独立学习示例
tests/                  单元、集成、异常路径和压力测试
docs/                   设计、学习和目标板验证记录
```

依赖方向由 CMake 目标约束：`apps/gateway` 只依赖 `edgevision_core`，核心层负责编排
领域、数据源、协议、网络、日志和运行时模块；具体数据源通过统一接口接入，避免业务
核心直接依赖某一种采集实现。

## 构建要求

默认构建需要：

- CMake 3.16 或更高版本
- 支持 C11 的 GCC 或 Clang
- POSIX Threads
- zlib

MQTT 是可选能力。启用时还需要 `pkg-config` 和 libmosquitto 开发包。cJSON 已随
仓库提供，不需要额外安装。

## 构建与测试

默认构建不启用 MQTT：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

编译默认开启 `-Wall -Wextra -Wpedantic -Werror`。如需临时允许编译警告，可在
配置时添加：

```bash
-DEDGEVISION_WARNINGS_AS_ERRORS=OFF
```

只查看已注册测试或运行某一类测试：

```bash
ctest --test-dir build -N
ctest --test-dir build -R '^gateway_smoke$' --output-on-failure
ctest --test-dir build -R '^net_stress$' --output-on-failure
```

## 运行 Gateway

常驻模式会初始化信号处理和异步日志，然后等待退出信号：

```bash
./build/gateway
./build/gateway /tmp/edgevision-gateway.log
```

按 `Ctrl+C` 或发送 `SIGTERM` 后，程序会记录退出请求、排空日志队列并释放资源。
默认日志文件为 `gateway.log`。

运行 TCP + Measurement + JSON + 协议帧的本地整链自检：

```bash
./build/gateway --smoke
./build/gateway --smoke /tmp/gateway-smoke.log
```

成功时标准输出包含 `SMOKE_PASS`，日志中会记录处理的 Measurement 数量以及请求、
响应帧统计。

## 可选 MQTT 构建

主机已安装 libmosquitto 开发文件时，可以启用 MQTT 模块：

```bash
cmake -S . -B build-mqtt \
    -DCMAKE_BUILD_TYPE=Debug \
    -DEDGEVISION_ENABLE_MQTT=ON
cmake --build build-mqtt --parallel
ctest --test-dir build-mqtt --output-on-failure
```

启动本机 MQTT Broker 后运行发布冒烟测试：

```bash
./build-mqtt/gateway --mqtt-smoke
```

默认连接 `127.0.0.1:1883`。发布器将消息发送到
`edgevision/v1/devices/<device_id>/measurements`，断线时按上限退避重连，单条
消息只有收到 QoS 1 PUBACK 后才视为成功。

如需同时构建 MQTT 学习示例：

```bash
cmake -S . -B build-mqtt \
    -DEDGEVISION_ENABLE_MQTT=ON \
    -DEDGEVISION_BUILD_MQTT_EXAMPLE=ON
cmake --build build-mqtt --parallel

./build-mqtt/gateway --mqtt-smoke 127.0.0.1 1884
```

目标板默认关闭 MQTT，因为 SDK 必须先提供对应架构的 libmosquitto。Buildroot
接入、SDK 导出、交叉编译和部署步骤见下方文档。

## 协议与数据约束

一条 TCP 应用帧的格式为：

```text
MAGIC(0xA5 0x5A) + LEN(1 byte) + PAYLOAD + CRC16(2 bytes)
```

单帧 payload 最大为 240 字节。当前约定一帧承载一条完整的 Measurement JSON，
不支持跨帧 JSON 分片。Measurement V1 包含设备 ID、设备内递增序号、UTC 毫秒
时间戳、指标、数值、单位和数据质量；完整字段规则及示例见
[Measurement V1 数据契约](docs/d33-measurement-contract.md)。

## 文档索引

- [D36 收尾核对与待验收边界](docs/d36-closeout.md)
- [RS485 接线、参数与硬件验收证据](hardware/rs485/README.md)

- [OK1126B-S UART5/RS485 与 NFS 开发指南](docs/rs485-uart5-nfs-development-guide.md)
- [Gateway 核心代码阅读与整链验证指南](docs/gateway-code-reading-guide.md)
- [Measurement V1 数据契约](docs/d33-measurement-contract.md)
- [TCP 协议帧与 Modbus RTU 对照](docs/d32-tcp-framing-modbus-rtu.md)
- [epoll 事件循环学习笔记](docs/epoll-event-loop-notes.md)
- [NetworkClient 设计复盘与面试题](docs/d30-network-client-review-interview.md)
- [编译、CMake、CTest 与内存安全检查](docs/build-cmake-ctest-sanitizers.md)
- [Buildroot 第三方库、SDK 与交叉编译指南](docs/buildroot-third-party-library-integration.md)
- [D29 网络入口验证](docs/d29-network-validation.md)
- [D29 OK1126B-S 板端验证证据](docs/d29-ok1126b-board-evidence.md)
