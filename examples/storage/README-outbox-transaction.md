# D39：Measurement 与 Outbox 同成同败

## 为什么做

如果先提交Measurement，进程在创建待发记录前失败，数据会永久留在库中但永远不会上报。
本练习用一个显式事务包住两次INSERT：成功时一起COMMIT；任一步失败时一起ROLLBACK。
SQLite不会保证每一种语句错误都自动回滚整个显式事务，因此错误路径由应用明确ROLLBACK。

## 输入、输出与链路位置

输入仍是2026-08-31真实温度JSON的离线回放，不重新访问硬件。
成功输出：measurements一行、outbox一行且state=pending。
故障输出：故意让outbox违反topic NOT NULL约束，两个表都保持0行。
位置：Measurement/JSON之后、MQTT发布之前。

本地measurement_id只关联两表，不等于设备sequence、MQTT消息ID或全局去重键。
Outbox目前只有pending状态；没有发送、确认、删除、重试或并发领取。

## 你练的关键逻辑

完成sqlite_outbox_demo.c中的T1-T3，再把TRANSACTION_EXERCISE_READY改为1：

1. `BEGIN IMMEDIATE`成功后标记事务已开始。
2. 插入Measurement，再插入Outbox；全部成功才`COMMIT`。
3. 任一步或COMMIT失败时，如果事务已开始，执行`ROLLBACK`并保留原错误码。

`BEGIN IMMEDIATE`会立即尝试启动写事务，其他连接已在写时可能返回SQLITE_BUSY。
本练习不做忙重试。不要在失败路径上用ROLLBACK的返回值覆盖最初的失败原因。
prepare/bind/step/finalize助手已完成，因为上一段已经练过。

## 完成标准

使用两个全新数据库分别验证：

- 正常库：save进程退出后，新status进程看到`measurement_count=1 pending_count=1`。
- 故障库：save-fail必须退出非0，新status进程看到`measurement_count=0 pending_count=0`。

测试指令在你完成代码后由助手逐条提供。本轮不连接MQTT。

参考：https://www.sqlite.org/lang_transaction.html
