# D30 NetworkClient 复习与面试手册

## 1. 今天为什么学它

嵌入式 Linux 设备通常需要长期连接云端、网关或局域网服务。真实网络会出现服务未启动、连接超时、路由中断、服务端重启和程序退出等情况，因此客户端不能只写一次阻塞式 `connect()`。

今天的目标是能够完成这条工程主线：

```text
非阻塞连接
→ 有超时地等待连接结果
→ 连接后收发数据并发送心跳
→ 发现断线
→ 带抖动地退避重连
→ 可以被主线程立即停止
```

对应源码：

- `modules/net/network_client.h`：公共配置和生命周期 API
- `modules/net/network_client.c`：线程、状态机和网络机制
- `examples/NetworkClient.c`：最小使用示例

---

## 2. 五分钟复习版

### 2.1 状态机

```text
                    连接成功
CONNECTING ─────────────────────→ CONNECTED
     ▲                               │
     │                               │ recv==0 / 读写错误 / HUP
     │                               ▼
     └──────── 退避结束 ───────── BACKOFF

CONNECTING / CONNECTED / BACKOFF
                 │ eventfd
                 ▼
             STOPPING
                 │
                 ▼
              STOPPED
```

### 2.2 四个核心内部函数

| 函数 | 唯一核心职责 |
|---|---|
| `connect_begin()` | 创建非阻塞 socket 并发起 `connect()` |
| `connect_wait()` | 用 `poll()+SO_ERROR` 获取异步连接结果 |
| `connected_loop()` | 处理数据、对端关闭、心跳和停止通知 |
| `backoff_wait()` | 控制重连频率，同时允许停止立即打断 |

`network_client_worker()` 负责把四个函数串成循环状态机。

### 2.3 五个公共生命周期 API

```text
create  → 准备对象和 eventfd
start   → 创建后台网络线程
stop    → 写 eventfd，请求线程退出
join    → 确认线程已经退出
destroy → 关闭 fd 并释放内存
```

必须记住：

> `request_stop()` 只是发出异步通知，`join()` 才能确认线程不再访问 client。

---

## 3. 非阻塞连接

### 3.1 为什么设置 O_NONBLOCK

阻塞式 `connect()` 可能让线程长时间停住，无法及时响应退出。设置非阻塞后，暂时无法完成的操作会立即返回，等待过程由应用使用 `poll()` 控制。

```c
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

必须先读取旧 flags，再执行 `flags | O_NONBLOCK`，否则可能覆盖 fd 原有属性。

### 3.2 connect 的三个核心结果

```text
connect == 0
→ 立即连接成功

connect == -1 && errno == EINPROGRESS
→ 不是失败；TCP 三次握手仍由内核继续

connect == -1 && errno != EINPROGRESS
→ 本次连接立即失败
```

---

## 4. poll 与 SO_ERROR

连接进行中时同时等待：

```text
socket fd：POLLOUT，连接过程产生结果
wake_fd：POLLIN，主线程请求停止
```

`poll()` 返回值：

| 返回值 | 含义 |
|---:|---|
| `> 0` | 至少一个 fd 发生事件，检查 `revents` |
| `== 0` | 超时 |
| `< 0` | poll 调用失败；`EINTR` 时通常重新等待 |

`events` 是应用希望监听的事件，`revents` 是内核返回的实际事件。

### 为什么 POLLOUT 不等于成功

成功和失败都可能使连接过程结束并唤醒 `poll()`。因此必须读取 socket 保存的异步错误：

```c
int socket_error = 0;
socklen_t size = sizeof(socket_error);

getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size);
```

```text
socket_error == 0
→ TCP 连接成功

