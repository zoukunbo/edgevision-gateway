# Edge Gateway Lite v0.1 发布骨架

这个目录是正式常驻 Gateway 的独立发布入口。旧的
`deploy/edgevision-outbox/` oneshot 教学部署继续保留，二者不会互相覆盖。

## 发布包结构

```text
edge-gateway-lite/
├── bin/gateway
├── lib/libmosquitto.so.1
├── config/edge-gateway-lite.env
├── scripts/run-gateway.sh
├── scripts/health-check.sh
├── systemd/edge-gateway-lite.service
├── install-board.sh
└── SHA256SUMS
```

程序和共享库位于只读发布目录；数据库与日志写入
`/userdata/edgevision-gateway`，升级二进制不会覆盖运行状态。正式 v0.1 默认使用
`edge-gateway-lite-v0.1.db`，不会自动打开历史 oneshot 的未版本化 `gateway.db`；
历史数据迁移必须通过单独、可审计的迁移步骤完成。

## 1. 组装发布包

先完成 ARM64、Storage+MQTT 构建，再显式传入构建目录、Buildroot sysroot 和输出目录：

```sh
deploy/edge-gateway-lite/prepare-bundle.sh \
  build-arm64-d40-stm32-gateway-20260907 \
  /path/to/buildroot/sysroot \
  deploy/nfs-root/edge-gateway-lite
```

脚本拒绝非空输出目录，避免把旧发布物与新版本混装。发布包由 `.gitignore` 排除，
源码仓库只保存可复现的模板和脚本。

## 2. 安装到目标板

把发布包放到板端可访问的 `/mnt/edgevision/edge-gateway-lite`，然后以 root 执行：

```sh
/mnt/edgevision/edge-gateway-lite/install-board.sh
```

首次安装会写入默认配置；已有 `/etc/edgevision-gateway/edge-gateway-lite.env`
不会被覆盖。安装脚本会校验 SHA256、安装 systemd unit、创建状态目录并启动服务。

## 3. 运维命令

```sh
systemctl status edge-gateway-lite.service
systemctl restart edge-gateway-lite.service
systemctl stop edge-gateway-lite.service
/mnt/edgevision/edge-gateway-lite/scripts/health-check.sh
journalctl -u edge-gateway-lite.service --no-pager
```

健康检查同时核对服务、发布版本、真实源设备节点和 SQLite 统计。它只读取状态，
不会启动、停止或修改 Gateway。

## 当前边界

- 默认使用 STM32 Modbus 源、板端 UART5、GPIO22 和本机 MQTT 1883。
- systemd 以 root 运行以访问当前板端受限的 GPIO 字符设备；生产环境应再收紧权限。
- 当前 Gateway CLI 只允许配置 source 和 database；串口/GPIO/MQTT 参数仍采用程序默认值。
- v0.1 是 at-least-once；消费端必须按持久化 `message_id` 幂等。
