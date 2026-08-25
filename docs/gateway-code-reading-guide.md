# `core/gateway.c` 阅读指南

## 1. 先建立整体认识

`core/gateway.c` 不负责实现 JSON、TCP、协议帧或日志的底层算法。

它是 Gateway 的核心编排层，主要作用是把以下模块组织成完整的运行流程：

- Measurement 领域模型；
- Measurement JSON 编解码；
- TCP 网络通信；
- 应用层协议帧；
- 异步日志；
- 信号处理和优雅退出。

因此，不建议从文件第一行开始逐句阅读。更容易理解的方法是先从唯一的对外入口
`gateway_run()` 开始，再沿着函数调用关系逐层展开。

```text
apps/gateway/main.c
        │
        ▼
gateway_run()
        │
        ├── 初始化退出信号
        ├── 初始化异步日志
        │
        ├── Service 模式
        │      └── wait_for_stop()
        │
        └── Smoke 模式
               └── run_network_smoke()
                      ├── build_simulated_measurement() × 100
                      ├── measurement_to_json()
                      ├── send_frame()
                      ├── gateway_connection_recv_frame()
                      ├── measurement_from_json()
                      ├── measurement_equals()
                      └── 返回 MeasurementAck
```

一句话概括这个文件：

> `gateway.c` 负责把独立模块组织起来；它自己不实现各模块的底层算法，只决定先调用谁、数据交给谁，以及失败后如何收尾。

## 2. 第一层：理解 `gateway_run()`

`gateway_run()` 是整个文件的入口，可以把它看成 Gateway 的“导演”。

它主要完成四件事：

1. 检查调用方传入的配置是否合法；
2. 安装 SIGINT、SIGTERM 等退出信号处理；
3. 初始化异步日志器；
4. 根据运行模式选择 Service 流程或 Smoke 流程。

核心分支如下：

```c
if (config->mode == GATEWAY_MODE_SMOKE)
{
    result = run_network_smoke(&logger);
    goto SHUTDOWN;
}
```

两种运行模式的区别是：

- Service 模式：程序持续运行，等待 `Ctrl+C`、SIGINT 或 SIGTERM；
- Smoke 模式：在一条本地 TCP 连接上连续处理 100 条模拟 Measurement，
  验证全部请求和响应后退出。

无论中间成功还是失败，程序最后都会进入 `SHUTDOWN`，关闭并销毁异步日志器。

这里的 `goto SHUTDOWN` 主要用于集中清理资源。它可以避免在每一个错误分支中重复编写日志器关闭代码。

## 3. 第二层：理解 Service 模式

Service 模式的流程比较简单：

```text
gateway_run()
    ↓
打印 gateway running
    ↓
wait_for_stop()
    ↓
收到 SIGINT 或 SIGTERM
    ↓
记录退出日志
    ↓
关闭并销毁日志器
```

`wait_for_stop()` 每隔 100 ms 调用一次：

```c
graceful_shutdown_requested()
```

它不直接处理系统信号，而是等待 `graceful_shutdown` 模块通知它“程序应该退出了”。

可以把该函数简单理解为：

> 程序在这里等待，直到信号处理模块设置退出状态。

## 4. 第三层：理解 Smoke 模式

`run_network_smoke()` 是这个文件中最复杂的函数，但它做的事情并不神秘：

> 在同一个进程中同时模拟设备客户端和 Gateway 服务端，通过同一条 TCP
> 连接连续发送、处理和确认 100 条 Measurement。

整个测试可以拆成七步。

### 4.1 生成 100 条不同的模拟数据

```c
build_simulated_measurement(index, &request_measurement);
```

`index` 从 0 到 99。生成的数据有以下变化规律：

- `sequence` 从 1 连续增长到 100；
- `timestamp_ms` 每条增加 1 ms；
- `value` 每条增加 `0.125`；
- `quality` 循环经过 `good`、`uncertain`、`bad`。

如果 100 次都使用完全相同的结构体，重复或错序的数据可能不容易被发现。
这里主动改变字段，接收端再根据同一下标重新生成期望值并逐字段比较，能够
发现漏帧、重复、乱序或字段被改写。选用 `0.125` 是因为它能被二进制浮点数
精确表示，JSON 往返后可以安全地直接比较。

