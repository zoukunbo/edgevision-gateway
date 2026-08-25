# edgevision-gateway

嵌入式 Linux 学习计划的长期集成项目。

## 当前能力

- W04 有界队列异步日志器
- 收到 SIGINT/SIGTERM 后优雅退出
- `modules/net` 中可复用的 IPv4 TCP/UDP 封装
- 有长度上限的 TCP 应用层帧、增量解析和 CRC16-Modbus
- TCP/UDP 示例和异常路径测试
- `gateway --smoke` 协议帧 Measurement 环回验证
- 100 个连接、100 MiB 数据的压力测试，并校验数据内容和文件描述符
- CMake 构建、CTest 测试、ASan 和 UBSan 内存安全检查

## 当前目录分层

```text
apps/gateway/          程序入口，只负责参数解析和启动
core/                  网关业务编排，组织各个基础模块
modules/net/           网络通信与事件循环
modules/protocol/      应用层协议帧的编码、解析和校验
modules/log/           异步日志
modules/runtime/       信号处理和优雅退出
modules/blocking_queue/并发基础组件
examples/              独立学习示例
tests/                 自动化测试
docs/                  中文学习与验证记录
```

本阶段只新增真正承载业务编排的 `core/`，没有提前创建空的设备管理、驱动、
线程池和命令行界面目录。后续对应功能开始实现时，再逐步增加目录和模块，
避免一次性搬迁造成包含路径、构建脚本和测试大面积变化。

## 构建和验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

运行 Gateway 冒烟测试（在同一 TCP 连接上连续处理 100 条模拟 Measurement）：

```bash
./build/gateway --smoke
```

运行 `./build/gateway` 后按 `Ctrl+C`，可以验证进程是否安全退出。

## 文档

- [编译、CMake、CTest 与内存安全检查](docs/build-cmake-ctest-sanitizers.md)
- [D29 网络入口验证](docs/d29-network-validation.md)
- [D29 OK1126B-S 板端验证证据](docs/d29-ok1126b-board-evidence.md)
- [D32 TCP 协议帧与 Modbus RTU 对照](docs/d32-tcp-framing-modbus-rtu.md)
- [Gateway 核心代码阅读与 100 条 Measurement 实机验证指南](docs/gateway-code-reading-guide.md)
