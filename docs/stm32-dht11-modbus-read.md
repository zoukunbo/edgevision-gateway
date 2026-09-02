# STM32 DHT11：一次真实温湿度读取

更新：2026-08-31。已完成发送方向切换修正，并在移除STM32诊断延时后实板读取成功。
本页后半部分记录对照、烧录与最终部署入口；前期离线记录保留为历史证据。

## 本段目的与边界

复用串口、GPIO 方向控制、接收截止时间和 CRC，把 STM32 的两个输入寄存器转换成两条 Measurement。
关键点：请求与响应功能码一致；寄存器语义及比例；通信成功与传感器采样有效性分开判断。
本段只读一次并输出 JSON，不修改 STM32 配置，不做循环采集、复杂重试、SQLite 或 Outbox。
默认 gateway 程序仍使用原模拟源；新增入口在 modbus_rtu_demo。

## 已确认的设备约定

- UART5：115200、8N1；从站地址 1。
- gpiochip0 offset 22：低电平接收，高电平发送，沿用已验证接口。
- 功能码 04，起始地址 0x0001，数量 2。
- 第一个寄存器为温度、第二个为湿度；均除以 10。
- 请求：`01 04 00 01 00 02 20 0B`。
- 用户截图响应：`01 04 04 01 09 01 4A AA 1D`，即 265/330 → 26.5°C/33.0%RH。

随后核对STM32源码：温度为int16补码；未曾成功、最近采样失败或成功数据年龄达到7000ms时，
04查询返回异常码04，不把保留的旧值作为正常回复。对应AppRTOS.c的flags和Modbus.c的检查。
网关当前仍只接受原始值0..1000，即温度0..100°C、湿度0..100%的临时应用保护范围；
这不是DHT11量程声明，也没有在本次排障中扩展负温度支持。
STM32来源为stm32-dht11-01；本轮保留quality=uncertain，不顺手调整质量策略。
时间戳是网关接受数据时的时间，不是设备采样时刻；单次演示序号为 1、2。

## 接入位置

- modules/protocol/frame.c/h：新增仅支持 03/04 的通用构造和校验入口，旧 03 接口保留。
- examples/serial/modbus_rtu_demo.c：--stm32 选择 04/地址1，映射比例改为 0.1。
- 正常响应先读 3 字节头，再读 6 字节；84 异常响应读头后再读 2 字节 CRC。
- 两次读取共享同一个 CLOCK_MONOTONIC 绝对截止时间；总接收预算 1000 ms。
- 该预算在发送完成后开始；新的发送完成等待另有预算，write仍无总超时保证，不是端到端预算。
- 通过完整校验后才提取两个寄存器；失败不输出测量值。

## 下一次现场运行

先确认开发板当前 IP。以下命令仅在确认仍为 192.168.0.232 后，在 WSL 执行。
新的接线应在断电状态核对供电、电平、A/B、参考地和方向控制；不要带电改线。
确保 UART5 无其他程序占用，PC 模拟器及自动发送已停止，总线上只有开发板发起请求。
此前额外帧 01 83 03 01 31 的来源仍未查明，不能把历史模拟链路成功当作总线已排查完毕；
本次应隔离旧模拟路径，若额外帧再现则停止收发并保留日志。

```sh
cd /home/zoukunbo/project/edgevision-gateway
cmake --build build-arm64-rs485 --target modbus_rtu_demo -j2
ssh root@192.168.0.232 'hostname; uname -m'
```

确认是目标 OK1126B 开发板后，复制到新建的临时目录并运行一次：

```sh
task_remote_dir=$(ssh root@192.168.0.232 'mktemp -d /tmp/edgevision-stm32-XXXXXX')
test -n "$task_remote_dir" && scp build-arm64-rs485/modbus_rtu_demo "root@192.168.0.232:$task_remote_dir/modbus_rtu_demo"
test -n "$task_remote_dir" && ssh root@192.168.0.232 "$task_remote_dir/modbus_rtu_demo --stm32"
```

SCP 只复制新程序；SSH 在板端启动。临时目录不会覆盖此前部署的二进制，也不改启动项。
务必带 --stm32：无参数和 --serial-mqtt 仍是原 PC 模拟 03 模式。
正常应看到请求、响应、QUERY OK、两条 JSON 和 RS485 closed；实际温湿度随环境变化。
异常时应明确打印超时、84 异常码或无效响应，不伪造温湿度。
本次停止点是一次真实读取与设备显示核对；若现场验证不应答超时，应先停止程序、断电再调整接线。

另有 --stm32-mqtt 可复用已有 MQTT 出口，但需 MQTT 开启构建及原有 Broker/动态库条件。
它不属于本次首轮现场验证，也不具备断网补发或两条记录原子发布能力。

## 接入初期的离线验证（历史记录，不是实板证据）

