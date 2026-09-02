# EdgeVision systemd、NFS 与板端持久化部署教程｜2026-09-01

> 适合第一次接触 NFS 和 systemd 的读者。本文不仅给出命令，还解释每一步解决什么问题、命令改变了什么、怎样判断成功。
>
> 本次实际部署已经完成并经过板卡重启验证。本文记录的是已经执行过的方案，不要求为了学习再次发布 MQTT 或重新操作 STM32。

## 1. 先弄清楚：这次到底部署了什么

这次部署的是 SQLite/Outbox 的两个 ARM64 演示程序和配套运行环境：

- `sqlite_outbox_delivery_demo`：把一条历史 Measurement 写入 SQLite，并产生一条 pending Outbox。
- `sqlite_outbox_mqtt_demo`：读取 pending Outbox，通过 MQTT QoS 1 发布；收到 PUBACK 后把该记录改成 sent。
- `run-outbox-once.sh`：供 systemd 调用，执行一次“检查并投递 pending”。
- `seed-historical-replay.sh`：把已经保存的 26.1°C 历史 JSON 写入板端数据库。
- `libmosquitto.so.1`：MQTT 程序在板端运行时需要的动态库。

它们没有被复制到板端的 `/usr/bin`。程序文件保存在 WSL，通过 NFS 挂载后出现在板端：

```text
/mnt/edgevision/edgevision-outbox/
```

板端真正持久保存的是：

```text
/usr/sbin/mount.nfs
/usr/sbin/mount.nfs4
/etc/systemd/system/mnt-edgevision.mount
/etc/systemd/system/edgevision-outbox.service
/userdata/edgevision-gateway/gateway.db
```

因此这是“程序放在开发主机，状态放在板端”的部署：

```text
WSL：程序、动态库、脚本、历史 JSON
       │
       │ NFSv4，只读
       ▼
板端：/mnt/edgevision/edgevision-outbox
       │
       │ 程序运行时读这里
       ▼
板端：/userdata/edgevision-gateway/gateway.db
       持久保存 Measurement 和 Outbox 状态
```

这样做适合当前学习和开发阶段：替换程序时只改 WSL 的部署包，板端数据库仍保留。它不是把完整默认 Gateway 部署成常驻服务；默认 Gateway 仍使用 `simulated_source`。

## 2. NFS 服务器、客户端和挂载

NFS 服务器在 WSL。它把这个真实目录共享给网络中的板卡：

```text
/home/zoukunbo/project/edgevision-gateway/deploy/nfs-root
```

OK1126B-S 是 NFS 客户端。它把 WSL 的共享目录挂到本地：

```text
/mnt/edgevision
```

挂载不是复制。挂载成功后，板端访问 `/mnt/edgevision/edgevision-outbox/bin/...`，实际读取的是 WSL 文件。因此：

- WSL 在线且 NFS 正常时，板端可以读取程序。
- WSL 关机或 IP 改变时，挂载会失败，程序无法从 NFS 启动。
- 板端 `/userdata` 中的数据库不依赖 NFS，重启后仍存在。

## 3. “板端不支持 NFS”究竟缺了什么

NFS 客户端由两部分组成：

1. **Linux 内核的 NFS 文件系统驱动**：真正处理 NFS 协议和文件访问。
2. **用户空间的 `mount.nfs` 工具**：解析挂载参数，再请求内核完成挂载。

可以把内核驱动理解成“发动机”，把 `mount.nfs` 理解成“启动和控制发动机的工具”。

本次检查发现：板端内核已经支持 NFS/NFSv4，但根文件系统没有 `/usr/sbin/mount.nfs`。所以准确说法是“缺少挂载助手”，不是“内核完全不支持 NFS”。

### 3.1 检查内核支持

在板端执行：

```sh
cat /proc/filesystems | grep nfs
```

本次能看到 nfs/nfs4，说明内核能力存在。也可以尝试：

```sh
zcat /proc/config.gz | grep -E 'CONFIG_NFS_FS|CONFIG_NFS_V4'
```

如果配置可读，预期包含 `CONFIG_NFS_FS=y`、`CONFIG_NFS_V4=y`。如果 `/proc/config.gz` 不存在，仍以 `/proc/filesystems` 和实际挂载结果为准。

检查用户空间工具：

```sh
command -v mount.nfs
ls -l /usr/sbin/mount.nfs /usr/sbin/mount.nfs4
```

本次板端原先找不到它们。

