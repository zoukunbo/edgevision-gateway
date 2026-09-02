# D40：真实PUBACK后标记sent

本练习复用sqlite_outbox_delivery_demo的seed命令创建pending，输入仍是历史真实温度JSON。
它不查询STM32。publish_real复用项目现有mqtt_publisher，发布QoS1并等待匹配PUBACK。

完成sqlite_outbox_mqtt_demo.c的Q1-Q3，再把MQTT_OUTBOX_EXERCISE_READY改为1。
顺序：load pending → publish_real → MQTT_PUBLISHER_OK → mark_sent。
任何连接、超时、序列化或发布错误都必须保持pending。
数据库中的topic会与Measurement推导主题核对，不允许静默忽略不一致。

验收时助手负责：新库seed；独立订阅端SUBACK先就绪；执行一次deliver-mqtt；核对订阅JSON；确认数据库从pending变sent；关闭订阅端。本轮不访问UART/GPIO/STM32，不重配Broker。

边界：PUBACK只表示Broker确认；独立订阅记录提供额外接收证据。PUBACK后到sent写回前崩溃仍可能导致重启重复发布，因此保持at-least-once口径。