### 4.2 把结构体转换成 JSON

```c
measurement_to_json(
    &request_measurement,
    &request_json,
    &request_json_size);
```

这一步完成第一次数据形态转换：

```text
measurement_t 结构体
        ↓
JSON 字符串
```

生成的 JSON 会包含 Measurement 契约规定的全部字段，例如设备编号、序号、
时间戳、指标、数值、单位和数据质量。

`measurement_to_json()` 返回的字符串使用动态内存。每次 `send_frame()` 成功
返回后，完整帧已经复制到内核 TCP 发送缓冲区，因此当前 JSON 可以立即释放：

```c
measurement_json_free(request_json);
```

### 4.3 创建本地 TCP 客户端和服务端

Smoke 测试创建三个 socket 文件描述符：

| 变量 | 作用 |
|---|---|
| `listen_fd` | Gateway 服务端的监听 socket |
| `client_fd` | 模拟设备客户端 |
| `accepted_fd` | Gateway 接受客户端连接后得到的通信 socket |

它们之间的关系是：

```text
模拟设备 client_fd
        │
        │ TCP
        ▼
Gateway accepted_fd

listen_fd 只负责监听和 accept
```

监听地址使用 `127.0.0.1`，因此测试只在本机回环网络中运行，不访问外部网络。

绑定端口时传入 `0`：

```c
net_address_ipv4(&bind_address, "127.0.0.1", 0)
```

端口 `0` 表示让操作系统自动选择一个空闲端口。随后通过 `getsockname()` 查询操作系统实际分配的端口，再让客户端连接该端口。

### 4.4 客户端连续发送 100 条 Measurement

```c
send_frame(
    client_fd,
    (const unsigned char *)request_json,
    request_json_size);
```

第一层循环只发送、不等待 Ack，连续执行 100 次 `send_frame()`。它不会直接
发送裸 JSON，而是先使用 `frame_encode()` 添加协议字段，再通过 TCP 完整发送：

```text
JSON
 ↓ frame_encode()
协议帧
 ↓ net_send_all()
TCP 字节流
```

可以把协议帧理解成 JSON 外面的运输包装：

```text
[魔数][长度][JSON payload][CRC16]
```

其中：

- 魔数用于识别帧的起始位置；
- 长度表示 payload 有多少字节；
- payload 是 Measurement JSON；
- CRC16 用于检查传输内容是否损坏。

“先把 100 帧都发送出去”与“发送 1 帧、接收 1 帧、再发送下一帧”不同：
前者会让内核 TCP 缓冲区中同时存在多个相邻帧，更接近设备连续上报的数据流，
也能测试解析器跨越相邻帧边界时是否仍保持正确状态。

### 4.5 Gateway 连续接收并解析 100 条 Measurement

Gateway 一侧调用：

```c
gateway_connection_recv_frame(
    &gateway_connection,
    payload,
    &payload_size);
```

这个函数完成发送过程的反向操作：

```text
TCP 字节流
        ↓
frame_parser_feed()
        ↓
完整协议帧
        ↓
frame_parser_next()
        ↓
JSON payload
```

函数内部的 TCP 接收缓冲区故意只有 7 字节：

```c
unsigned char recv_buffer[7];
```

这样一个完整帧通常需要多次 `recv()` 才能读取完；由于发送端已经连续写入
100 帧，某次 7 字节读取也可能跨过两个相邻帧的边界。这个设计可以稳定覆盖
TCP 拆包和连续流中的跨帧读取，并验证增量解析状态是否正确保留。

恢复出 JSON payload 后，再调用：

```c
measurement_from_json(
    (const char *)payload,
    payload_size,
    &received_measurement);
```

于是完整的数据变化过程是：

```text
request_measurement
        ↓ measurement_to_json()
request_json
        ↓ frame_encode() + TCP
payload
        ↓ measurement_from_json()
received_measurement
```

最后使用 `measurement_equals()` 逐字段比较发送前和接收后的两个结构体：

```c
measurement_equals(
    &received_measurement,
    &request_measurement);
```

这里不直接使用 `memcmp()` 比较整个结构体，因为 C 结构体中可能存在未参与业务含义的填充字节。逐字段比较更准确，也更容易知道实际验证了哪些内容。

