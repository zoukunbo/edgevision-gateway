# OK1126B-S UART5/RS485 与 NFS 开发指南

本文记录 OK1126B-S 上 UART5、GPIO 方向控制、半双工 RS485 收发、
Buildroot 交叉编译以及 ADB、SCP、NFS 部署流程，并给出后续工程化路线。

初次实板验证：2026-08-29；任务范围核对：2026-08-30。

> 本文是使用手册，不是 D36 当天待办。当前学习停止点与范围以D36收尾文档为准。D36 核对结论见
> [D36 收尾核对](d36-closeout.md)，接线、参数和证据见
> [hardware/rs485](../hardware/rs485/README.md)。电脑串口助手收发成功
> 不等于真实 Modbus 从站的请求/响应验收通过。

## 1. 当前成果

已经完成并在实板验证：

- `/dev/ttyS5` 配置为 115200、8N1、无软硬件流控、原始模式；
- GPIO0_C6 通过 `/dev/gpiochip0` 的 line offset 22 控制 RSE；
- RSE 高电平发送、低电平接收；
- 发送路径为 `TX -> write_full -> tcdrain -> RX`；
- 使用 `poll()` 等待串口可读，并将数据追加到接收缓冲区；
- 开发板向电脑发送 `11 22 33 44 55` 成功；
- 电脑向开发板发送 `01 03 00 00 00 02 C4 0B` 成功；
- 使用 Buildroot AArch64 工具链生成目标板程序；
- ADB、SCP、NFS 三种部署路径均已走通；
- NFSv4 只读挂载目录为 `/mnt/edgevision`。

当前程序是单次收发验证程序，还不是正式的连续采集服务。

## 2. 硬件连接

### 2.1 P16 与 SP3485

| OK1126B-S P16 | SoC/功能 | SP3485 | 说明 |
|---|---|---|---|
| Pin 8 | UART5_TX | DI | 开发板发送数据 |
| Pin 10 | UART5_RX | RO | 开发板接收数据 |
| Pin 7 | GPIO0_C6 | DE 与 `/RE` 并联后的 RSE | 高发送，低接收 |
| 3.3V 引脚 | 3.3V | VCC | 不要误接 5V |
| GND 引脚 | GND | GND | 两端必须有共同参考地 |

SP3485 的 A、B 分别连接 USB-RS485 转换器的 A+、B-。如果没有数据，
先核对两端丝印定义，不要仅凭线色判断；某些厂商会使用相反的 A/B 命名。

### 2.2 Linux 映射

```text
UART5                 /dev/ttyS5
GPIO 控制器           /dev/gpiochip0
GPIO0_C6              line offset 22
P16 物理引脚          Pin 7
串口参数              115200 8N1
RSE=1                 发送
RSE=0                 接收
```

不要把 P16 物理引脚号、SoC GPIO 名和 Linux line offset 混为一谈。
本板 GPIO0_C6 的计算为 `0 * 32 + C * 8 + 6 = 22`。

## 3. 代码结构

```text
modules/serial/
├── rse_control.h       GPIO 方向控制接口
├── rse_control.c       GPIO character device v2 ioctl 实现
├── rs485_serial.h      TTY、发送、等待、接收接口
└── rs485_serial.c      termios、write、tcdrain、poll、read

examples/serial/
└── rs485_demo.c        实板单次双向收发验证程序
```

### 3.1 初始化生命周期

程序启动时只初始化一次：

```text
rs485_port_t port = RS485_PORT_INITIALIZER
        |
rs485_port_open(&port, "/dev/ttyS5", "/dev/gpiochip0", 22)
        | 内部完成 open + termios配置 + GPIO申请
进入收发流程
        |
rs485_port_close(&port)
```

不能每发送一帧就重新打开串口或 GPIO。初始化函数成功后，资源所有权交给
`rs485_port_t`；退出时先停止收发，再由 `rs485_port_close()` 尽力恢复RX并统一释放。
重复open返回EBUSY而不覆盖原FD；失败回滚保留errno；close后FD均为-1，可再次open。
对象打开后不得按值复制，也不得由外部单独关闭其中的FD。

### 3.2 完整写入

`write()` 可能只写入部分数据，也可能被信号中断。`serial_write_full()`
持续使用：

```c
data + written
length - written
```

直到全部字节进入内核串口发送队列。`EINTR` 需要重试；返回 0 字节但仍有
剩余数据时按 `EIO` 处理，不能误报成功。

### 3.3 半双工发送

正确顺序：

