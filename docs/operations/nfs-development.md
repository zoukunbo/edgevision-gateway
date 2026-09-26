# NFS 开发环境运维指南

这是仓库内 NFS 配置与排障的唯一维护入口。它说明如何让 WSL/Linux 主机以只读方式向 OK1126B-S 提供构建产物。ADB、SCP、NFS 的选择和日常部署命令见 [快速部署指南](quick-deployment.md)。

## 1. 拓扑与固定边界

```text
开发主机导出目录 ── NFSv4/TCP 2049 ──> 板端 /mnt/edgevision
板端可写状态                               /userdata/edgevision-gateway
```

- NFS 内容只读，程序和脚本由开发主机维护。
- 数据库等运行状态放在板端 `/userdata`，不要写进只读挂载。
- 开发主机离线、IP 改变或服务停止时，板端不能从 NFS 启动程序。
- NFS `sec=sys` 不加密，只应在可信开发网络使用，并限制客户端 IP。

下方示例沿用历史环境：开发主机 `192.168.0.100`，板端 `192.168.0.232`。实际使用前先替换成当前地址。

## 2. 主机端配置

```bash
sudo apt install nfs-kernel-server
systemctl status nfs-kernel-server
```

在 `/etc/exports` 中只保留一个项目导出，例如：

```text
/home/zoukunbo/project/edgevision-gateway/deploy/nfs-root 192.168.0.232(ro,sync,no_subtree_check,insecure,fsid=0)
```

应用并核对：

```bash
sudo exportfs -rav
sudo systemctl restart nfs-kernel-server
sudo exportfs -v
sudo ss -lntp | grep ':2049'
```

主机防火墙只允许板端访问 TCP 2049。WSL mirrored networking 还需要检查 Windows/Hyper-V 防火墙规则。

## 3. 板端前置检查

```sh
cat /proc/filesystems | grep nfs
command -v mount.nfs
file /usr/sbin/mount.nfs
```

- 没有 `nfs`/`nfs4`：需要重新配置并更新内核，复制工具无效。
- 内核支持但没有 `mount.nfs`：在匹配的 Buildroot 中启用 `nfs-utils`。
- `mount.nfs` 必须是与板端一致的 ARM64 产物，不能复制主机的 x86-64 程序。

Buildroot 常用配置位置：

```text
Package Selection for the target
  -> Filesystem and flash utilities
    -> nfs-utils
      [*] NFSv4/NFSv4.1
```

## 4. 手动挂载与卸载

```sh
mkdir -p /mnt/edgevision
mount.nfs 192.168.0.100:/ /mnt/edgevision \
  -o vers=4,proto=tcp,ro
mount | grep /mnt/edgevision
ls -la /mnt/edgevision
```

卸载：

```sh
umount /mnt/edgevision
```

如果工具尚未持久安装，可以先用 SCP 引导复制 ARM64 `mount.nfs`；这种 `/tmp` 方案在重启后会消失。持久安装和开机挂载见 [systemd 板端部署](systemd-board-deployment.md)。

## 5. 日常开发流程

```text
主机修改代码 -> ARM64 构建 -> 更新导出目录 -> 板端直接运行挂载中的产物
```

更新后用 `sha256sum` 同时核对主机文件和板端可见文件，避免运行旧产物。重新构建前结束仍占用旧文件或设备的进程。

## 6. 故障定位

| 现象 | 检查顺序 |
| --- | --- |
| `unknown filesystem type nfs4` | `/proc/filesystems`、内核 NFS 配置 |
| 找不到 `mount.nfs` | `nfs-utils`、安装路径、ARM64 架构 |
| 挂载超时 | 主机 IP、TCP 2049、防火墙、NFS 服务、`exportfs -v` |
| `access denied` | `/etc/exports` 的目录、客户端 IP 和 `fsid=0` |
| 能挂载但程序不能执行 | 架构、执行权限、动态库和解释器 |
| 挂载正常但没有新数据 | NFS 只提供文件；检查采集进程和 `/userdata` 状态 |

完整的 2026-09-01 执行过程和原始环境细节保存在 [历史部署记录](../records/systemd-nfs-board-deployment-2026-09-01.md)，不应把其中的旧 IP、旧程序和 oneshot 行为直接当成当前配置。
