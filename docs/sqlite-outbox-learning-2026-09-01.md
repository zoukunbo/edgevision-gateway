# SQLite / Outbox 学习总结｜2026-09-01

## 已完成的数据流

历史真实STM32 Measurement JSON → SQLite事务保存Measurement与pending Outbox → 进程退出/重开仍pending → 现有mqtt_publisher QoS1发布 → WSL Broker → 独立订阅端 → PUBACK后sent。

## 最小证据

- `../hardware/storage/d39-transaction-verification-2026-09-01.log`：正常1/1；Outbox约束失败后0/0。
- `../hardware/storage/d40-offline-delivery-verification-2026-09-01.log`：未确认保持pending，确认后sent。
- `../hardware/storage/d40-real-mqtt-outbox-verification-2026-09-01.log`：独立订阅端与真实PUBACK后sent。
- `../hardware/storage/d40-real-failure-recovery-verification-2026-09-01.log`：127.0.0.1:1884真实连接超时仍pending；恢复到现有1883后补发成功。

所有MQTT数据均为用户明确授权的2026-08-31历史温度JSON回放，不是2026-09-01新增硬件采样。未修改或重启Broker，未访问UART/GPIO/STM32。

## 已理解的关键边界

Measurement和Outbox必须同一事务提交；错误路径显式回滚并保留原错误码。发布未确认不得标sent；只有mqtt_publisher返回MQTT_PUBLISHER_OK（匹配QoS1 PUBACK）后才更新。独立订阅记录与PUBACK是两份证据。

这是at-least-once基础：PUBACK后到sent写回前崩溃可能导致重启重复发布。尚无并发领取、重试退避、死信、消费者幂等键或exactly-once。练习程序是主机离线/本机Broker入口，尚未接入默认Gateway，也不是正式部署服务。

## 下一入口

进入C块D41+D42，先做干净构建与历史数据回放的最小可复现部署说明，不重新查询硬件，不把整个部署周一次铺开。