```text
RSE=TX
  -> serial_write_full()
  -> tcdrain()
  -> RSE=RX
```

`write_full()` 只保证数据交给内核队列；`tcdrain()` 等待数据真正发送完成。
如果过早拉低 RSE，最后几个字节可能被截断。任何错误路径都应尽力恢复 RX，
同时保存最初的 `errno`，防止清理操作覆盖根因。

### 3.4 等待与追加读取

当前只有一个串口 fd，因此使用 `poll()` 足够。`epoll` 更适合统一管理大量
网络连接、串口、`timerfd` 和退出事件时使用。

`serial_wait_readable()` 返回：

```text
1    POLLIN，可调用 read
0    超时
-1   poll 调用失败或错误事件
```

`serial_read_append()` 使用：

```c
buffer + *used
capacity - *used
```

读取成功后增加 `*used`。它只读取当前可用数据（VMIN=0、VTIME=0），不承诺读满一帧。
0 字节表示本次没有数据，示例不会再将它算作收帧成功。
`poll()` 被信号中断时当前接口返回 -1/EINTR；带总截止时间的重试属于 D37。

### 3.5 D37 增量：带总超时的定长接收

新增 `serial_read_exact_timeout(fd, buffer, length, timeout_ms, &received)`：
返回1表示收齐指定长度，0表示总超时，-1表示错误；部分字节和数量均保留。
调用前须配置 raw、VMIN=0/VTIME=0（或非阻塞FD），串口只允许一个读者。
截止时间基于CLOCK_MONOTONIC且只计算一次；poll/read被EINTR打断后按剩余预算重试。
函数不清空旧输入，不读取超过length的字节，也不识别CRC或Modbus边界。

`rs485_demo` 已接入新接口，用户已提供实板8字节接收成功截图。
定长收齐不等同于协议帧有效；超时只收3字节的实板结果尚未单独提供截图。
资源管理也已迁入模块，demo只保留设备配置、收发演示与输出。

### 3.6 D37 增量：可停止接收

`serial_read_exact_timeout_stop(..., should_stop, stop_context)` 通过回调检查停止。
回调在接收线程中执行，context仅在调用期间借用；跨线程使用时由调用者正确同步。
原接口保留，通过NULL回调调用新接口，原有接收逻辑不变。

- 返回1=收齐，0=总超时，-1=错误；主动取消使用errno=ECANCELED。
- 取消时保留已收字节与数量，不清空缓冲区，不关闭串口。
- 有回调时每次poll最多等待100ms，使用同一个CLOCK_MONOTONIC总截止时间。
  100ms是停止检查间隔，不是帧间隔，也不是新的总超时；不是硬实时保证。
- demo复用graceful_shutdown模块。SIGINT/SIGTERM处理函数只设置标志；
  接收返回后，main区分取消与错误，再调用rs485_port_close统一清理。
- 当前仅支持取消接收；阻塞发送/write/tcdrain尚不具备取消或总超时。

实板复现步骤（已远程验证SIGINT/SIGTERM取消与重开；PC输入测试仍需串口助手配合）：

在WSL执行：

```bash
cd /home/zoukunbo/project/edgevision-gateway
cmake --build build-arm64-rs485 --target rs485_demo -j2
scp build-arm64-rs485/rs485_demo root@192.168.0.232:/tmp/rs485_demo
ssh root@192.168.0.232
```

进入板端后运行：

```bash
chmod +x /tmp/rs485_demo
/tmp/rs485_demo
```

看到等待提示后按Ctrl+C，预期打印 `RX canceled: received 0/8 bytes` 和
`RS485 closed` 后返回命令行。若先发3字节，取消时应保留为3/8。
再次运行并发送完整8字节，预期 `RX complete` 后正常关闭。
在交互SSH会话内测试Ctrl+C；不要把没有远端PTY的SSH调用当作同等信号测试。
NFS已挂载时可改为运行 `/mnt/edgevision/rs485_demo`；重新编译前先结束旧进程。

### 3.7 共享截止时间（已完成，供后续事务使用）

`serial_deadline_after_ms`生成CLOCK_MONOTONIC纳秒截止时间；
`serial_read_exact_until_stop`接受这个固定时刻。前3字节与剩余部分可以共用同一个值。
旧timeout接口仅负责生成截止时间后调用新接口，接收循环只有一份。
每次调用的received单独清零和累计，第二段传buffer+第一段长度。
主机共享预算测试通过；这不代表Modbus实际串口事务已完成。

## 4. CMake 与交叉编译