第二层循环对 100 条数据逐条执行上述解析和比较。只有每条的八个业务字段
都一致，才说明 Measurement 校验、JSON 编解码、帧协议和 TCP 通信能够连续
协同工作。

### 4.6 Gateway 连续返回 100 个确认响应

Gateway 根据解析后的 Measurement 序号构造响应：

```text
MeasurementAck{sequence=1,status=accepted}
...
MeasurementAck{sequence=100,status=accepted}
```

响应中的序号取自：

```c
received_measurement.sequence
```

而不是直接使用循环下标。这样可以证明 Gateway 确实解析并使用了收到的
Measurement 数据。

响应再次经过以下链路：

```text
Gateway
 ↓ send_frame()
TCP
 ↓ gateway_connection_recv_frame()
模拟客户端
```

### 4.7 客户端验收全部 Ack 和帧计数

第三层循环按顺序读取 100 个 Ack，并检查每个响应中的 `sequence` 是否为
1 到 100。最后还检查两个解析器的 `frame_ok`：

```text
Gateway ingress frame_ok = 100
Client response frame_ok = 100
```

逐条内容检查可以发现错序或错误响应，最终计数检查可以发现少处理或提前退出。
两项都通过后，整条 Smoke 链路才算通过。

## 5. `gateway_connection_t` 的作用

```c
typedef struct
{
    int fd;
    frame_parser_t parser;
} gateway_connection_t;
```

它把一条 TCP 连接和该连接自己的协议解析状态放在一起。

这是因为 TCP 是字节流，一次 `recv()` 不一定正好返回一个完整帧：

- 可能只收到前半帧；
- 可能收到一帧加下一帧的一部分；
- 也可能一次收到多个完整帧。

因此，每条连接都必须保存自己尚未处理完的字节和解析进度，不能让多个连接共用同一个解析器。

## 6. 每个辅助函数只记一句话

| 函数 | 一句话理解 |
|---|---|
| `wait_for_stop()` | 等待退出信号 |
| `build_simulated_measurement()` | 按下标生成字段会变化的模拟 Measurement |
| `measurement_equals()` | 逐字段比较两条 Measurement |
| `gateway_connection_init()` | 初始化一条连接及其帧解析器 |
| `gateway_connection_recv_frame()` | 从 TCP 字节流恢复一个完整 payload |
| `send_frame()` | 把 payload 包装成协议帧并完整发送 |
| `close_if_open()` | 安全关闭 socket 并把文件描述符改为 `-1` |
| `log_smoke_error()` | 统一记录并打印 Smoke 失败原因 |
| `run_network_smoke()` | 模拟 100 条 Measurement 的连续双向通信 |
| `gateway_run()` | 管理 Gateway 的完整生命周期 |

## 7. `DONE` 和 `SHUTDOWN` 的区别

文件中有两处集中清理入口，它们的层级不同。

### `DONE`

`DONE` 位于 `run_network_smoke()` 内部，负责清理单次 Smoke 测试创建的资源：

- Measurement JSON 动态内存；
- Gateway 已连接 socket；
- 模拟客户端 socket；
- 服务端监听 socket。

### `SHUTDOWN`

`SHUTDOWN` 位于 `gateway_run()` 内部，负责清理整个 Gateway 生命周期中的公共资源：

- 停止异步日志线程；
- 排空日志队列；
- 销毁日志器。

可以把两者理解为：

```text
gateway_run()                    整个程序运行层
    │
    └── run_network_smoke()      一次 Smoke 业务层
            └── DONE             清理网络测试资源
    │
    └── SHUTDOWN                 清理程序公共资源
```

## 8. 推荐阅读顺序

不要按照文件行号从上到下阅读，推荐按照以下顺序：

1. `gateway_run()`：先看程序入口和两种运行模式；
2. `run_network_smoke()`：理解完整业务链路；
3. `send_frame()`：理解数据如何进入协议帧和 TCP；
4. `gateway_connection_recv_frame()`：理解如何从 TCP 恢复 payload；
5. `measurement_equals()`：理解 Smoke 测试如何判断成功；
6. `wait_for_stop()`：理解 Service 模式如何等待退出；
7. 最后再看日志、错误处理和资源清理函数。

第一遍阅读时，建议先只看函数名、注释和成功路径，暂时跳过以下内容：