如果 `/proc/filesystems` 没有 nfs/nfs4，复制 `mount.nfs` 也没用；此时必须在 Buildroot/内核中打开 NFS 客户端配置，重新构建和更新内核。本次不用重建内核。

## 4. mount.nfs 从哪里来，为什么能运行

与板端匹配的 Buildroot target 中已有 ARM64 版本：

```text
/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/target/usr/sbin/mount.nfs
```

它来自同一套 Buildroot 目标环境。不能从普通 Ubuntu 随便复制，因为 PC Ubuntu 的程序通常是 x86_64，放到 ARM64 板端会出现 `Exec format error`。

在 WSL 可检查：

```sh
file /home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/target/usr/sbin/mount.nfs
```

应看到 AArch64。我们把它持久安装为 `/usr/sbin/mount.nfs`，并创建：

```text
/usr/sbin/mount.nfs4 -> mount.nfs
```

符号链接让系统按 `nfs4` 类型挂载时也能找到同一助手。

## 5. 为什么第一次必须用 SCP

最初板端没有 `mount.nfs`，因此不能挂 NFS；可 `mount.nfs` 又位于准备通过 NFS 提供的部署包中。这是一个引导循环：

```text
想从 NFS 读取 mount.nfs
        ↓
挂 NFS 又需要 mount.nfs
        ↓
无法开始
```

解决顺序：

1. WSL 先通过已有 SSH/SCP，把 `mount.nfs` 直接复制到板端 `/tmp`。
2. 板端把它安装到持久位置 `/usr/sbin/mount.nfs`。
3. 板端现在具备 NFS 挂载能力。
4. systemd 再挂载 WSL 的 NFS。
5. 以后程序和脚本从 NFS 读取，不必用 SCP 复制整个部署包。

SCP 只用于第一次引导；它走 SSH，与 NFS 是两条不同通路。

## 6. 完整拓扑

```text
WSL 192.168.0.100
├── deploy/nfs-root ──NFSv4只读──► 板端 /mnt/edgevision
├── Mosquitto 127.0.0.1:1883
└── SSH反向隧道 ────────────────► 板端 127.0.0.1:1883
                                      │
                                      ▼
                         edgevision-outbox.service
                                      │
                         读取NFS程序，读写本地数据库
                                      │
                                      ▼
                    /userdata/edgevision-gateway/gateway.db
```

两条网络通路的职责不同：

- NFS：WSL 向板端提供程序文件。
- SSH 反向隧道：让板端程序通过自己的 `127.0.0.1:1883` 访问 WSL Broker。

## 7. ARM64 部署包是怎样准备的

### 7.1 交叉编译

WSL PC 与板端架构不同，必须使用板卡 Buildroot 交叉工具链。初学复现建议新建构建目录，不直接删除旧目录：

```sh
cd /home/zoukunbo/project/edgevision-gateway

cmake -S examples/storage -B build-arm64-storage-reproduce \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/home/zoukunbo/work/OK1126B-linux-source/buildroot/output/rockchip_ok1126b-s/host/share/buildroot/toolchainfile.cmake

cmake --build build-arm64-storage-reproduce \
  --target sqlite_outbox_delivery_demo sqlite_outbox_mqtt_demo -j
```

参数解释：

| 参数 | 含义 |
| --- | --- |
| `-S examples/storage` | CMake 源码入口 |
| `-B build-arm64-storage-reproduce` | 新的构建和产物目录 |
| `CMAKE_BUILD_TYPE=Release` | 发布优化配置 |
| `CMAKE_TOOLCHAIN_FILE=...` | 使用 ARM64 编译器和 sysroot |
| `--target ...` | 只构建两个目标 |
| `-j` | 并行编译 |

检查：

```sh
file build-arm64-storage-reproduce/sqlite_outbox_delivery_demo
file build-arm64-storage-reproduce/sqlite_outbox_mqtt_demo
```

必须看到 AArch64；看到 x86-64 表示工具链没生效。

### 7.2 包目录

```text
deploy/nfs-root/edgevision-outbox/
├── bin/       两个ARM64程序
├── lib/       libmosquitto.so.1
├── scripts/   投递和播种脚本
├── share/     temperature-replay.json
├── systemd/   mount、service、隧道unit
├── tools/     mount.nfs
└── SHA256SUMS 文件完整性清单
```

板端已有 SQLite 库，但缺少 Mosquitto 动态库，所以包中携带 `libmosquitto.so.1`。运行程序时使用：

```sh
LD_LIBRARY_PATH=/mnt/edgevision/edgevision-outbox/lib
```