### 4.1 CMakeLists 与工具链的分工

```text
CMakeLists.txt          决定编译什么、目标如何链接
toolchainfile.cmake     决定为谁编译、使用什么编译器和 sysroot
```

项目目标：

```cmake
add_library(edgevision_serial STATIC
    modules/serial/rse_control.c
    modules/serial/rs485_serial.c
)

add_executable(rs485_demo
    examples/serial/rs485_demo.c
)

target_link_libraries(rs485_demo PRIVATE
    edgevision_source  # PUBLIC依赖edgevision_serial
    edgevision_runtime
)
```

### 4.2 首次配置

```bash
cd /home/zoukunbo/project/edgevision-gateway

cmake -S . -B build-arm64-rs485 \
  -DCMAKE_TOOLCHAIN_FILE=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/share/buildroot/toolchainfile.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DEDGEVISION_ENABLE_MQTT=OFF
```

工具链文件设置 AArch64 编译器、`CMAKE_SYSROOT`、目标头文件和目标库搜索
范围。必须使用独立构建目录，不能把主机 `build/` 与目标板构建缓存混用。

### 4.3 日常增量编译

```bash
cmake --build build-arm64-rs485 \
  --target rs485_demo -j2
```

确认架构：

```bash
file build-arm64-rs485/rs485_demo
```

预期包含 `ARM aarch64`，不能是 `x86-64`。

## 5. 三种部署方式

### 5.1 ADB：快速临时验证

```bash
adb connect 192.168.0.232:5555
adb push build-arm64-rs485/rs485_demo /tmp/rs485_demo
adb shell chmod +x /tmp/rs485_demo
adb shell /tmp/rs485_demo
```

适合设备调试、无 SSH 环境和快速救援。

### 5.2 SCP：练习标准 Linux 远程部署

```bash
scp build-arm64-rs485/rs485_demo \
  root@192.168.0.232:/tmp/rs485_demo

ssh root@192.168.0.232
chmod +x /tmp/rs485_demo
/tmp/rs485_demo
```

远端路径规则：

```text
:/tmp/rs485        远端文件名变成 rs485
:/tmp/rs485_demo   远端文件名变成 rs485_demo
:/tmp/             保留源文件名 rs485_demo
```

复制后运行错误版本时，可用 `strings`、`file`、`sha256sum` 和时间戳确认
板端文件是否为最新产物。

### 5.3 NFS：高频开发

日常流程缩短为：

```text
WSL 修改并编译 -> 开发板直接运行共享目录中的新程序
```

#### WSL 服务端

安装：

```bash
sudo apt update
sudo apt install -y nfs-kernel-server
```

`/etc/exports`：

```text
/home/zoukunbo/project/edgevision-gateway/build-arm64-rs485 192.168.0.232(ro,sync,no_subtree_check,insecure,fsid=0)
```

加载并检查：

```bash
sudo exportfs -rav
sudo systemctl restart nfs-kernel-server
sudo exportfs -v
sudo ss -lntp | grep ':2049'
```

当前 WSL 使用 mirrored networking，局域网地址为 `192.168.0.100`。
Windows 管理员 PowerShell 已添加仅允许开发板访问 TCP 2049 的规则：

```powershell
New-NetFirewallHyperVRule `
  -Name "WSL-NFSv4-2049" `
  -DisplayName "WSL NFSv4 for OK1126B" `
  -Direction Inbound `
  -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}' `
  -Protocol TCP `
  -LocalPorts 2049 `
  -RemoteAddresses 192.168.0.232
```

#### Buildroot 客户端

在 `menuconfig` 中启用：

```text
Package Selection for the target
  -> Filesystem and flash utilities
    -> nfs-utils
      [*] NFSv4/NFSv4.1
```

客户端不需要 `rpc.nfsd`。验证配置：

```bash
grep -E '^BR2_PACKAGE_NFS_UTILS' \
  output/rockchip_ok1126b-s/.config
```

Buildroot 不接受包含空格的 WSL/Windows 混合 PATH。使用临时干净 PATH：

```bash
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make O=output/rockchip_ok1126b-s nfs-utils -j2
```

当前为避免立即重刷镜像，将 AArch64 `mount.nfs` 临时复制到板端：

```bash
scp output/rockchip_ok1126b-s/target/usr/sbin/mount.nfs \
  root@192.168.0.232:/tmp/mount.nfs