- `errno`；
- `goto DONE`；
- `goto SHUTDOWN`；
- 失败日志；
- 资源清理。

等成功路径清楚以后，第二遍再看错误处理，理解难度会低很多。

## 9. 调试时如何观察这条链路

可以先构建项目：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
```

然后直接执行 Smoke 测试：

```bash
./build/gateway --smoke
```

也可以通过 CTest 运行相关测试：

```bash
ctest --test-dir build --output-on-failure \
    -R 'measurement|gateway_smoke'
```

当前测试链路覆盖：

```text
Measurement 领域校验
        ↓
Measurement JSON 编码
        ↓
应用层协议帧编码
        ↓
本地 TCP 发送与拆包接收
        ↓
协议帧增量解析与 CRC 校验
        ↓
Measurement JSON 解码
        ↓
Measurement 字段比较
        ↓
MeasurementAck 返回与校验
```

## 10. 2026-08-25 主机与开发板验证记录

### 10.1 主机端步骤与结果

构建并运行完整测试集：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

结果为 `16/16` 通过，其中 `gateway_smoke` 通过。直接运行 Smoke 得到：

```text
SMOKE_PASS
gateway smoke passed: framed Measurement loopback; measurements=100 ingress_frames=100 response_frames=100
```

这三个数字分别表示：成功处理 100 条 Measurement、Gateway 成功解析 100 个
请求帧、模拟客户端成功解析 100 个响应帧。

另外使用 AddressSanitizer 和 UndefinedBehaviorSanitizer 重新构建，并运行
`gateway_smoke`、`frame_protocol`、`measurement`、`measurement_json` 四项
相关测试，结果为 `4/4` 通过，未报告内存泄漏、越界访问或未定义行为。

### 10.2 OK1126B-S 交叉构建步骤

使用仓库既有的 Buildroot GCC 12.4.0 SDK：

```bash
cmake \
    -S . \
    -B /tmp/edgevision-build-ok1126b-measurement100 \
    -DCMAKE_TOOLCHAIN_FILE=/home/zoukunbo/aarch64-buildroot-linux-gnu_sdk-buildroot/share/buildroot/toolchainfile.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON

cmake --build /tmp/edgevision-build-ok1126b-measurement100 \
    --target gateway measurement_test measurement_json_test frame_protocol_test -j
```

`file` 检查确认四个产物均为 ARM aarch64 ELF，动态加载器为
`/lib/ld-linux-aarch64.so.1`。

### 10.3 开发板执行步骤与结果

通过 ADB 地址 `192.168.0.232:5555` 连接开发板，把四个 ARM64 程序推送到
`/tmp` 后依次运行：

```bash
adb connect 192.168.0.232:5555
adb push /tmp/edgevision-build-ok1126b-measurement100/gateway \
    /tmp/edgevision_gateway_measurement100
adb push /tmp/edgevision-build-ok1126b-measurement100/measurement_test \
    /tmp/measurement_test_measurement100
adb push /tmp/edgevision-build-ok1126b-measurement100/measurement_json_test \
    /tmp/measurement_json_test_measurement100
adb push /tmp/edgevision-build-ok1126b-measurement100/frame_protocol_test \
    /tmp/frame_protocol_test_measurement100
adb shell /tmp/measurement_test_measurement100
adb shell /tmp/measurement_json_test_measurement100
adb shell /tmp/frame_protocol_test_measurement100
adb shell /tmp/edgevision_gateway_measurement100 \
    --smoke /tmp/gateway-measurement100.log
adb shell tail -n 5 /tmp/gateway-measurement100.log
```

板端环境与结果：

```text
Linux OK1126B-buildroot 6.1.141 aarch64 GNU/Linux
measurement tests passed
measurement JSON tests passed
frame protocol module tests passed
SMOKE_PASS
gateway smoke passed: framed Measurement loopback; measurements=100 ingress_frames=100 response_frames=100
```

结论：Measurement 领域校验、JSON 编解码、协议帧测试，以及同一 TCP 连接上
连续 100 条请求和 100 条 Ack 的完整链路均在 OK1126B-S 实机通过。板载 RTC
仍显示 2024-01-24，因此日志中的板载时间不是本次实际验收日期；实际执行日期
以本节标题的 2026-08-25 为准。
