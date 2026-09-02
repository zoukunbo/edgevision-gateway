# D42 最小可审计回放包｜2026-09-01

## 目标与停止点

本回放包不再次读取 STM32，也不再次发布 MQTT 数据。它把 D41 已经完成的单次正式回放按稳定输入、文件哈希、状态转换、PUBACK、独立订阅和重启终态重新核对，使同一结论可以由另一人从现有证据复查。

当前完成的是 D42 的最小证据回放。原 D42 计划中的正常/timeout/CRC/设备断开四类帧 corpus、`ReplayDataSource` 接入主工程、真实源与回放源对照以及 W07 Top-3 风险清单尚未完成，严格验收保持未勾选。

## 固定对象

| 对象 | 固定值 |
| --- | --- |
| 输入文件 | `deploy/nfs-root/edgevision-outbox/share/temperature-replay.json` |
| 输入 SHA256 | `1b5cc3d3ce5d0526dfdcf51a716b472b7590588ec0e00749420660f2a8a19ac2` |
| device | `stm32-dht11-01` |
| metric/value | `temperature / 26.1 celsius` |
| timestamp | `1788179459579` |
| topic | `edgevision/v1/devices/stm32-dht11-01/measurements` |
| 板端数据库 | `/userdata/edgevision-gateway/gateway.db` |
| 正式运行证据 | `hardware/storage/systemd-nfs-board-deployment-2026-09-01.log` |
| 正式证据 SHA256 | `62785e662bf6b5498786499fc723655fbc289974ff7b16f799603f9f5a1e3f6d` |
| 只读审计日志 | `hardware/storage/d42-auditable-replay-2026-09-01.log` |

## 证据链

| 阶段 | 输入或动作 | 可核对结果 | 正式证据行 |
| --- | --- | --- | --- |
| 订阅准备 | 独立客户端 QoS 1 订阅固定 topic | CONNACK、SUBACK | 2–6 |
| 写入待发 | 历史 JSON 经 seed 脚本写入板端数据库 | `measurement=1 pending=1 sent=0` | 12–15 |
| systemd 投递 | 手动启动 oneshot | `Result=success`、`ExecMainStatus=0` | 16–19 |
| Broker 确认 | 发布器收到 QoS 1 PUBACK 后写回 | `PUBACK_THEN_MARKED_SENT id=<1>` | 25–28 |
| 独立接收 | 独立订阅端收到 PUBLISH 并回 PUBACK | JSON 字段与固定输入相同 | 7–10 |
| 数据库终态 | 再次查询 SQLite | `measurement=1 pending=0 sent=1` | 29–30 |
| 幂等复核 | 再次启动 oneshot | `NO_PENDING`，终态不变 | 35–46 |
| 重启复核 | 板卡重启，systemd 自动执行 | NFS active/enabled，服务成功，仍为 `1/0/1` | 49–71 |

这里的“可审计”表示每个结论都有固定输入、哈希或日志位置。它不把 MQTT PUBACK 解释为业务订阅者已经完成处理；独立订阅日志单独证明消息到达订阅客户端。

## 一条命令重新审计

在 WSL 中运行：

```sh
cd /home/zoukunbo/project/edgevision-gateway
deploy/edgevision-outbox/audit-replay.sh \
  | tee hardware/storage/d42-auditable-replay-$(date +%F-%H%M%S).log
```

脚本只做以下只读检查：

1. 用 `SHA256SUMS` 校验完整 NFS 部署包。
2. 输出固定历史 JSON 及其 SHA256。
3. 从正式运行日志提取订阅、PUBACK、sent、幂等和重启证据。
4. 通过 SSH 查询板端当前 mount、service、SQLite 终态和既有 journal。

它不会运行 seed、不会启动投递服务、不会发布 MQTT、不会清理数据库，也不会访问 STM32/UART/GPIO。

## 本次新运行结果

2026-09-01 18:49 CST 执行只读审计：

- NFS 包内 10 个清单项全部 `OK`。
- 输入 JSON SHA256 与固定值一致。
- 正式证据日志 SHA256 与本页固定值一致。
- 板端 NFS 为 active，Outbox service 为 enabled；最近结果 success，oneshot 正常显示 inactive。
- 板端数据库仍为 `measurement_count=1 pending_count=0 sent_count=1`。
- journal 同时保留一次 `PUBACK_THEN_MARKED_SENT` 和后续两次 `NO_PENDING`。

## 边界与下一步

本包复核的是已经授权并完成的历史温度单条回放，不产生第二次发布。它证明 D41 最小部署证据内部一致，但不替代 D42 原计划中的四类串口故障 replay corpus。

如果继续 D42 严格验收，下一行为应是先整理现有真实 normal/timeout/CRC/设备断开原始帧与期望结果，形成不接硬件也能运行的 corpus；此时再决定是否实现 `ReplayDataSource`。不应从头重做 CRC、串口事务或 SQLite/Outbox。
