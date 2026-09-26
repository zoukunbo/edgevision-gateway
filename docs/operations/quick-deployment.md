# ADB、SCP 与 NFS 快速部署指南

本文集中管理开发阶段把 ARM64 程序送到 OK1126B-S 并运行的方法。三种方式不是互相替代，而是分别适合临时调试、标准远程复制和高频迭代。

## 1. 先选择部署方式

| 方式 | 是否复制文件 | 板端重启后 | 依赖 | 适合场景 |
| --- | --- | --- | --- | --- |
| ADB | 复制到板端 | `/tmp` 内容可能丢失 | ADB 服务和 5555 端口 | 首次调试、无 SSH、快速救援 |
| SCP | 复制到板端 | 取决于目标目录 | SSH 服务 | 日常单文件验证、明确保存一个版本 |
| NFS | 不复制，远程挂载 | 需配置自动挂载才会恢复 | NFSv4、网络和开发主机在线 | 频繁编译、频繁替换程序 |

选择原则：

- 只想尽快把一个程序跑起来：ADB。
- 已有 SSH，需要明确把某个产物复制过去：SCP。
- 一天内会反复修改、编译、运行：NFS。
- 正式发布或现场离线运行：不要依赖开发主机 NFS，应使用 `deploy/edge-gateway-lite/` 的发布流程。

## 2. 部署前检查

先完成 ARM64 交叉构建，并确认产物架构：

```bash
cmake --build build-arm64-rs485 --target rs485_demo -j2
file build-arm64-rs485/rs485_demo
```

预期包含 `ARM aarch64`，不能是 `x86-64`。下面沿用历史开发环境作为示例：

```text
板端 IP       192.168.0.232
ADB 端口      5555
板端临时目录  /tmp
NFS 挂载点    /mnt/edgevision
```

IP 和构建目录变化时应替换示例值，不要把它们理解成程序协议的一部分。

## 3. ADB：最快的临时验证

```bash
adb connect 192.168.0.232:5555
adb devices
adb push build-arm64-rs485/rs485_demo /tmp/rs485_demo
adb shell chmod +x /tmp/rs485_demo
adb shell /tmp/rs485_demo
```

需要交互或连续执行多条命令时，先进入板端 Shell：

```bash
adb shell
cd /tmp
./rs485_demo
```

ADB 的优势是部署快，而且不依赖 SSH。缺点是 `/tmp` 通常不持久，设备重启后程序可能消失；它适合验证，不代表已经正式安装。

常用检查：

```bash
adb shell file /tmp/rs485_demo
adb shell ls -l /tmp/rs485_demo
adb shell sha256sum /tmp/rs485_demo
```

## 4. SCP：标准 Linux 单文件部署

复制并运行：

```bash
scp build-arm64-rs485/rs485_demo \
  root@192.168.0.232:/tmp/rs485_demo

ssh root@192.168.0.232
chmod +x /tmp/rs485_demo
/tmp/rs485_demo
```

SCP 目标路径容易混淆：

```text
:/tmp/rs485        远端文件名变成 rs485
:/tmp/rs485_demo   远端文件名变成 rs485_demo
:/tmp/             保留源文件名 rs485_demo
```

因此“已经复制但运行结果没变化”时，先确认复制和执行的是不是同一个路径：

```bash
sha256sum build-arm64-rs485/rs485_demo
ssh root@192.168.0.232 \
  'file /tmp/rs485_demo; ls -l /tmp/rs485_demo; sha256sum /tmp/rs485_demo'
```

本机与板端 SHA256 应相同。也可以用 `strings` 检查新版本中特有的提示文本，但哈希更可靠。

## 5. NFS：高频开发部署

NFS 的核心流程是：

```text
开发主机修改并交叉编译
        ↓
产物写入 NFS 导出目录
        ↓
板端直接运行 /mnt/edgevision 中的新产物
```

完整的服务安装、`/etc/exports`、Buildroot `nfs-utils`、挂载和防火墙配置统一见 [NFS 开发环境运维指南](nfs-development.md)。这里仅保留日常使用流程。

主机更新导出目录中的产物后，在板端执行：

```sh
mount | grep /mnt/edgevision
ls -lh /mnt/edgevision
file /mnt/edgevision/rs485_demo
sha256sum /mnt/edgevision/rs485_demo
/mnt/edgevision/rs485_demo
```

如果 NFS 尚未自动挂载：

```sh
mkdir -p /mnt/edgevision
mount.nfs 192.168.0.100:/ /mnt/edgevision \
  -o vers=4,proto=tcp,ro
```

NFS 是挂载，不是复制。开发主机关机、IP 改变或 NFS 服务停止时，板端无法读取这些程序。数据库和运行状态应保存在 `/userdata/edgevision-gateway`，不能放在只读 NFS 中。

## 6. 每次开发的最短流程

1. 修改代码并执行 ARM64 增量构建。
2. 用 `file` 确认产物架构。
3. 按本次场景选择 ADB、SCP 或 NFS。
4. 用路径、时间戳或 SHA256 确认板端看到的是新产物。
5. 运行程序并保存必要输出；不要把一次成功等同于完整验收。
6. 结束仍占用串口、GPIO、数据库或旧可执行文件的进程，再进行下一轮构建。

## 7. 常见问题

| 现象 | 常见原因 | 处理 |
| --- | --- | --- |
| `Exec format error` | 把 x86-64 主机程序传到 ARM64 板端 | 用正确工具链重编译并检查 `file` |
| 程序提示没有变化 | 复制到新文件名，却执行了旧路径 | 核对完整路径和 SHA256 |
| `Permission denied` | 文件没有执行权限，或挂载禁止执行 | `chmod +x`，并检查挂载参数 |
| ADB 连接失败 | ADB 服务未启用、IP/端口或防火墙错误 | 检查 `adb devices` 和 5555 端口 |
| SCP 失败 | SSH 服务、密钥、密码或目标目录权限问题 | 先用 `ssh root@板端IP` 验证登录 |
| NFS 挂载超时 | 主机 IP、2049 防火墙、导出规则或服务异常 | 按 NFS 专题的故障顺序检查 |
| 重启后程序消失 | 程序只放在 `/tmp` | 改用持久目录或正式发布流程 |
| NFS 上程序存在但无数据 | NFS 只提供文件，不负责采集和启动服务 | 检查进程、systemd 和 `/userdata` |

## 8. 与正式部署的边界

ADB、SCP 到 `/tmp` 和手动 NFS 都是开发期快速部署。需要开机启动、失败重启、配置持久化和可恢复升级时，继续阅读：

- [systemd 板端部署运维指南](systemd-board-deployment.md)
- [`deploy/edge-gateway-lite/` 发布说明](../../deploy/edge-gateway-lite/README.md)

历史上实际执行过的 Outbox NFS/oneshot 部署过程保存在 [2026-09-01 部署记录](../records/systemd-nfs-board-deployment-2026-09-01.md)。
