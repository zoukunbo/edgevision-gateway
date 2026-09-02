# D39：一条 Measurement 保存后由新进程读回

A块真实STM32到MQTT最小验证已于2026-08-31完成，证据：
`../../hardware/rs485/2026-08-31-stm32-live-mqtt-203058.log`。
本练习输入是该记录中的温度JSON离线回放，非本轮新增硬件采样。
保留原来源、时间、sequence与quality；不得据此声称当前温度仍为26.1°C。

## 本轮行为与边界

输入：一条通过现有measurement_from_json校验的JSON。
输出：SQLite文件的一行数据；写入进程退出后另一个进程读回。
位置：Measurement/JSON之后、未来Outbox与MQTT之前。
暂用id和payload_json两列，不拆成八个字段，避免重复已有领域契约。
数据库id是本地行号，不是设备sequence，也不是全局事件ID或去重保证。
本轮只用一条INSERT的隐式事务；下一段才做Measurement与Outbox两表同成同败。

## 你练的代码

在sqlite_measurement_demo.c的insert_json中完成S1-S3：
prepare_v2准备SQL → bind_text绑定参数 → step执行并检查SQLITE_DONE。
prepare/bind成功为SQLITE_OK；这条INSERT的step成功为SQLITE_DONE。
任何失败走统一cleanup；不要拼接JSON到SQL、不要吞掉错误或直接返回成功。
SQLITE_TRANSIENT要求SQLite复制绑定字符串；?1的参数下标是1。
完成后将INSERT_EXERCISE_READY设为1。未完成版本会明确退出，不创建数据库。
初始化、建表、领域校验、读回和清理外壳已提供。

## WSL运行（在项目根目录）

```sh
cmake -S examples/storage -B build-d39-sqlite
cmake --build build-d39-sqlite -j2
```

填完练习、重新构建后，依次运行：

```sh
task_db_dir=$(mktemp -d "$PWD/build-d39-sqlite/replay-XXXXXX")
./build-d39-sqlite/sqlite_measurement_demo put "$task_db_dir/measurements.db" < examples/storage/temperature-replay.json
./build-d39-sqlite/sqlite_measurement_demo read "$task_db_dir/measurements.db" > "$task_db_dir/readback.json"
cmp examples/storage/temperature-replay.json "$task_db_dir/readback.json"
```

每次用新的临时目录；同一个库重复put会追加一行，本轮没有去重。
cmp退出0且无输出表示文本一致；put/read是两个独立进程。
失败即检查错误信息，不把编译成功当作存储通过。
完成标准仅为正常进程退出后的读回一致，不是异常断电/磁盘故障验收。

本轮不访问硬件、不发MQTT、不实现补发、线程池或正式Gateway存储接口。

参考：
- https://www.sqlite.org/cintro.html
- https://www.sqlite.org/c3ref/bind_blob.html