它只为当前命令增加动态库搜索目录，没有修改板端全局 `/usr/lib`。忘记它会出现 `libmosquitto.so.1` 找不到的错误。

`SHA256SUMS` 用于确认板端通过 NFS 看到的文件与制作部署包时一致。

## 8. WSL 如何成为 NFS 服务器

先检查：

```sh
systemctl status nfs-kernel-server
```

如果提示找不到服务，才需要：

```sh
sudo apt update
sudo apt install nfs-kernel-server
```

本次环境原先已安装 NFS 服务。

实际写入 `/etc/exports` 的配置：

```text
/home/zoukunbo/project/edgevision-gateway/deploy/nfs-root 192.168.0.232(ro,sync,no_subtree_check,insecure,fsid=0)
```

| 配置 | 含义 |
| --- | --- |
| 共享路径 | WSL 上的真实部署目录 |
| `192.168.0.232` | 只允许该板卡 |
| `ro` | 板端只读，不能经 NFS 修改部署包 |
| `sync` | 同步处理；本次为只读导出 |
| `no_subtree_check` | 不做子目录路径检查 |
| `insecure` | 允许嵌入式客户端使用高位源端口 |
| `fsid=0` | 把该目录作为 NFSv4 导出根 |

所以板端挂载 `192.168.0.100:/`。这里的 `/` 是 NFSv4 导出根，不是 WSL 整个根目录。


## 9. install-wsl-host.sh 是如何实现的

脚本路径：

```text
deploy/edgevision-outbox/install-wsl-host.sh
```

### 9.1 为什么要求 sudo

```sh
if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi
```

修改 `/etc/exports` 和安装 systemd unit 需要 root 权限。权限不足就立即退出，避免只完成一半。

### 9.2 定义固定环境

```sh
repo=/home/zoukunbo/project/edgevision-gateway
export_root="$repo/deploy/nfs-root"
board_ip=192.168.0.232
export_line="$export_root $board_ip(ro,sync,no_subtree_check,insecure,fsid=0)"
```

这些变量组成 NFS 导出配置。工程路径或 IP 变化时，必须同步修改脚本和 unit。

### 9.3 安全更新 /etc/exports

```sh
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
awk -v root="$export_root" 'NF == 0 || $1 != root { print }' /etc/exports > "$tmp"
printf '%s\n' "$export_line" >> "$tmp"
install -m 0644 "$tmp" /etc/exports
```

它没有每次都盲目追加：

1. `mktemp` 创建临时文件。
2. `awk` 保留原配置，但删除该部署目录的旧行。
3. `printf` 追加当前正确配置。
4. `install -m 0644` 写回 `/etc/exports`。
5. `trap` 在脚本退出时删除临时文件。

因此重复执行不会为同一目录堆积多行。

### 9.4 让 NFS 配置生效

```sh
exportfs -rav
systemctl restart nfs-kernel-server
```

- `exportfs -r`：重新读取 `/etc/exports`。
- `-a`：处理全部导出。
- `-v`：显示详细结果。
- 重启服务保证服务器按当前配置运行。

### 9.5 安装 MQTT 隧道服务

```sh
install -m 0644 .../edgevision-board-mqtt-tunnel.service \
    /etc/systemd/system/edgevision-board-mqtt-tunnel.service
systemctl daemon-reload
systemctl enable --now edgevision-board-mqtt-tunnel.service
```

四个概念要分开：

- **install**：复制 unit 文件。
- **daemon-reload**：让 systemd 重新读取磁盘配置。
- **enable**：创建开机启动链接。
- **--now**：本次也立即启动。

运行整个脚本：

```sh
cd /home/zoukunbo/project/edgevision-gateway
sudo deploy/edgevision-outbox/install-wsl-host.sh
```

检查：

```sh
exportfs -v
systemctl is-enabled nfs-kernel-server
systemctl is-active nfs-kernel-server
systemctl is-enabled edgevision-board-mqtt-tunnel.service
systemctl is-active edgevision-board-mqtt-tunnel.service
```

`enabled` 回答“以后开机是否自动启动”，`active` 回答“现在是否运行”。

## 10. MQTT 反向隧道如何工作

MQTT 程序固定连接 `127.0.0.1:1883`。在板端，`127.0.0.1` 指板端自己；但 Broker 在 WSL 的 `127.0.0.1:1883`，且只监听回环地址。

WSL unit 执行：

```sh
ssh -NT ... -R 127.0.0.1:1883:127.0.0.1:1883 root@192.168.0.232
```

方向是：

```text
WSL主动建立SSH连接到板端
板端 127.0.0.1:1883
          │
          │ SSH反向转发
          ▼
WSL 127.0.0.1:1883（现有Broker）
```

