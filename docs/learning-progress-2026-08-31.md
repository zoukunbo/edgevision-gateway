> **2026-09-02 严格审计更正（优先于本文后续旧表述）**
>
> D36～D42 按原始 DoD 均应为“进行中”，严格验收均未通过。此前“D39/D40/D41/D42 完成”只表示对应最小示例或链路曾跑通，不能代表可复用模块、主工程接入和原始验收完成。D36 的 Notion“验收通过”属于误勾。SQLite/Outbox 仍位于 `examples/storage`，WAL 尚未实现，默认 Gateway 仍使用模拟源，板端 systemd 当前启动的是 Outbox oneshot 示例。
>
> 后续会话请先读 `docs/week06-d36-d42-audit-and-next-plan-2026-09-02.md` 和 `docs/modbus-sqlite-outbox-beginner-tutorial-2026-09-02.md`，并以严格审计结论为准。

# 学习进度与接续状态｜2026-08-31

> 2026-09-01接续更新：A块真实STM32到MQTT、B块SQLite/Outbox最小闭环、C块D41最小正式部署和D42最小可审计回放均已完成；Notion已同步。

> 当前入口。优先于2026-08-30的D36收尾快照及早期“没有真实从站”的描述。
> 2026-08-31原始记录保留；2026-09-01新增了真实STM32 MQTT证据、SQLite/Outbox练习、构建和本机Broker回放，未重新烧录STM32。

## 当前到哪一层

真实链路已通：DHT11 → STM32 → SP3485/RS485 → OK1126B UART5 → 04事务 → 两条Measurement → WSL Broker → 独立订阅端。
SQLite/Outbox链路也已通：历史Measurement回放 → 原子保存 → pending跨进程 → 连接失败保留 → Broker恢复补发 → PUBACK后sent。
D41最小正式部署已通：干净ARM64构建 → WSL NFSv4只读包 → 板端systemd oneshot → /userdata持久库 → 独立订阅端；板卡重启后自动挂载、服务运行和数据库状态均恢复。
默认gateway程序仍用simulated_source；当前正式部署的是Outbox单次投递程序，不表示主程序真实源已经接入。

## 学习块状态

| 项目 | 已完成 | 仍保留的边界 |
| --- | --- | --- |
| D36基础 | 原始双向字节、方向控制、取消/关闭/重开、安全基线记录 | 严格验收仍未完成；新从站已接入，但20组事务、完整电气测量、断电复现仍缺 |
| A：D37+D38 | 03/04请求响应、映射；PC模拟MQTT；真实STM32经04到两条Measurement、WSL Broker和独立订阅端 | 默认Gateway源未切换；硬实时与压力可靠性未验证 |
| B：D39+D40 | 单条持久化；Measurement与Outbox原子提交/回滚；pending跨进程；未确认保留；真实PUBACK后sent；真实连接失败后恢复补发 | at-least-once；PUBACK到sent写回崩溃窗口可能重复；无并发领取、退避、死信或exactly-once |
| C：D41+D42 | D41最小正式部署完成：干净ARM64构建、NFS只读包、systemd开机投递、/userdata持久库、反向隧道、独立订阅和重启恢复 | D41严格验收仍缺30分钟运行、三类故障注入和Git tag/commit；D42最小可审计回放包已完成；严格验收仍缺四类帧corpus、ReplayDataSource、真实/回放对照和W07 Top-3 |

学习经历：用户已练习并表示理解事务收帧与映射关键逻辑；不能从CRC、声明或基础串口重新教学。
本次TIOCOUTQ/TEMT方向切换修正由助手代做，下一轮可用一个完整发送行为简述关键决策，不以反复考试阻碍推进。
按意义完整的行为学习：说明目的、2～3个关键点、输入输出、完成标准与不做项；每天主动学习最多6小时。

Notion口径：2026-09-01已读取并更新。D39、D40标为完成但严格验收保持未勾选；D41记录最小正式部署完成并保留30分钟/故障注入/Git边界；D42记录最小审计完成，严格验收保持未勾选。

## 工程与设备

- Windows + WSL Ubuntu-22.04。
- Linux工程：/home/zoukunbo/project/edgevision-gateway。
- STM32工程：F:\江科大学习资料\Modbus-RTU。
- 开发板：OK1126B-S，最近确认root@192.168.0.232，hostname=OK1126B-buildroot，aarch64；新会话使用前核对。
- UART5=/dev/ttyS5，115200、8N1；GPIO0_C6=P16 Pin7=/dev/gpiochip0 offset22，低RX、高TX。
- 当前用户说明为PC、开发板、STM32三者A/B并接；开发板发请求时PC COM9只监听，关闭自动发送/应答。
- 串口工具能读出字节不等于电气安全项目全部验收。改线应断电，不能任意试反接或更改供电。

## 协议、数据与预算

- 真实STM32：slave1，04，从0x0001读取2个输入寄存器；温度、湿度各除以10。
- 请求：01 04 00 01 00 02 20 0B。最近无诊断延时响应：01 04 04 01 05 01 18 EB E3。
- 原始261/280 → 26.1°C/28.0%RH，来源stm32-dht11-01；两条记录共用网关接收时间戳，演示序号1/2。
- STM32源码温度为int16补码；无有效数据、最近采样失败或成功数据年龄>=7000ms时，04返回异常码04。
- 网关暂只接受原始0..1000，未支持负温度映射；quality仍保守为uncertain。以上不等于传感器精度/量程验收。
- 程序还保留早期启动文案“failure/freshness encoding not yet confirmed”；现有注释已说明该文字滞后，不能据此重复索要已有固件规则。
- 正常帧先3字节再6字节；83/84异常先3字节再2字节CRC；与请求匹配后才交付寄存器。
- 两段接收共用CLOCK_MONOTONIC绝对截止时间（纳秒），总接收预算1000ms，在发送成功后生成。
- serial_wait_tx_complete先检查TIOCOUTQ=0且TEMT成立。发送前检查和write后等待各100ms，忙时请求休眠50us再查询。
- write_full仍可能阻塞，不支持完整发送取消；这些独立预算不是完整端到端预算。用户态调度也不提供硬实时保证。

