# PC模拟寄存器到Measurement：A块第二段

本段目的：把已经校验的原始寄存器赋予明确语义，交给现有Measurement和JSON接口。
当前状态（2026-08-31）：映射与主机样本上报已通过；随后以PC模拟26、51完成实板串口到MQTT订阅端的整链核对。A块最小软件学习闭环达到停止点。额外帧来源未明，硬件/真实设备验收仍独立保留；默认Gateway数据源未替换。

## 本练习的模拟寄存器表

下面是项目为PC模拟器定义的约定，不是DHT11或其他真实设备的手册。
第一段中的25、50此前只是原始寄存器；从本段起按本表解释为模拟值。

| 地址 | 类型 | 含义 | 比例 | 本练习允许范围 | Measurement |
|---|---|---|---|---|---|
| 0 | uint16 | 模拟温度 | 1 count = 1℃ | 0..100 | temperature / celsius |
| 1 | uint16 | 模拟相对湿度 | 1 count = 1% | 0..100 | humidity / percent |

一个Measurement只有一个metric/value/unit，所以一次采样形成两条记录。
device_id固定为pc-modbus-sim-01，不能借用真实设备ID。
quality=good只表示数据符合本模拟表及领域契约，不是硬件验收结论。

## 输入输出和时间

- 输入：03响应校验成功后得到的两个uint16寄存器、UTC Unix毫秒时间、首条序号。
- 输出：temperature、humidity各一条Measurement；两个记录共用时间，序号相邻。
- 时间指网关接受此次数据的时刻，不是设备内部测量时刻。
- Measurement时间使用CLOCK_REALTIME；接收预算继续使用CLOCK_MONOTONIC。
- 单次demo序号为1、2；不承诺进程重启后序号持续递增。
- 任何映射失败都不修改输出；串口超时/异常/坏帧不调用映射，不伪造零值。
- measurement_validate检查领域字段，但不会知道本模拟表的温湿度上限；映射层需先检查范围。

## 这次只练一个行为

已在examples/serial/modbus_rtu_demo.c的map_pc_registers中完成：
1. TODO M1：范围检查和数值映射到局部pending。
2. TODO M2：复用领域校验，两个记录都合法后才交付output。

字段初始化、时间戳、JSON编码和释放均已提供；不重写CRC/串口/JSON。

主机无硬件入口（在WSL项目目录执行）：

```sh
cmake --build build-d36-closeout --target modbus_rtu_demo -j2
./build-d36-closeout/modbus_rtu_demo --map-sample
```

--map-sample直接使用固定寄存器25、50，不打开串口或GPIO；不是新的现场通信证据。
映射已完成：正常样本输出两条模拟Measurement JSON；任一寄存器为101时被拒绝，原有两条输出保持不变。
无参数运行仍是实板事务入口，只在QUERY_OK后调用映射；此阶段不自动部署。

最小验收：正常样本得到模拟25℃和50%的两条合法JSON；
越界样本被拒绝且输出保持不变。复用已有Measurement/JSON测试，不扩大测试矩阵。

默认--map-sample只输出JSON；新增--map-sample-mqtt显式调用Gateway使用的mqtt_publisher模块。
不改默认Gateway，不添加SQLite/Outbox或新硬件。主机样本出口验证与实板串口整链证据分别记录，不能混称。

## 已有MQTT出口的主机样本验证（2026-08-31）

本次没有重写MQTT协议/线程/JSON：沿用mqtt_publisher_create、start、
wait_connected、publish、destroy；只有publish确认对应QoS 1 PUBACK才报告成功。
这个入口复用Gateway的出口模块，不是修改gateway默认模拟数据源或生产采集服务。

构建目录build-d36-closeout已启用EDGEVISION_ENABLE_MQTT=ON。
build-arm64-rs485仍保持MQTT=OFF，原有串口/JSON路径继续可构建。

两个WSL终端分别执行（均在项目目录）：

```sh
mosquitto_sub -h 127.0.0.1 -p 1883 -t edgevision/v1/devices/pc-modbus-sim-01/measurements -q 1 -v -R -C 2 -W 15
```

先让订阅端连接就绪，再执行：

```sh
cmake --build build-d36-closeout --target modbus_rtu_demo -j2
./build-d36-closeout/modbus_rtu_demo --map-sample-mqtt
```