socket_error != 0
→ TCP 连接失败，socket_error 是真实错误码
```

不能直接读取旧的 `errno`，因为成功返回的 `poll()` 没有为这次异步连接设置 errno。

---

## 5. eventfd 可中断停止

普通变量可以表示“需要停止”，但不能唤醒阻塞在 `poll()` 中的线程。

`eventfd` 同时可以：

- 被主线程 `write()`
- 被网络线程 `read()`
- 被 `poll()` 监听

流程：

```text
主线程 write(wake_fd)
→ eventfd 计数器增加
→ wake_fd 变为可读
→ 网络线程 poll 立即返回
→ 网络线程读取 uint64_t
→ 清理并退出
```

eventfd 必须使用完整的 8 字节 `uint64_t` 读写。

同一个 wake_fd 被用于：

- 连接等待
- 已连接事件循环
- 退避等待

所以无论网络线程处在哪个等待阶段，都可以及时停止。

---

## 6. 对端关闭和业务数据

连接成功后，`poll()` 监听 socket 的 `POLLIN`。

```c
ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
```

| recv 结果 | 含义 |
|---:|---|
| `> 0` | 收到业务数据，交给上层回调 |
| `== 0` | 对端正常关闭连接 |
| `< 0, EAGAIN` | 非阻塞 socket 当前暂时没有数据 |
| 其他负数 | 读取错误，连接通常需要重建 |

必须记住：

> `recv()==0` 是 TCP 对端正常关闭的核心判断，不是“暂时没有数据”。

对端关闭后，旧 socket 不能重新 `connect()`。应关闭旧 fd、执行退避、创建新 socket。

`POLLIN` 和 `POLLHUP` 可能同时出现，所以先调用 `recv()` 处理最后一批数据，再处理挂断。

---

## 7. 指数退避与随机抖动

失败后立即无限重试会造成 CPU 空转、日志刷屏和服务器重连风暴。

基础退避：

```text
delay = min(base × 2^(attempt-1), max)
```

当前配置示例：

```text
500 ms → 1000 ms → 2000 ms → 2000 ms
```

随机抖动范围为基础退避的 0～25%：

```text
500～625 ms
1000～1250 ms
2000～2500 ms
```

`backoff_max_ms` 限制的是基础值，最终时间还会包含抖动。

退避使用 `poll(wake_fd, timeout)`，而不是 `sleep()`，因为 eventfd 到达时必须立即停止。

---

## 8. 心跳

当前最小实现定时发送：

```text
PING\n
```

目的：

- 为长时间空闲的连接产生网络活动
- 在 `send()` 返回连接错误时辅助发现断线

发送使用 `MSG_NOSIGNAL`，避免向断开的 socket 写数据时，`SIGPIPE` 默认终止整个进程。

```text
send 成功
→ 数据被本机 TCP 发送缓冲区接受

EAGAIN/EWOULDBLOCK
→ 发送缓冲区暂时繁忙，跳过本轮