`-R` 表示在 SSH 远端，也就是板端，创建监听端口。参数说明：

| 参数 | 作用 |
| --- | --- |
| `-N` | 不执行远程命令，只转发 |
| `-T` | 不分配交互终端 |
| `BatchMode=yes` | 不等待密码，要求密钥登录 |
| `ExitOnForwardFailure=yes` | 建立端口失败就退出 |
| `ServerAliveInterval=15` | 每15秒检查连接 |
| `ServerAliveCountMax=3` | 连续3次无响应后断开 |
| `Restart=always` | 进程退出后由systemd重启 |
| `RestartSec=5` | 5秒后重试 |

它没有修改或重启 Mosquitto 配置。

## 11. install-board-from-wsl.sh 如何解决首次安装

该脚本在 WSL 执行，内部通过 `ssh` 和 `scp` 操作板端。

### 11.1 定义来源和目标

```sh
repo=/home/zoukunbo/project/edgevision-gateway
bundle="$repo/deploy/nfs-root/edgevision-outbox"
board=root@192.168.0.232
stage=/tmp/edgevision-board-install
```

`bundle` 是 WSL 部署包，`board` 是 SSH 目标，`stage` 是板端临时中转目录。

### 11.2 检查包并准备临时目录

```sh
test -f "$bundle/SHA256SUMS"
ssh "$board" "rm -rf '$stage' && mkdir -p '$stage'"
```

`test -f` 确认清单存在，否则停止。脚本只清理固定的 `/tmp/edgevision-board-install`，不碰 `/userdata` 或源码。

### 11.3 用 SCP 传入三个引导文件

```sh
scp "$bundle/tools/mount.nfs" "$board:$stage/mount.nfs"
scp "$bundle/systemd/mnt-edgevision.mount" "$board:$stage/mnt-edgevision.mount"
scp "$bundle/systemd/edgevision-outbox.service" "$board:$stage/edgevision-outbox.service"
```

传输内容：

1. NFS 挂载助手。
2. 自动挂 NFS 的 mount unit。
3. 执行 Outbox 的 service unit。

此时 NFS 还没挂，所以只能走已有 SSH/SCP。

### 11.4 创建板端目录

```sh
install -d -m 0755 /mnt/edgevision /userdata/edgevision-gateway /etc/systemd/system
```

- `/mnt/edgevision`：NFS 挂载点。
- `/userdata/edgevision-gateway`：持久数据库目录。
- `/etc/systemd/system`：本机自定义 unit 目录。
- `0755`：所有人可进入和读取，只有所有者可写。

### 11.5 持久安装 mount.nfs

```sh
install -m 0755 "$stage/mount.nfs" /usr/sbin/mount.nfs
ln -sf mount.nfs /usr/sbin/mount.nfs4
```

`install -m 0755` 复制并设置可执行权限；`ln -sf` 创建或更新符号链接。它们位于板端持久根文件系统，重启后仍存在。

### 11.6 安装 unit

```sh
install -m 0644 "$stage/mnt-edgevision.mount" /etc/systemd/system/mnt-edgevision.mount
install -m 0644 "$stage/edgevision-outbox.service" /etc/systemd/system/edgevision-outbox.service
systemctl daemon-reload
```

unit 是普通配置文件，使用 `0644`，不需要可执行权限。复制完成后必须 `daemon-reload`。

### 11.7 立即挂载并设置开机挂载

```sh
systemctl enable --now mnt-edgevision.mount
```

这同时完成：

1. `enable`：以后进入多用户启动阶段时自动挂载。
2. `--now`：现在立即挂载，不等重启。

挂载成功后才会出现 `/mnt/edgevision/edgevision-outbox` 的内容。

### 11.8 校验包

```sh
cd /mnt/edgevision/edgevision-outbox
sha256sum -c SHA256SUMS
```

全部显示 `OK` 才说明板端看到的 NFS 文件与制作时一致。

### 11.9 启用并首次运行 Outbox

```sh
systemctl enable edgevision-outbox.service
systemctl start edgevision-outbox.service
```

- `enable`：以后开机自动执行。
- `start`：现在执行一次。

当时数据库还不存在，所以服务输出 `NO_DATABASE` 并成功退出。它验证了路径和服务，不会自动生成或发布历史数据。

查看结果：

```sh
systemctl show edgevision-outbox.service \
  -p Result -p ExecMainStatus -p ActiveState
```

预期：

```text
Result=success
ExecMainStatus=0
ActiveState=inactive
```

