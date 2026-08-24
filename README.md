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

## 构建和验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

运行 Gateway 冒烟测试：

```bash
./build/gateway --smoke
```

运行 `./build/gateway` 后按 `Ctrl+C`，可以验证进程是否安全退出。

## 文档

- [编译、CMake、CTest 与内存安全检查](docs/build-cmake-ctest-sanitizers.md)
- [D29 网络入口验证](docs/d29-network-validation.md)
- [D29 OK1126B-S 板端验证证据](docs/d29-ok1126b-board-evidence.md)
- [D32 TCP 协议帧与 Modbus RTU 对照](docs/d32-tcp-framing-modbus-rtu.md)