EPIPE/ECONNRESET 等
→ 连接不可继续使用，进入重连
```

能力边界：

> 发送 PING 成功不等于服务端应用正常。完整应用层心跳还需要服务端回复 PONG，并在多个周期未收到 PONG 时主动重连。

---

## 9. 日志定位

| 最后日志 | 含义和检查方向 |
|---|---|
| `connecting` 后长时间无结果 | 检查连接 timeout 和 `connect_wait()` |
| `Connection refused` | 地址可达，但目标端口没有服务监听 |
| 持续 `backoff` | 服务持续不可用，退避保护正在工作 |
| `connected` 后立即关闭 | 检查服务端行为和协议格式 |
| `peer closed connection` | `recv()==0`，服务端正常关闭 |
| `socket hangup/error` | socket 出现 HUP/ERR |
| 没有 `stopped` | 检查 eventfd 消费和线程退出路径 |
| stop 后主程序不退出 | 检查网络线程卡点和 `pthread_join()` |

---

## 10. 高频错误

### 错误一：把 POLLOUT 当作连接成功

必须继续检查 `SO_ERROR`。

### 错误二：把 recv==0 当作暂时没数据

`recv==0` 表示对端关闭；暂时没数据是负数加 `EAGAIN/EWOULDBLOCK`。

### 错误三：stop 后立即 free

正确顺序：

```text
request_stop → join → destroy
```

否则可能发生 use-after-free。

### 错误四：用 sleep 做退避

`sleep()` 不能被 eventfd 唤醒，会拖慢程序退出。

### 错误五：断线后复用旧 socket

TCP 连接失效后应关闭旧 fd，重新创建 socket。

### 错误六：心跳发送未处理 SIGPIPE

Linux 下使用 `MSG_NOSIGNAL`，或者在进程层统一处理 SIGPIPE。

---

## 11. 面试问答

### Q1：如何实现带超时的非阻塞 connect？

先通过 `fcntl()` 设置 `O_NONBLOCK`，调用 `connect()`。若返回 `EINPROGRESS`，使用 `poll(POLLOUT)` 等待，但可写不等于成功，必须再通过 `getsockopt(SOL_SOCKET, SO_ERROR)` 判断最终结果。超时使用 `CLOCK_MONOTONIC` deadline 控制。

### Q2：为什么 poll 返回 POLLOUT 后还要检查 SO_ERROR？

成功连接和连接失败都可能使 socket 出现可写或错误事件。POLLOUT 只表示连接过程已有结果，SO_ERROR 为 0 才表示成功。

### Q3：如何让阻塞在 poll 中的线程及时退出？

创建 eventfd，把它与网络 socket 一起加入 poll。控制线程向 eventfd 写入 uint64_t 后，poll 立即返回，网络线程读取通知、清理并退出。

### Q4：如何检测 TCP 对端正常关闭？

socket 会变为可读，调用 `recv()` 返回 0。客户端应关闭旧 socket，并进入重连流程。

### Q5：为什么重连要使用指数退避和抖动？

指数退避避免持续失败时 CPU 空转和高频请求；上限避免等待无限增长；随机抖动分散大量设备的重连时间，避免惊群和重连风暴。

### Q6：request_stop 和 join 有什么区别？

request_stop 是异步通知，只负责唤醒并请求线程退出；join 是同步等待，确认线程已经返回并回收线程资源。只有 join 成功后才能安全释放共享对象。

### Q7：心跳发送成功是否代表服务端正常？

不完全代表。它只说明本机内核接受了待发送数据。完整存活检测需要 PING/PONG，并对 PONG 设置超时。

### Q8：如何描述整个客户端设计？

使用后台线程维护状态机。非阻塞 connect 后通过 poll 和 SO_ERROR 获取结果；连接成功后同时处理 socket、心跳和停止 eventfd；断线后关闭旧 socket，执行带抖动的指数退避，再重新连接；退出时 stop、join、destroy。

---

## 12. 编译与最小验证

构建：

```bash
cmake -S . -B build
cmake --build build --target network_client_demo
```

无服务端场景：

```bash
./build/network_client_demo
```

预期观察：

```text
connecting
connect failed
backoff
connecting
...
stopped
```

成功连接场景可先启动本地服务：

```bash
python3 -m http.server 8080 --bind 127.0.0.1
```

再运行客户端，观察 `connected` 和 `heartbeat sent`。

最小验收：

1. 项目无警告编译通过。
2. 服务不可用时按退避节奏重连，不高频空转。
3. 服务可用时进入 CONNECTED 并发送心跳。
4. 服务端关闭后能够检测并重连。
5. 主线程请求停止后能够及时 join 并退出。

---

## 13. 当前阶段不需要深挖

以下内容属于后续按项目需求学习：

- PING/PONG 应答超时协议
- TCP Keepalive 全部内核参数
- TLS 非阻塞握手
- 多地址并行连接
- 高并发 `epoll`
- 完整发送队列和半包处理
- 无锁状态查询和高级性能优化

复习时优先保证能够解释：

```text
connect_begin
→ connect_wait
→ connected_loop
→ backoff_wait
→ worker 再次循环
```