`inactive` 不是失败，因为这是执行一次就退出的 oneshot。

执行首次引导：

```sh
cd /home/zoukunbo/project/edgevision-gateway
deploy/edgevision-outbox/install-board-from-wsl.sh
```

前提是 WSL 已导出 NFS，且 SSH 密钥能登录板端。

## 12. systemd 如何固化 NFS 挂载

### 12.1 mount unit 为什么叫 mnt-edgevision.mount

systemd 要求 mount unit 名与挂载路径对应：

```text
/mnt/edgevision
去掉开头 /
把 / 变成 -
加 .mount
得到 mnt-edgevision.mount
```

名字与 `Where=/mnt/edgevision` 不匹配时 systemd 会拒绝。

### 12.2 unit 逐项解释

```ini
[Unit]
Wants=network-online.target
After=network-online.target

[Mount]
What=192.168.0.100:/
Where=/mnt/edgevision
Type=nfs4
Options=ro,vers=4,proto=tcp,_netdev
TimeoutSec=20

[Install]
WantedBy=multi-user.target
```

| 配置 | 含义 |
| --- | --- |
| `Wants/After network-online.target` | 等网络在线阶段后再挂载 |
| `What` | WSL NFSv4 导出根 |
| `Where` | 板端挂载点 |
| `Type=nfs4` | 使用 NFSv4 |
| `ro` | 只读 |
| `proto=tcp` | 使用 TCP |
| `_netdev` | 标记为网络文件系统 |
| `TimeoutSec=20` | 最多等待20秒 |
| `WantedBy=multi-user.target` | 允许设置为多用户阶段开机启动 |

执行 `systemctl enable mnt-edgevision.mount` 后，systemd 会创建：

```text
/etc/systemd/system/multi-user.target.wants/mnt-edgevision.mount
    -> /etc/systemd/system/mnt-edgevision.mount
```

这个符号链接就是开机自动挂载的固化形式。

手动控制：

```sh
systemctl start mnt-edgevision.mount
systemctl stop mnt-edgevision.mount
systemctl enable mnt-edgevision.mount
systemctl disable mnt-edgevision.mount
systemctl status mnt-edgevision.mount
mount | grep /mnt/edgevision
```

`status` 看 systemd 判断，`mount` 看内核是否真的挂载，建议一起看。

## 13. systemd 如何实现程序开机执行

service 的关键依赖：

```ini
[Unit]
Requires=mnt-edgevision.mount
After=mnt-edgevision.mount network-online.target
ConditionFileIsExecutable=/mnt/edgevision/edgevision-outbox/scripts/run-outbox-once.sh
```

- `Requires`：服务需要 NFS mount。
- `After`：先挂 NFS，再运行程序。
- `ConditionFileIsExecutable`：运行脚本存在且可执行才启动。

这避免了开机时 NFS 尚未挂好就寻找脚本。

执行部分：

```ini
[Service]
Type=oneshot
ExecStart=/mnt/edgevision/edgevision-outbox/scripts/run-outbox-once.sh
TimeoutStartSec=20
Restart=on-failure
RestartSec=5
```

- `Type=oneshot`：做一个有限任务，完成后退出。
- `ExecStart`：真正执行的包装脚本。
- `TimeoutStartSec=20`：单次最长20秒。
- `Restart=on-failure`：失败才重试。
- `RestartSec=5`：5秒后重试。

它不是常驻程序，不持续监控数据库。只在开机、手动 start 或失败重试时扫描一次 pending。

```ini
[Install]
WantedBy=multi-user.target
```

执行 `systemctl enable edgevision-outbox.service` 会创建：

```text
/etc/systemd/system/multi-user.target.wants/edgevision-outbox.service
    -> /etc/systemd/system/edgevision-outbox.service
```

开机进入多用户阶段后 systemd 拉起它；它再通过依赖先拉起 NFS mount。

## 14. run-outbox-once.sh 如何连接 systemd 和程序

固定路径：

```sh
bundle=/mnt/edgevision/edgevision-outbox
state=/userdata/edgevision-gateway
database="$state/gateway.db"
mkdir -p "$state"
```

`bundle` 是只读程序目录，`state` 是板端持久状态目录。

没有数据库时：

```sh
if [ ! -f "$database" ]; then
    echo "NO_DATABASE: $database; nothing to deliver"
    exit 0
fi
```

首次安装没有数据是正常空状态，所以退出码为 0，避免 systemd 把它当故障不断重启。

真正投递：