## 最近解决的故障

旧程序用tcdrain之后才切RX。PC可见请求，STM32回复却收不到；PC发送同样请求能成功。
strace截图显示tcdrain约8.771ms，TX保持约9.1ms；源码显示STM32在请求后约1.837ms判帧并安排回复。
临时在STM32通信任务回复前加20ms后读取成功，支持释放总线偏晚的假设。
助手核对SDK的8250驱动（包括DMA状态）后，用真实队列/TEMT检查替换旧等待路径，保留完成检查和失败清理。
随后撤回STM32唯一一处诊断延时，按原Keil工程重新编译、ST-Link烧录校验并运行，实板无strace读取成功。
没有波形证据，不能声称精确测得总线冲突时刻，也不要把诊断20ms加回作为永久解决。
STM32中ulTaskNotifyTake(...20ms)是任务最大空闲等待，由帧通知提前唤醒，不是回复延时，不能误删。

## 可核验的结果

| 证据 | 结果 |
| --- | --- |
| hardware/rs485/2026-08-30-source-stop.log | 历史SIGINT/SIGTERM、关闭重开 |
| hardware/rs485/2026-08-31-modbus-pc.log | PC模拟03正常/不应答超时；非真实传感器 |
| hardware/rs485/2026-08-31-modbus-measurement-mqtt.log | 固定模拟25/50的主机MQTT验证 |
| hardware/rs485/2026-08-31-modbus-live-mqtt.log | PC现场模拟26/51经开发板发布，独立订阅端核对 |
| hardware/rs485/2026-08-31-stm32-delay20-195508.log | 旧Linux+STM32诊断等待，26.0°C/27.0%RH |
| hardware/rs485/2026-08-31-stm32-temt-no-delay-200506.log | 新Linux+无诊断等待，26.1°C/28.0%RH，exit0 |
| hardware/rs485/2026-08-31-stm32-no-delay-firmware.log | Keil构建0错误0警告；Verify OK/Application running；烧录时源码与HEX哈希 |
| hardware/rs485/2026-08-31-tx-complete-ctest.log | 最近完整主机CTest 24/24通过，约7.54s |

ARM64 MQTT=OFF与ON的modbus_rtu_demo均已编译成功。主机测试中的PTY/GPIO/TEMT替身不是电气证据。
更早PC模拟试验出现额外01 83 03 01 31，来源没有独立查明；当前读取成功不能倒推那项已解决，若重现须保留记录。

本次注释整理涉及4个Linux文件与STM32 AppRTOS.c；逐文件核对非注释C词法标记相同，运行逻辑与字符串未改。
核对记录：build-d36-closeout/comment-only-audit-20260831.json。
源码哈希和行号因注释变化已更新，旧烧录日志中的源码哈希是当时证据，不要求与现在注释版一致；现有二进制未替换。

## 当前可运行产物

最近已部署的正确程序：

```sh
ssh root@192.168.0.232 '/tmp/edgevision-stm32-temt-KZ2R2d/modbus_rtu_demo --stm32'
```

此目录是MQTT关闭构建，只用于真实读数/JSON。旧/tmp/edgevision-stm32-uZRxb7保留的是修正前程序，不要误用。
/tmp可能重启后消失；使用前检查，不要求重建整个环境。

- 主机目录：build-d36-closeout（Debug、MQTT ON）。
- ARM目录：build-arm64-rs485（MQTT OFF）。
- ARM MQTT目录：build-arm64-modbus-mqtt（MQTT ON）；其程序已用新临时目录完成真实STM32发布验证。
- ARM Outbox正式构建：build-arm64-storage-deploy；NFS正式包：deploy/nfs-root/edgevision-outbox。
- 板端正式状态：/mnt/edgevision只读挂载；/userdata/edgevision-gateway/gateway.db持久保存；edgevision-outbox.service为oneshot。
- 工具链：/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/share/buildroot/toolchainfile.cmake。
- MQTT库：同一host下aarch64-buildroot-linux-gnu/sysroot/usr/lib/libmosquitto.so.1；板端原有OpenSSL依赖此前已用过。
- STM32原构建工具：D:\Keil_v5\UV4\UV4.exe；Project.uvprojx，Objects/Project.axf和Project.hex。无需再烧录来重复本轮成功。

## 下一轮唯一建议入口

继续D42严格验收的第一个最小行为：只从现有日志中盘点 normal、timeout、CRC错误、设备断开四类可用原始输入，建立 corpus 清单和缺口表。先读 docs/d42-auditable-replay-2026-09-01.md 与 hardware/storage/d42-auditable-replay-2026-09-01.log。

不查询STM32，不发送硬件请求，不重做A/B/D41，不立即实现ReplayDataSource。先确认四类证据哪些已经存在、哪些确实缺失，再决定最小代码范围。默认Gateway真实源、D41长时间/三故障/Git版本点仍是独立边界。
