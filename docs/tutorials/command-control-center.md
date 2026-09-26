# D49 命令控制中心：本地与 MQTT 共用同一执行路径

本文用于复习、学习和实际使用 EdgeVision Gateway 的命令控制中心。重点不是记住某个
`if` 分支，而是理解一条命令如何从不同入口到达同一业务处理函数，并得到一致结果。

## 1. 解决什么问题

网关需要同时支持两类控制端：

- 板端或同机维护程序通过 Unix domain socket 发命令；
- 远程控制端通过 MQTT 请求主题发命令，并从响应主题接收 JSON 结果。

如果两个入口各自实现业务逻辑，参数校验、状态修改和错误语义容易逐渐不一致。本项目
因此把入口适配与业务执行分开：入口只负责接收和包装，所有命令最终进入同一个
`gateway_handle_command()`。

## 2. 总体数据流

```text
本地 gatewayctl
    │ Unix socket，一行文本
    v
本地 command worker ───────────────┐
                                    v
                            gateway_execute_command
                                    │ command_mutex 串行化
                                    v
                            gateway_handle_command
                                    │
                    ┌───────────────┼────────────────┐
                    │               │                │
                 读状态        改内存状态       Storage worker
              status/version   pause/resume     set/get storage
                    │               │                │
                    └───────────────┴────────────────┘
                                    │ 文本 reply
                                    v
远程 MQTT worker ───────────── 包装 JSON 响应并发布
    ^
    │ MQTT JSON 请求、request_id 校验与去重
远程控制端
```

两个入口的共同路径是：

```text
gateway_execute_command() -> gateway_handle_command()
```

`gateway_execute_command()` 使用 `command_mutex` 保证命令串行执行；
`gateway_handle_command()` 根据命令完成参数校验、状态读取或业务交付并生成文本回复。

## 3. 入口分别负责什么

### 3.1 本地 Unix socket

`gatewayctl` 发送一条以换行结束的文本命令。手工开发启动时，客户端与服务端
都默认使用 `/tmp/edgevision-study.sock`；可通过 `EDGEVISION_COMMAND_SOCKET`
同时改变两端路径。服务端
读取完整行后调用共享执行函数，再把文本回复写回客户端。

在正式 systemd 部署中，unit 创建 `/run/edgevision-gateway` 并设置
`EDGEVISION_COMMAND_SOCKET=/run/edgevision-gateway/control.sock`。运行时目录由
systemd 管理，因此即使服务保留 `PrivateTmp=true`，外部运维客户端仍可访问命令通道。
事务升级也通过该通道查询实际运行版本；只检查磁盘上的候选二进制不能证明
systemd 已切换到新进程。

示例：

```bash
./build-service/gatewayctl status
./build-service/gatewayctl get_config
./build-service/gatewayctl set_interval 1000
./build-service/gatewayctl pause_collection
./build-service/gatewayctl resume_collection
./build-service/gatewayctl get_storage_stats
./build-service/gatewayctl get_version
```

本地入口不需要 `request_id`，因为一次 socket 请求直接对应一次回复。

### 3.2 远程 MQTT

默认请求和响应主题为：

```text
edgevision/v1/devices/gateway-01/commands/request
edgevision/v1/devices/gateway-01/commands/response
```

请求必须包含非空 `request_id` 和 `command`：

```json
{"request_id":"version-check-1","command":"get_version"}
```

远程 worker 的职责是：

1. 解析 JSON 并验证字段；
2. 拒绝不在白名单中的命令；
3. 根据 `request_id + command` 查询响应缓存；
4. 未命中时调用共享命令执行路径；
5. 把文本结果包装为 JSON，缓存并发布。

成功响应示例：

```json
{"request_id":"version-check-1","status":"ok","result":"ok version=0.1.0"}
```

若共享 handler 返回 `error=...`，远程入口生成 `status=error`，并把等号后的内容放进
`error` 字段。MQTT 包装函数只负责协议格式，不重新实现命令业务。

## 4. 当前命令