- 主机 CTest：23/23 通过，已有 frame_protocol_test 增加 04 请求、正常响应、84 异常和错误 CRC 检查。
- ARM64：MQTT 关闭及开启版本的 modbus_rtu_demo 均构建成功。
- 离线 socket 流测试：截图请求响应、265/330 映射、84 异常、不应答超时、越界不改输出、旧 PC03 回归通过。
- 离线测试使用发送替身，未访问 UART、GPIO、STM32 或 MQTT。
- 临时复核记录：build-d36-closeout/stm32-integration-review-8ggimkga/result.log。
- 这一阶段尚未运行新版板端程序；后续现场结果见下文，仍不更新 D36 完整硬件验收结论。

## 发送方向切换修正与无诊断延时的实板结果

2026-08-31 20:05（Asia/Shanghai）：真实读取成功，当前这一段完成。

### 为什么改

用户的 strace 截图显示旧程序 tcdrain 等待约 8.771 ms，TX方向保持约9.1 ms。
PC能收到完整请求，却看不到回复；同一接线由PC发请求能正常回复。
在STM32通信任务中临时增加20ms回复等待后，旧Linux程序得到26.0°C/27.0%RH。
这些对照支持主站释放总线偏晚的判断，但没有电气波形，不能宣称已测得具体冲突时刻。

SDK serial_core.c 的 uart_wait_until_sent 使用节拍轮询；板端 /proc/config.gz 确认 CONFIG_HZ=250。
核对8250的tx_empty实现：DMA不忙且硬件FIFO/移位寄存器均为空时才返回TEMT；
serial_core的TIOCSERGETLSR还会检查待发送软件队列。相关语义见
[Linux UART驱动接口](https://www.kernel.org/doc/html/v5.15/driver-api/serial/driver.html)。

### 实际变更

- rs485_serial.c/h：用TIOCOUTQ=0与TIOCSERGETLSR/TIOCSER_TEMT共同判断完成。
- 每次忙状态后请求休眠50us再检查；50us不是发送完成时间，也不是调度延迟上限。
- 进入TX前确认本机发送端为空并验证接口支持；不是检测整个A/B总线是否空闲。
- 发送前检查与write后等待分别使用100ms单调时钟预算。write仍可能阻塞，不是端到端预算。
- 不支持状态查询时在发送前失败，不通过猜测发送时间或静默退回旧路径掩盖问题。
- 写入/等待失败则尽力清除本机待发送队列并恢复RX，保留原始errno，不自动重试。
- 只针对已核对的OK1126B原生8250 UART；换USB串口或其他驱动必须重新核对语义。
- STM32 User/AppRTOS.c只移除临时20ms等待，其他采样、显示、协议逻辑未改。

### 构建、烧录与验证

- 新增serial_tx_complete测试：不提前切回RX、软件队列与硬件状态分别忙、短写/EINTR、
  不支持接口、等待超时清理和错误保留。原PTY源测试仅补TEMT替身，字节读写仍走PTY。
- 完整主机CTest：24/24通过；ARM64 MQTT关闭与开启的modbus_rtu_demo均构建成功。
- STM32原Keil工程构建：0错误、0警告；仅一个ST-Link被检测到。
- 按原工程下载配置执行，Keil报告Erase Done、Programming Done、Verify OK、Application running。
- 未更改下载器配置、选项字节、Linux镜像或启动项。
- 无strace、无STM32诊断延时，运行一次得到：

```text
TX 01 04 00 01 00 02 20 0B
RX 01 04 04 01 05 01 18 EB E3
QUERY OK: reg[0]=261 reg[1]=280
temperature=26.1 celsius; humidity=28 percent
source=stm32-dht11-01; quality=uncertain
RS485 closed; exit=0
```

当前已部署的新程序（旧目录保留，不要误运行旧二进制）：

```sh
ssh root@192.168.0.232 '/tmp/edgevision-stm32-temt-KZ2R2d/modbus_rtu_demo --stm32'
```

Linux二进制SHA256：b885a133a4fa7feb2dd599853f68f678b4f2a756fba96427a5d906d84e55a01d。
STM32 HEX SHA256：0b482afbde854fe5483393aeb2a6e39a09c28b54e35f5f633b532c4ad0458887。
STM32 AppRTOS.c SHA256：799d64db099f48a63ac454360a4cc421acfcac8f6c87fa82d1dfddf18cf82354。

记录：
- [无临时延时的实板读取](../hardware/rs485/2026-08-31-stm32-temt-no-delay-200506.log)
- [旧程序加20ms对照](../hardware/rs485/2026-08-31-stm32-delay20-195508.log)
- [STM32构建/烧录与文件清单](../hardware/rs485/2026-08-31-stm32-no-delay-firmware.log)
- [最终主机回归记录](../hardware/rs485/2026-08-31-tx-complete-ctest.log)

停止点：本次不再重复现场请求，不追加MQTT或持久化。没有进行高负载、断电复现、
完整电气测量或20组真实事务，不替代原D36严格验收。用户态调度仍无硬实时保证。
两个值的quality沿用保守uncertain，本轮没有扩大负温度映射或质量策略的修改范围。

## 新对话交接

完整学习状态见[学习进度总结](learning-progress-2026-08-31.md)，可复制的接续要求见[新对话提示词](next-chat-prompt-2026-08-31.md)。
本轮仅补注释，未更新板端二进制、STM32固件或Notion；源码注释变化后的哈希与烧录当时的源码哈希不同是正常现象。
