# epoll 多连接事件循环复习

## 一句话模型

`epoll_ctl()` 告诉内核“关心哪些 FD 的哪些事件”；`epoll_wait()` 把本次已就绪的事件写进 `ready[]`，返回值 `count` 是有效元素数量。

```text
注册集合（长期保存）                  ready[]（一次等待的快照）
listen_fd: EPOLLIN  ─┐               ready[0] = {listen_fd, EPOLLIN}
client 5 : EPOLLIN   ├─ 内核筛选 ──> ready[1] = {5, EPOLLIN}
client 6 : EPOLLIN  ─┘               count = 2
```

循环只允许访问 `ready[0]` 到 `ready[count - 1]`。数组其余位置不是本轮有效事件。

## 最容易混淆的地方：listen_fd 就绪

`listen_fd` 的 `EPOLLIN` 表示 accept 队列非空，不表示只有一个客户端：

```text
listen_fd 的 accept 队列：[A] [B] [C]
ready[]：                 [listen_fd]     ← 仍然只出现一次
```

因此监听 socket 必须设置非阻塞，并反复 `accept()`，直到返回 `EAGAIN`，这才说明队列已排空。

这里 `for (;;)` 与 `while (1)` 效果相同。前者常用来表达“由内部条件退出的永久事件循环”。

## ready[] 中不同 FD 的处理

```text
ready[i].data.fd == listen_fd
    └─ EPOLLIN：accept 到 EAGAIN

ready[i].data.fd 是 client_fd
    ├─ EPOLLIN ：recv；0 表示对端关闭，EAGAIN 表示暂时读空
    ├─ EPOLLOUT：继续发送 output 中尚未发完的数据
    └─ ERR/HUP ：读取剩余数据或清理连接
```

多个客户端通过不同 FD 区分。FD 关闭后可能被内核复用，因此程序还需维护连接状态，保存该 FD 尚未发完的数据和发送进度。

## epoll 与非阻塞的关系

epoll 只负责通知，不会自动让 `read()`、`send()`、`accept()` 非阻塞。若其中一个 FD 仍以阻塞方式卡住，整个单线程事件循环便无法服务其他连接。

- `n > 0`：实际处理了 n 字节，不保证一次完成全部操作。
- `n == 0`：对端正常关闭（适用于 `read/recv`）。
- `EINTR`：被信号中断，可以重试。
- `EAGAIN/EWOULDBLOCK`：当前暂时不能继续；保存状态并等待下一次事件。

## 为什么 EPOLLOUT 按需开启

TCP socket 大部分时间都可写。如果始终监听 `EPOLLOUT`，`epoll_wait()` 会频繁返回，造成无效 CPU 消耗。

```text
EPOLLIN 收到数据
  → 保存 output_length 和 output_sent
  → MOD 为 EPOLLOUT
  → send 可能只发一部分
  → EAGAIN 时保留 output_sent
  → 下次 EPOLLOUT 从断点继续
  → 全部发完后 MOD 回 EPOLLIN
```

## 当前采用 LT 模式

代码未指定 `EPOLLET`，因此使用默认 LT（水平触发）：只要就绪条件仍成立，下一次等待还会通知。仍然应把 `accept()` 执行到 `EAGAIN`，并正确处理非阻塞读写的 `EINTR/EAGAIN`。

## 复习检查表

- `count`：本轮 `ready[]` 中有效事件的数量。
- `ready[]`：本轮就绪事件快照，不是全部连接。
- 一个 listen_fd 就绪：可能对应多个等待连接。
- 区分客户端：读取 `ready[i].data.fd` 并查找对应连接状态。
- `EAGAIN`：暂时不能继续，不是断线。
- `output_sent`：记录部分发送进度。
- `EPOLLOUT`：仅在确有待发数据时关注。
- 关闭连接：从 epoll 删除、清除连接状态、关闭 FD。