```

开发板挂载：

```bash
chmod +x /tmp/mount.nfs
mkdir -p /mnt/edgevision
/tmp/mount.nfs 192.168.0.100:/ /mnt/edgevision \
  -o vers=4,proto=tcp,ro
```

检查和运行：

```bash
mount | grep edgevision
ls -lh /mnt/edgevision
/mnt/edgevision/rs485_demo
```

解除挂载：

```bash
umount /mnt/edgevision
```

当前挂载和 `/tmp/mount.nfs` 在重启后都会消失。

## 6. 已遇到的问题与定位方法

| 现象 | 根因 | 处理 |
|---|---|---|
| 串口收到连续 `00` | 重启后未配置 termios | 程序初始化时调用串口配置函数 |
| 点击两次才看到数据显示 | `hexdump -C` 默认等待16字节，而一帧只有8字节 | 使用 `hexdump -C` 时理解其显示缓冲行为，或由程序按实际读取长度打印 |
| 发送尾部可能缺失 | `write()` 后立即切回 RX | `tcdrain()` 完成后再拉低 RSE |
| GPIO 申请返回 `EBUSY` | GPIO22 仍被 sysfs 或其他进程占用 | 释放旧使用者并检查 `/sys/kernel/debug/gpio` |
| 程序运行后没有 `Waiting` | 新程序复制为 `/tmp/rs485`，却运行旧 `/tmp/rs485_demo` | 明确 SCP 目标名，并用 `strings` 检查版本 |
| Shell 出现 `>` | 反斜杠或引号导致命令尚未结束 | `Ctrl+C` 后改用单行命令 |
| `mount` 要求 `/sbin/mount.nfs4` | 板端 Buildroot 未启用 `nfs-utils` | 启用 `nfs-utils` 和 NFSv4，或临时复制 `mount.nfs` |
| NFSv4 挂载超时 | WSL Hyper-V 防火墙拦截 TCP 2049 | 添加限定开发板IP的入站规则 |
| Buildroot 报 PATH 含空格 | WSL 导入了 Windows PATH | 对该次 make 使用干净 PATH |
| nfs-utils 安装时 chown 报错 | 普通用户不能改变目标目录所有者 | 日志标记 `(ignored)` 且完成标记存在时不影响目标安装；最终镜像由 fakeroot 处理元数据 |

## 7. 当前实现的边界

当前演示程序存在以下限制：

- 只进行一次发送和一次接收；
- demo在总超时内累计收齐8字节，但这不保证它们是一帧有效协议数据；
- 没有基于长度、CRC 或帧间空闲判断一帧是否完整；
- 协议层已实现03请求编码和完整响应校验，但尚未接入demo的实际串口事务；
- 没有设备断线、串口错误和超时重试状态机；
- 已有PTY接收测试和资源生命周期GPIO替身测试；尚未覆盖真实GPIO电平与完整发送时序；
- 尚未接入 Gateway 的 Measurement 数据链路；
- `nfs-utils` 尚未正式进入可刷写根文件系统和持久配置。

## 8. 按学习日划分的后续范围

以当前 Notion V4 页面为依据，不能把项目级功能全部算作 D36 欠账。

| 范围 | 当前状态 / 后续动作 |
|---|---|
| D36 硬件与原始数据基线 | 工具双向收发已验证；补齐真实从站参数、电气记录和20组请求/响应证据后才能按原清单完整验收 |
| D36 RealSerialSource 占位 | 已提供 `real_serial_source_placeholder()`，返回 NO_DATA，不打开设备、不伪造 Measurement、不替换模拟主链 |
| D37 串口模块接入 Gateway | 基础收发、定长总超时、可停止接收及RealSerialSource原始数据适配已完成；默认Gateway仍使用模拟Measurement |
| D38 Modbus 顺序事务与 Measurement 映射 | 请求/响应匹配、CRC、异常码、寄存器到 Measurement 映射，不能提前算成 D36 缺口 |
| 后续工程完善 / 选做 | 扩大测试矩阵、统计/退避、进程托管、NFS镜像固化、发布包等按实际依赖再安排 |

不再把“可靠帧接收器”列为 D36 收尾的默认下一项。
当前一读不等于一帧是明确的阶段边界，不是要求今天重建完整协议层。

## 9. 每次开发的最短流程

1. WSL 修改代码并编译 `rs485_demo`。
2. 使用 SCP 部署；已挂载 NFS 时可直接运行共享产物。
3. 只连接已验证的串口助手测试链路运行当前 demo；它发送的
   `11 22 33 44 55` 不是有效 Modbus 查询，不用于真实仪表验收。
4. 看到等待提示后，电脑发送测试字节；无输入时约10秒超时退出。
5. 修改软件后运行必要回归，保存测试来源，不将 PTY 当成实板证据。

开发板重启后先确认程序和挂载是否仍存在；NFS 并非程序运行的必要条件。
正式从站请求必须先取得设备手册、站号和可安全读取的寄存器参数。

## 10. 已学内容与下次入口（不是D36追加任务）

已完成：串口参数配置、完整写入/方向控制、定长总超时接收、资源对象与统一open/close、
demo接入、可停止接收与信号退出、主机基础回归。不要重复布置这些内容。

1. **当前阶段已完成**：RealSerialSource持有设备资源、配置值和原始接收缓冲区；
   demo通过open/send/read/close调用它。主机网络smoke与板端取消/重开均已通过。
2. **下一阶段**：已有Modbus RTU请求编码、CRC及响应校验不再重学；下一轮锁定为一次03顺序事务，用PC模拟从站验证；
   有有效Measurement后再接入Gateway真实数据链路，不能把原始8字节直接当测量值。
   原Notion的真实设备多次事务仍缺硬件，不以模拟验证替代，也未修改Notion状态。

D38的请求编码、CRC和响应校验准备已完成；真实事务与Measurement映射尚未联通。本轮仅D36收尾，不继续编码。
D36的电气记录与真实从站证据仍单独保留，不通过软件重构将其标为完成。
STM32+DHT11节点、NFS持久化、扩大测试矩阵均不是本次新增任务。

### 本次资源重构验证

主机CTest 21/21通过，包含打开失败回滚、重复打开EBUSY、恢复RX失败仍清理、
重复关闭不误关复用FD、重新打开及FD基线检查。ARM64的rs485_demo和rs485_port_test编译通过。
生命周期测试使用GPIO替身和真实PTY，不声称完成新的真实GPIO实板验证。

### 本次可停止接收验证（2026-08-30）

主机CTest 22/22通过；新增serial_stop测试验证正常接收、预先取消不消耗输入、
没有信号打断时仍定期检查停止、部分数据保留、分片不改变总超时、SIGINT/SIGTERM，
以及取消后FD仍归调用者所有。生命周期测试补充取消后统一关闭的检查。
ARM64的rs485_demo、serial_stop_test、rs485_port_test编译通过。
此项为首次实现时的记录；随后已运行板端信号取消与重开测试，结果见下节。

### RealSerialSource适配与板端验证（2026-08-30）

新增接口位于 `modules/source/real_serial_source.h/.c`：

- `real_serial_source_open`：验证配置并打开TTY/GPIO，复制接收长度和总超时。
  路径字符串只在打开时借用，不保存；失败不遗留本次打开的资源。
- `real_serial_source_send`：复用半双工发送逻辑，不自行生成协议帧。
- `real_serial_source_read`：写入源对象持有的256字节缓冲区，以枚举区分
  COMPLETE、TIMEOUT、CANCELED、ERROR；部分数据通过buffer和received保留。
  每次调用开始新的定长接收，不跨调用自动拼接或重新发送。
- `real_serial_source_close`：由正常控制流释放资源，可重复关闭和重新打开。
- 对象必须用REAL_SERIAL_SOURCE_INITIALIZER初始化；禁止复制打开的对象或并发关闭。
- 原Measurement占位接口保持NO_DATA；默认Gateway仍使用模拟源。协议层后续已添加独立编码/校验函数，但未接入真实源；
  没有后台采集线程或虚构传感器温湿度。

验证记录：

- 主机CTest：23/23通过（包括Gateway/网络回归）。
- ARM64：新demo及real_serial_source_test编译通过。
- 板端PTY + GPIO替身：原始收发、部分超时、取消、配置、失败回滚、重开、NO_DATA契约通过。
- 板端真实/dev/ttyS5 + GPIO22：demo发送后进入等待，通过kill发送SIGINT和SIGTERM，
  均报告RX canceled和RS485 closed，第二次成功重新申请TTY/GPIO。
  信号与Ctrl+C在demo中使用同一SIGINT处理函数，但这不是人工键盘测试。
- 本次没有操控PC串口助手，未验证新的实线RX完整帧或GPIO释放后的物理电平。
- 板端临时文件位于/tmp/edgevision-stop-qvRMc2，保留以便复现，重启可能丢失；
  未覆盖用户的/tmp/rs485_demo。日志见
  [板端验证记录](../hardware/rs485/2026-08-30-source-stop.log)。
