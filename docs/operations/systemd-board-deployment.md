# systemd 板端部署运维指南

本文维护板端 NFS 自动挂载和 Gateway 服务托管的通用做法。NFS 服务端配置与手动排障统一见 [NFS 开发环境运维指南](nfs-development.md)。

## 1. 持久位置

```text
/usr/sbin/mount.nfs                         NFS 挂载助手
/etc/systemd/system/mnt-edgevision.mount   自动挂载单元
/etc/systemd/system/edgevision-gateway.service  程序服务单元
/userdata/edgevision-gateway/              板端可写状态
```

首次引导可以通过 SCP 安装这些文件；之后程序可从只读 NFS 读取，状态继续留在 `/userdata`。

## 2. NFS mount unit

挂载 `/mnt/edgevision` 的 unit 必须命名为 `mnt-edgevision.mount`：

```ini
[Unit]
Description=EdgeVision development files
After=network-online.target
Wants=network-online.target

[Mount]
What=192.168.0.100:/
Where=/mnt/edgevision
Type=nfs4
Options=vers=4,proto=tcp,ro

[Install]
WantedBy=multi-user.target
```

替换示例 IP 后执行：

```sh
systemctl daemon-reload
systemctl enable --now mnt-edgevision.mount
systemctl status mnt-edgevision.mount
mount | grep /mnt/edgevision
```

`status` 表示 systemd 的判断，`mount` 表示内核实际挂载状态，两者都要检查。

## 3. Gateway service unit

```ini
[Unit]
Description=EdgeVision Gateway
Requires=mnt-edgevision.mount
After=mnt-edgevision.mount network-online.target

[Service]
Type=simple
ExecStart=/mnt/edgevision/edge-gateway-lite/bin/gateway
WorkingDirectory=/userdata/edgevision-gateway
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

`Requires` 建立依赖，`After` 规定启动顺序。当前正式部署脚本和实际参数以 `deploy/edge-gateway-lite/` 为准，不能照抄旧 Outbox oneshot 示例。

## 4. 常用操作

```sh
systemctl daemon-reload
systemctl enable --now edgevision-gateway.service
systemctl restart edgevision-gateway.service
systemctl status edgevision-gateway.service
journalctl -b -u edgevision-gateway.service --no-pager
```

停止服务但保留数据：

```sh
systemctl disable --now edgevision-gateway.service
systemctl disable --now mnt-edgevision.mount
```

不要把停止服务、卸载 NFS 和删除 `/userdata` 数据合并成一个动作。

## 5. 验收顺序

1. `mount.nfs` 是 ARM64 且可以执行。
2. mount unit 已启用，重启后 `/mnt/edgevision` 真实挂载。
3. 程序、动态库和配置在挂载目录中可读。
4. `/userdata/edgevision-gateway` 可写且重启后保留。
5. 服务退出时能收到 SIGTERM 并正常清理资源。
6. 服务失败时按预期重启，不形成无限快速重启。
7. `journalctl` 能定位 NFS、动态库、配置和进程错误。

2026-09-01 的 Outbox oneshot 部署属于历史验证，其完整执行过程见 [systemd/NFS 历史部署记录](../records/systemd-nfs-board-deployment-2026-09-01.md)。