```sh
LD_LIBRARY_PATH="$bundle/lib" \
"$bundle/bin/sqlite_outbox_mqtt_demo" deliver-mqtt "$database"
```

程序读取 pending，发布 MQTT，收到 PUBACK 后把 Outbox 改为 sent。

数据库存在但没有 pending 时，程序输出 `NO_PENDING`。包装脚本把它视为成功。其他连接或发布错误保留非零退出码，systemd 才会重试，数据库记录保持 pending。


## 15. 第一次启动、手动启动和开机启动的真实行为

### 15.1 安装脚本第一次启动

安装脚本执行：

```sh
systemctl enable edgevision-outbox.service
systemctl start edgevision-outbox.service
```

当时数据库不存在，日志为：

```text
NO_DATABASE: /userdata/edgevision-gateway/gateway.db; nothing to deliver
```

服务成功退出，没有发布 MQTT。

### 15.2 播种历史数据后手动启动

先运行 `seed-historical-replay.sh`。它读取 NFS 上的历史 JSON，把 Measurement 和 pending Outbox 写入板端数据库。状态变成：

```text
measurement_count=1 pending_count=1 sent_count=0
```

再执行：

```sh
systemctl start edgevision-outbox.service
```

完整顺序：

```text
systemctl start
      ↓
systemd检查NFS依赖
      ↓
执行run-outbox-once.sh
      ↓
程序读取pending
      ↓
连接板端127.0.0.1:1883
      ↓
SSH隧道转发到WSL Broker
      ↓
Broker返回QoS 1 PUBACK
      ↓
SQLite Outbox改成sent
      ↓
脚本退出0，oneshot结束
```

最终状态：

```text
measurement_count=1 pending_count=0 sent_count=1
```

### 15.3 再手动启动

再次 `systemctl start` 时没有 pending，日志是 `NO_PENDING`，不会再次发布同一记录。

### 15.4 以后每次开机

因为 mount 和 service 都已 enable：

```text
网络进入online阶段
      ↓
mnt-edgevision.mount自动启动
      ↓
NFS出现在/mnt/edgevision
      ↓
edgevision-outbox.service自动启动
      ↓
扫描一次/userdata中的pending
      ├── 有pending：尝试发布
      ├── 没pending：NO_PENDING
      └── 没数据库：NO_DATABASE
```

本次重启时数据库已是 sent，所以开机日志为 `NO_PENDING`，数据库仍为 `1/0/1`。

### 15.5 新 pending 会自动立刻发送吗

不会。当前没有常驻 worker 或 timer。

板卡已经运行后，若其他程序写入新 pending，需要手动执行：

```sh
systemctl start edgevision-outbox.service
```

或者等待下次开机。持续自动投递属于后续设计。

## 16. 如何手动开启、停止和查看服务

以下命令在板端执行，也可以在 WSL 前加 `ssh root@192.168.0.232`。

立即投递一次：

```sh
systemctl start edgevision-outbox.service
```

查看执行结果和日志：

```sh
systemctl show edgevision-outbox.service \
  -p Result -p ExecMainStatus -p ActiveState
journalctl -u edgevision-outbox.service -n 30 --no-pager
```

设置开机执行：

```sh
systemctl enable edgevision-outbox.service
```

取消开机执行：

```sh
systemctl disable edgevision-outbox.service
```

`enable` 不会立即执行；`start` 不会自动设置下次开机。合并写法：

```sh
systemctl enable --now edgevision-outbox.service
```

对 oneshot 来说，它立即运行一次，完成后仍显示 inactive。

`systemctl stop` 只有在任务正在执行时才有意义；任务已经成功退出，本来就是 inactive。

修改 unit 后要执行：

```sh
systemctl daemon-reload
systemctl start edgevision-outbox.service
```

## 17. 历史回放验证怎样执行

这部分会真实发布一次历史 JSON。当前已经验证通过，不需要为了阅读本文重复执行。

### 17.1 先在 WSL 启动独立订阅端

```sh
mosquitto_sub \
  -h 127.0.0.1 -p 1883 \
  -q 1 -d -v -R \
  -C 1 -W 25 \
  -t edgevision/v1/devices/stm32-dht11-01/measurements
```

| 参数 | 含义 |
| --- | --- |
| `-h/-p` | Broker地址和端口 |
| `-q 1` | QoS 1订阅 |
| `-d` | 显示SUBACK/PUBLISH/PUBACK |
| `-v` | 显示topic和payload |
| `-R` | 不输出retained历史消息 |
| `-C 1` | 收到一条后退出 |
| `-W 25` | 最多等待25秒 |
| `-t` | topic |