| 命令 | 作用 | 关键结果或边界 |
| --- | --- | --- |
| `status` | 查询运行状态 | 返回间隔、累计采集数和采集暂停状态 |
| `set_interval <ms>` | 持久化并应用采集间隔 | 只接受 100～60000 ms；保存成功后才更新运行值 |
| `get_config` | 查询当前采集间隔 | 返回正在使用的 `interval_ms` |
| `pause_collection` | 暂停产生新 Measurement | 不停止命令通道，也不会伪造 Outbox 已发送 |
| `resume_collection` | 恢复采集 | 后续 source 调用继续产生数据 |
| `get_storage_stats` | 查询存储统计 | 返回 measurements、pending、sent |
| `get_version` | 查询当前程序版本 | 返回 CMake 构建时注入的版本 |

`set_interval` 和 `get_storage_stats` 需要访问 SQLite，因此 handler 把带操作类型的请求
放进 storage control queue，由唯一拥有 Store 的 storage worker 执行。请求结果也携带
操作类型，handler 必须先核对类型，再决定如何解释结果和生成回复。

## 5. `get_version` 为什么这样实现

工程版本只有一个可信来源：

```cmake
project(edgevision_gateway VERSION 0.1.0 LANGUAGES C)
```

CMake 将 `${PROJECT_VERSION}` 作为 `EDGEVISION_VERSION` 宏分别传给 `edgevision_core`
和 `gateway`。因此：

- `gateway --version` 可以在不初始化硬件和数据库时打印版本；
- `gateway_handle_command()` 可以让本地和远程控制端查询同一个版本；
- 不需要在 C 源码的多个位置手工维护版本字符串。

共享 handler 中的行为是：

```text
command == "get_version"
    -> snprintf(reply, ..., "ok version=%s", EDGEVISION_VERSION)
```

D49 的独立实现链路为：

```text
入口收到 get_version
    -> gateway_execute_command()
    -> gateway_handle_command()
    -> get_version 分支读取 EDGEVISION_VERSION
    -> 生成 ok version=0.1.0
    -> 本地直接返回 / 远程包装成 JSON
```

## 6. 请求去重语义

远程请求按 `request_id + command` 判断：

- 两者都相同：认为是重复请求，不再次执行，直接发布缓存响应；
- `request_id` 相同但命令不同：返回 `request_id_conflict`，不执行新命令；
- 没有命中：执行命令，缓存生成的完整 JSON 响应，再发布。

这能避免 QoS 1 重投或控制端重试让写命令重复产生副作用。缓存是当前进程内的有限
缓存，不是跨重启的持久幂等记录。

## 7. 构建和使用

命令控制 worker 只有在 Storage 与 MQTT 同时启用时才进入正式服务构建：

```bash
cmake -S . -B build-service \
    -DCMAKE_BUILD_TYPE=Debug \
    -DEDGEVISION_ENABLE_STORAGE=ON \
    -DEDGEVISION_ENABLE_MQTT=ON \
    -DEDGEVISION_WARNINGS_AS_ERRORS=ON
cmake --build build-service --parallel
```

使用模拟源启动：

```bash
./build-service/gateway --source simulated ./gateway.db ./gateway.log
```

本地查询：

```bash
./build-service/gatewayctl get_version
# ok version=0.1.0
```

远程查询：

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 \
    -t edgevision/v1/devices/gateway-01/commands/response

mosquitto_pub -h 127.0.0.1 -p 1883 \
    -t edgevision/v1/devices/gateway-01/commands/request \
    -m '{"request_id":"version-check-1","command":"get_version"}'
```

## 8. D49 验证记录

2026-09-23 的最小验收结果：

- 开启 Storage、MQTT 和 warnings-as-errors 的全新 Debug 构建通过；
- 本地入口返回 `ok version=0.1.0`；
- MQTT 入口返回同一业务结果，并保留请求标识；
- 网关收到终止信号后干净退出；
- `get_version` 的核心实现和完整链路由学习者独立完成，导师只做事后审查、验证和一处
  尾随空白清理。

## 9. 复习检查

能够回答下面五个问题，就掌握了本功能的关键边界：

1. 为什么本地入口和 MQTT 入口不能各写一套命令业务？
2. `gateway_execute_command()` 为什么还需要一层 `command_mutex`？
3. 为什么 Store 操作交给 storage worker，而不是 MQTT 回调直接执行？
4. 为什么远程去重必须同时比较 `request_id` 和 `command`？
5. 为什么版本来自 CMake，而不是直接写死在 handler 中？

继续增加新命令时，通常只需要补充共享 handler 的业务分支，并让远程白名单接受该
命令；如果命令需要访问某个线程独占的资源，则还要通过相应队列交付，不能绕过资源
所有权。
