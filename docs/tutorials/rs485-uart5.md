# OK1126B-S UART5/RS485 开发教程

本文只讲 OK1126B-S 上的 UART5、GPIO 方向控制和半双工 RS485。NFS、交叉编译与部署已移到独立运维文档，实板运行结果也不再混入教程。

- NFS 开发挂载：[NFS 开发环境运维指南](../operations/nfs-development.md)
- ADB/SCP/NFS 部署：[快速部署指南](../operations/quick-deployment.md)
- Buildroot 与交叉编译：[Buildroot 第三方库与 SDK](../operations/buildroot-third-party-library-integration.md)
- 历史实板证据：[STM32 DHT11 Modbus 读取记录](../records/stm32-dht11-modbus-read.md)
- 当前阶段审计：[W06 D36～D42 严格审计](../records/week06-d36-d42-audit-and-next-plan-2026-09-02.md)

## 1. 硬件连接与 Linux 映射

| OK1126B-S P16 | SoC/功能 | SP3485 | Linux 映射 |
| --- | --- | --- | --- |
| Pin 8 | UART5_TX | DI | `/dev/ttyS5` |
| Pin 10 | UART5_RX | RO | `/dev/ttyS5` |
| Pin 7 | GPIO0_C6 | DE 与 `/RE` 并联后的 RSE | `/dev/gpiochip0` line 22 |
| 3.3V | 电源 | VCC | 不要接 5V |
| GND | 地 | GND | 两端共地 |

串口参数为 115200、8N1、无软硬件流控、raw 模式。RSE 高电平发送，低电平接收。A/B 线应按设备丝印和手册核对，不能仅凭线色判断。

GPIO0_C6 的 line offset 为：`0 * 32 + C * 8 + 6 = 22`。物理引脚号、SoC GPIO 名和 Linux line offset 是三个不同概念。

## 2. 代码位置与资源生命周期

```text
modules/serial/
├── rse_control.h/.c       GPIO character device v2 方向控制
└── rs485_serial.h/.c      termios、发送、截止时间和接收

modules/source/
└── real_serial_source.h/.c  串口资源和原始接收缓冲区的上层封装
```

正确生命周期：

```text
初始化对象 -> 打开 TTY/GPIO -> 多次收发 -> 停止收发 -> 统一关闭
```

不要每帧重新打开串口或 GPIO。打开后的对象不能按值复制，外部也不能单独关闭对象内部的 fd。失败回滚要保存最初的 `errno`；关闭应尽力恢复 RX 状态。

## 3. 半双工发送

`write()` 可能部分写入，也可能被信号中断，因此发送实现必须持续写入剩余区域，直到所有字节进入内核队列。

当前可靠的方向切换顺序是：

```text
RSE=TX
  -> write_full
  -> 等待 TIOCOUTQ=0 且 TIOCSER_TEMT
  -> RSE=RX
```

早期实现使用 `tcdrain()`；2026-08-31 实板排障发现，仅依赖它不足以可靠判断硬件移位寄存器已经发送完毕。历史过程和证据见 STM32 实板记录，教程只保留当前做法。

任何发送错误都要尝试切回 RX，但清理错误不能覆盖原始失败原因。

## 4. 接收、超时与停止

串口只有一个读取者时使用 `poll()` 即可。一次 `read()` 不等于一帧，读取层只负责字节和时间预算，CRC、功能码、长度等属于 Modbus 协议层。

核心接口语义：

- `serial_read_exact_timeout`：在总超时内累计指定长度。
- `serial_read_exact_until_stop`：多个读取阶段共享同一个 `CLOCK_MONOTONIC` 截止时间。
- `serial_read_exact_timeout_stop`：定期调用停止回调，取消时返回 `ECANCELED`，并保留已收字节。

返回值统一为：`1` 表示收齐、`0` 表示总超时、`-1` 表示错误。超时预算只计算一次，`poll()` 或 `read()` 被 `EINTR` 打断后只能使用剩余预算，不能重新获得完整超时时间。

## 5. RS485、UART 和 Modbus 的边界

- RS485：电气层，定义差分线和半双工方向。
- UART/termios：字节流层，定义波特率、数据位和读写行为。
- Modbus RTU：协议层，定义站号、功能码、寄存器、长度和 CRC。

“成功收到 8 字节”只能证明串口读取行为，不能证明 Modbus 帧有效。完整协议讲解见 [Modbus、SQLite 与 Outbox 教程](modbus-sqlite-outbox.md)。

## 6. 构建与最小验证

```bash
cmake -S . -B build-arm64-rs485 \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DEDGEVISION_ENABLE_MQTT=OFF
cmake --build build-arm64-rs485 --target rs485_demo -j2
file build-arm64-rs485/rs485_demo
```

`file` 输出必须是 ARM aarch64，不能是 x86-64。构建完成后按 [ADB、SCP 与 NFS 快速部署指南](../operations/quick-deployment.md) 选择部署方式。

验证时依次确认：

1. TTY 参数和 GPIO line 正确。
2. 发送结束后确实切回 RX。
3. 完整接收、部分超时和主动取消都保留正确的字节数。
4. SIGINT/SIGTERM 后资源可以重新打开。
5. Modbus 层另外验证长度、站号、功能码和 CRC。

## 7. 常见故障

| 现象 | 优先检查 |
| --- | --- |
| 连续收到 `00` | termios 是否配置为 raw、115200、8N1 |
| 帧尾缺失 | 是否等待发送器真正空闲后才切回 RX |
| GPIO 返回 `EBUSY` | line 22 是否仍被其他进程或 sysfs 占用 |
| 运行的还是旧程序 | `file`、`sha256sum`、路径和时间戳 |
| 定长读取成功但协议失败 | Modbus 长度、站号、功能码和 CRC |
| Ctrl+C 后不能再次启动 | 关闭路径是否释放 TTY/GPIO 并恢复 RX |

历史测试数量、临时目录和当时的完成度统一保存在 `docs/records/`，不再作为本教程的当前结论。