先看到 `received SUBACK`，再触发板端发布，避免错过非 retained 消息。

### 17.2 播种历史数据

```sh
ssh root@192.168.0.232 \
  '/mnt/edgevision/edgevision-outbox/scripts/seed-historical-replay.sh'
```

脚本核心是：

```sh
sqlite_outbox_delivery_demo seed gateway.db < temperature-replay.json
```

`<` 表示把 JSON 文件作为程序标准输入。固定演示数据面向空数据库；当前数据库已有该记录，重复播种可能因唯一键冲突失败。不要为重复演示直接删除正式数据库。

### 17.3 查看发送前状态

```sh
ssh root@192.168.0.232 '
  LD_LIBRARY_PATH=/mnt/edgevision/edgevision-outbox/lib   /mnt/edgevision/edgevision-outbox/bin/sqlite_outbox_mqtt_demo   status /userdata/edgevision-gateway/gateway.db
'
```

首次播种后预期 `measurement=1 pending=1 sent=0`。

### 17.4 开启服务并看日志

```sh
ssh root@192.168.0.232   'systemctl start edgevision-outbox.service'

ssh root@192.168.0.232   'journalctl -u edgevision-outbox.service -n 30 --no-pager'
```

关键日志：

```text
PUBACK_THEN_MARKED_SENT id=<1>
```

再查数据库，预期 `measurement=1 pending=0 sent=1`。独立订阅端收到的 device、metric、timestamp 和 value 应与输入一致。

## 18. 怎样验证“固化”真的生效

重启前：

```sh
systemctl is-enabled mnt-edgevision.mount
systemctl is-enabled edgevision-outbox.service
```

都应返回 `enabled`。重启：

```sh
systemctl reboot
```

重新登录后分四层检查。

NFS：

```sh
systemctl is-active mnt-edgevision.mount
mount | grep /mnt/edgevision
```

服务：

```sh
systemctl is-enabled edgevision-outbox.service
journalctl -b -u edgevision-outbox.service --no-pager
```

`-b` 表示只看本次开机。

数据库：

```sh
LD_LIBRARY_PATH=/mnt/edgevision/edgevision-outbox/lib \
/mnt/edgevision/edgevision-outbox/bin/sqlite_outbox_mqtt_demo \
status /userdata/edgevision-gateway/gateway.db
```

MQTT 隧道监听：

```sh
netstat -ltn | grep 127.0.0.1:1883
```

本次实测：

- NFS active、enabled，且实际以 nfs4/ro 挂载。
- Outbox service enabled，开机执行成功并输出 `NO_PENDING`。
- 数据库仍为 `measurement=1 pending=0 sent=1`。
- 板端 `127.0.0.1:1883` 为 LISTEN。
- WSL 隧道 active、enabled。

## 19. 常见故障定位

### 19.1 mount.nfs 找不到

```sh
ls -l /usr/sbin/mount.nfs /usr/sbin/mount.nfs4
file /usr/sbin/mount.nfs
```

不存在就重新执行首次引导。存在但不能运行，检查是否 AArch64 和动态库是否匹配。

### 19.2 No such device 或 unknown filesystem type nfs4

检查：

```sh
cat /proc/filesystems | grep nfs
```

没有结果说明更可能缺内核支持，需要回到 Buildroot/内核配置，不是重复复制助手。

### 19.3 NFS 超时

依次检查：

1. WSL 当前 IP 是否仍为 `192.168.0.100`。
2. 板端到 WSL 网络是否可达。
3. WSL NFS 服务是否 active。
4. `exportfs -v` 是否允许 `192.168.0.232`。
5. mount unit 的 `What=` 是否正确。

WSL：

```sh
hostname -I
systemctl status nfs-kernel-server
exportfs -v
```

板端：

```sh
systemctl status mnt-edgevision.mount
journalctl -b -u mnt-edgevision.mount --no-pager
```

修改 unit 后：

```sh
systemctl daemon-reload
systemctl restart mnt-edgevision.mount
```

### 19.4 找不到 libmosquitto.so.1

检查：

```sh
ls -l /mnt/edgevision/edgevision-outbox/lib/libmosquitto.so.1
```

通过包装脚本启动会自动设置 `LD_LIBRARY_PATH`；手动直接运行二进制时必须自己设置。

### 19.5 inactive 是否失败

```sh
systemctl show edgevision-outbox.service \
  -p Result -p ExecMainStatus -p ActiveState
```

