# D40：确认后再把 pending 标为 sent

输入是SQLite中最早的一条pending Outbox；seed仍使用历史真实温度JSON离线回放。
输出是一次离线发布替身结果，以及数据库状态pending或sent。
链路位置：SQLite/Outbox之后、真实mqtt_publisher之前。

完成sqlite_outbox_delivery_demo.c中的D1-D3，再把DELIVERY_EXERCISE_READY改为1。
顺序必须是load pending → publish → confirmed后mark sent。未确认时不能执行UPDATE。
发布替身只是可控测试接口，没有Broker、PUBACK或网络证据。

最小验收使用同一个新数据库、多个独立进程：
1. seed后状态1 measurement / 1 pending / 0 sent。
2. deliver-fail退出非0，重新打开仍为1/1/0。
3. deliver-ok退出0，重新打开变为1/0/1。
4. 再次deliver-ok报告NO_PENDING且退出非0，不重复标记。

这实现的是at-least-once方向的状态基础。若Broker已确认后、UPDATE前进程崩溃，记录仍pending，重启会再次发布；本轮不解决消费者去重、并发领取、重试退避或真实MQTT。