实际自动验证使用独立订阅进程，并等待其SUBACK后才启动发布。
未修改或重启本机Broker，不发送到外部公共Broker。
两条模拟记录value=25/50、unit=celsius/percent，device_id=pc-modbus-sim-01，
sequence=1/2，共用同一timestamp_ms；发送端得到两次PUBLISH_CONFIRMED，
订阅端接收的完整JSON逐字段相等。

日志：[主机样本到MQTT证据](../hardware/rs485/2026-08-31-modbus-measurement-mqtt.log)。

证据边界：
- 本次输入是固定寄存器样本，未访问COM9或开发板，不声称本次重新经过UART。
- 之前的实板03正常/超时证据独立保存在2026-08-31-modbus-pc.log。
- 两条发布彼此独立；第二条失败不会撤回第一条。这里不保证原子批次、重启补发或exactly-once。
- PUBACK表示Broker确认，不等于所有订阅者都已处理；本次另有一个独立订阅者的实际接收证据。
- 映射与发布不是默认Gateway真实采集源已经接通的声明，也不替代真实传感器验收。

## PC模拟从站的实板整链核对（2026-08-31 16:58，Asia/Shanghai）

A块既定最小软件学习闭环已跑通：COM9模拟应答 → OK1126B UART5/GPIO22 →
03顺序事务 → 两条Measurement → 现有mqtt_publisher → WSL Broker → 独立订阅端。
这不是原D36/D37/D38真实传感器及完整电气验收通过；Notion原验收框没有自动改动。

新增显式模式--serial-mqtt：
- 无参数仍执行串口查询并输出JSON，不自动上报。
- --map-sample和--map-sample-mqtt仍使用主机固定样本25、50，不访问串口。
- --serial-mqtt只使用query_two_registers返回的真实串口缓冲结果，成功后映射并上报。
- MQTT关闭的构建若收到--serial-mqtt，会在打开硬件前拒绝。
- 默认gateway可执行程序的数据源没有替换；该整链demo复用Gateway已有出口模块。

本次刻意使用不同于固定样本的模拟寄存器26、51：

```text
TX 01 03 00 00 00 02 C4 0B
RX 01 03 04 00 1A 00 33 9B E1
Measurement temperature=26 celsius, humidity=51 percent
device_id=pc-modbus-sim-01; sequence=1/2; 同一timestamp_ms
两次PUBLISH_CONFIRMED；独立订阅端完整JSON逐字段一致
RS485 closed
```

回复CRC通过项目已有frame_crc16_modbus生成，并由已有响应校验函数确认；没有重写CRC。

部署与运行：
- 复用现有Buildroot工具链和sysroot中的libmosquitto.so.1，不重建SDK或刷板。
- 独立MQTT开启构建目录：build-arm64-modbus-mqtt；原build-arm64-rs485仍保持MQTT关闭。
- 本次程序和库仅放在/tmp/edgevision-modbus-mqtt-TIdwdN，启动时用LD_LIBRARY_PATH指向该目录。
- 程序/库的本地与板端SHA256一致，ldd解析到已有OpenSSL库。
- SSH临时转发-R 127.0.0.1:1883:127.0.0.1:1883，让板端回环连接到WSL已有Broker。
- 独立订阅端SUBACK就绪后才启动板端；命令退出时转发关闭，COM9也已释放。

日志：[实板串口到MQTT记录](../hardware/rs485/2026-08-31-modbus-live-mqtt.log)。

### 独立保留的异常观察

PC在正常模拟回复之后，再次收到了额外字节01 83 03 01 31。
同样的字节曾出现在前一轮串口验证中；本次已完整保留，不声明来源已查明、
不声明总线上只有一个应答参与者，也不以整链数据正确替代总线/电气验收。
已停止收发；后续接真实设备或继续现场通信前应确认额外帧来源。
本次不继续扩大排查或追加软件功能。

### 学习停止点

本轮成功路径与之前独立的不应答超时、映射越界拒绝证据共同构成A块最小学习结果。
接收预算不覆盖write/tcdrain；序号仍是单次演示序号，两条MQTT发布不具有原子性。
现有状态不能抵抗进程重启或断网丢失，这属于下一学习块B的SQLite/Outbox主题。
不以进入下一块为理由声称真实传感器验收通过，也不在本轮提前实现B。