- `Result=success`、`ExecMainStatus=0`、`inactive`：oneshot 正常完成。
- `Result=exit-code` 或非零退出码：失败，应查 journal。

### 19.6 MQTT 连接失败

板端：

```sh
netstat -ltn | grep 127.0.0.1:1883
```

WSL：

```sh
systemctl status mosquitto
systemctl status edgevision-board-mqtt-tunnel.service
journalctl -u edgevision-board-mqtt-tunnel.service -n 50 --no-pager
```

隧道失败时 Outbox 应保持 pending。恢复后再手动 start 服务。

### 19.7 NFS active，但没有新数据

NFS 只提供程序，不产生 Measurement；oneshot 也不持续监听数据库。必须先有采集/播种行为产生 pending，再在开机或手动启动时投递。

## 20. 常用命令词典

| 命令 | 初学者理解 |
| --- | --- |
| `cd 路径` | 进入目录 |
| `sudo 命令` | 以管理员权限执行 |
| `ssh 用户@IP 命令` | 在另一台Linux执行命令 |
| `scp 来源 用户@IP:目标` | 通过SSH复制文件 |
| `install -m 0755` | 复制并设为可执行 |
| `install -m 0644` | 复制普通配置文件 |
| `ln -sf` | 创建或更新符号链接 |
| `systemctl daemon-reload` | 让systemd重新读取unit |
| `systemctl enable` | 设置开机启动，不代表现在运行 |
| `systemctl start` | 现在启动，不代表以后开机启动 |
| `systemctl enable --now` | 设置开机启动并立即启动 |
| `systemctl is-enabled` | 查询是否开机启动 |
| `systemctl is-active` | 查询现在是否运行 |
| `journalctl -u` | 查看unit日志 |
| `mount` | 查看内核已挂载文件系统 |
| `grep` | 只显示匹配行 |
| `sha256sum -c` | 按清单校验文件 |
| `LD_LIBRARY_PATH=目录 命令` | 只为本命令增加动态库目录 |

## 21. 停用而不删除数据

取消板端服务开机执行：

```sh
systemctl disable edgevision-outbox.service
```

停止并取消 NFS：

```sh
systemctl disable --now mnt-edgevision.mount
```

WSL 停止隧道：

```sh
sudo systemctl disable --now edgevision-board-mqtt-tunnel.service
```

不要顺手删除 `/userdata/edgevision-gateway/gateway.db`。停用服务、卸载 NFS、删除数据库是三个不同动作。

## 22. 当前边界与证据

当前边界：

1. 部署的是 Outbox 演示程序，不是完整默认 Gateway。
2. 服务是 oneshot，只在开机、手动 start 或失败重试时运行一次。
3. NFS 依赖 WSL 在线和固定 IP，适合开发部署，不等同于把程序烧进板端镜像。
4. NFS 使用 `sec=sys`、无加密；本次用板端 IP 限制和只读导出缩小风险。
5. MQTT 是 at-least-once；PUBACK 后、sent 写回前崩溃可能重复。
6. 本次没有修改 STM32、UART/GPIO、Broker 配置或默认 Gateway 数据源。
7. D41 的30分钟运行、三类故障注入和Git版本点仍未完成。

证据：

- `hardware/storage/systemd-nfs-board-deployment-2026-09-01.log`
  - 首次无数据库启动成功。
  - pending 由1变0，sent由0变1。
  - 独立订阅端收到26.1°C JSON。
  - journal出现 `PUBACK_THEN_MARKED_SENT`。
  - 重复启动出现 `NO_PENDING`。
  - 重启后NFS、服务、隧道和数据库恢复。
- `hardware/storage/d42-auditable-replay-2026-09-01.log`
  - 部署包10项SHA256通过。
  - 固定输入、运行日志和板端终态可重新审计。

配套脚本：

- `deploy/edgevision-outbox/install-wsl-host.sh`
- `deploy/edgevision-outbox/install-board-from-wsl.sh`
- `deploy/edgevision-outbox/audit-replay.sh`
- `deploy/edgevision-outbox/scripts/run-outbox-once.sh`
- `deploy/edgevision-outbox/scripts/seed-historical-replay.sh`

读完后应能回答：

1. 板端原先缺的是内核驱动，还是 `mount.nfs` 助手？
2. 为什么第一次必须用 SCP？
3. 哪些文件固化在板端，哪些仍在 WSL？
4. `enable` 与 `start` 有什么区别？
5. 为什么 oneshot 成功后显示 inactive？
6. 开机时怎样保证先挂 NFS、再运行服务？
7. 新 pending 为什么还需要手动 start？
