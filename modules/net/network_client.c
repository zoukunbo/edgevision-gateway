#include "network_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

struct network_client {
    network_client_state_t state;
    int fd;
    int wake_fd;
    pthread_t thread;
    bool thread_started;
    uint32_t attempt;
    unsigned int rng_state;
    network_client_config_t config;
};

enum step_result {
    STEP_ERROR = -1,
    STEP_STOP = 0,
    STEP_OK = 1,
    STEP_PENDING = 2
};

/**
 * 按配置输出学习用状态日志。正式项目可在此接入统一日志模块。
 *
 * @param client 当前客户端，必须有效。
 * @param format printf 风格的格式字符串。
 */
static void client_log(const network_client_t *client, const char *format, ...)
{
    if (!client->config.enable_logging) {
        return;
    }

    va_list args;
    va_start(args, format);
    fputs("[network] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

/**
 * 获取单调时钟毫秒值。CLOCK_MONOTONIC 不受修改系统日期或 NTP 校时影响，
 * 所以连接超时、退避和心跳都使用它计算 deadline。
 *
 * @return 成功返回单调时钟毫秒值；失败返回 -1，并设置 errno。
 */
static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/**
 * 关闭当前 socket，并恢复 fd=-1 这一“未持有 socket”的统一状态。
 *
 * @param client 当前客户端，必须有效。
 */
static void close_socket(network_client_t *client)
{
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
}

/**
 * 设置非阻塞模式。先用 F_GETFL 读取旧标志，再写回
 * old_flags | O_NONBLOCK，避免覆盖 fd 原有属性。
 *
 * @param fd 需要设置为非阻塞模式的文件描述符。
 * @return 成功返回 0；失败返回 -1，并设置 errno。
 */
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * 消费一次 eventfd 停止通知。
 *
 * eventfd 内部保存的是 64 位计数器，所以 read 必须使用 uint64_t 并完整
 * 读取 8 字节。读取成功后计数器恢复为 0，wake_fd 不再保持可读状态。
 * 这一步相当于网络线程正式接收并消费了主线程发出的停止事件。
 *
 * @param client 当前客户端，wake_fd 必须有效。
 * @return 成功返回 0；失败返回 -1，并设置 errno。
 */
static int consume_stop_event(network_client_t *client)
{
    uint64_t value;
    ssize_t size;

    do {
        size = read(client->wake_fd, &value, sizeof(value));
    } while (size < 0 && errno == EINTR);

    if (size == (ssize_t)sizeof(value)) {
        return 0;
    }
    if (size >= 0) {
        errno = EIO;
    }
    return -1;
}

/**
 * 创建非阻塞 socket 并发起 connect。
 *
 * connect=0：立即成功；EINPROGRESS：握手由内核继续，需要 poll；
 * 其他错误：本次连接立即失败。
 *
 * @param client 当前未持有 TCP socket 的客户端。
 * @return STEP_OK 表示立即成功；STEP_PENDING 表示握手进行中；
 *         STEP_ERROR 表示连接立即失败。
 */
static int connect_begin(network_client_t *client)
{
    const struct sockaddr *address =
        (const struct sockaddr *)&client->config.server_addr;

    client->fd = socket(address->sa_family, SOCK_STREAM, 0);
    if (client->fd < 0) {
        return STEP_ERROR;
    }

    if (set_nonblocking(client->fd) < 0) {
        int saved_errno = errno;
        close_socket(client);
        errno = saved_errno;
        return STEP_ERROR;
    }

    client->state = NETWORK_CLIENT_CONNECTING;
    if (connect(client->fd, address, client->config.server_addrlen) == 0) {
        client->state = NETWORK_CLIENT_CONNECTED;
        return STEP_OK;
    }
    if (errno == EINPROGRESS) {
        return STEP_PENDING;
    }

    int saved_errno = errno;
    close_socket(client);
    errno = saved_errno;
    return STEP_ERROR;
}

/**
 * 等待非阻塞连接完成，并同时监听停止 eventfd。
 *
 * POLLOUT 只表示连接“有结果”，不代表成功；最终必须读取 SO_ERROR，
 * SO_ERROR=0 才是连接成功，否则其值就是真实连接错误。
 *
 * @param client 处于 CONNECTING 状态且持有有效 socket 的客户端。
 * @return STEP_OK 表示连接成功；STEP_STOP 表示收到停止通知；
 *         STEP_ERROR 表示连接失败或超时。
 */
static int connect_wait(network_client_t *client)
{
    /*
     * 一次 poll 同时等待两个来源：
     *
     * pfds[0]：正在进行非阻塞 connect 的 TCP socket。
     *          POLLOUT 表示“连接过程已经产生结果”，但结果可能成功也可能失败。
     *
     * pfds[1]：主线程用于请求停止的 eventfd。
     *          POLLIN 表示主线程已经写入停止通知，网络线程应立即退出等待。
     *
     * events 是应用希望监听的事件；revents 是 poll 返回时由内核填写的
     * 实际事件。每次 poll 前都把 revents 清零，避免误读上一次结果。
     */
    struct pollfd pfds[2] = {
        { client->fd, POLLOUT, 0 },
        { client->wake_fd, POLLIN, 0 }
    };
    int64_t now = monotonic_ms();
    if (now < 0) {
        return STEP_ERROR;
    }
    int64_t deadline = now + client->config.connect_timeout_ms;

    for (;;) {
        now = monotonic_ms();
        if (now < 0) {
            return STEP_ERROR;
        }
        int64_t remaining = deadline - now;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return STEP_ERROR;
        }

        pfds[0].revents = 0;
        pfds[1].revents = 0;
        /*
         * poll 只负责等待 fd 状态变化：
         *   rc > 0：至少一个 fd 发生事件，需要查看各自的 revents；
         *   rc == 0：本次连接超过剩余时间；
         *   rc < 0：poll 自身调用失败。
         *
         * 它既不执行 TCP 握手，也不直接保证连接成功。
         */
        int rc = poll(pfds, 2, (int)remaining);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return STEP_ERROR;
        }
        if (rc == 0) {
            errno = ETIMEDOUT;
            return STEP_ERROR;
        }
        /*
         * 优先处理停止通知。即使 socket 同时出现连接结果，用户已经要求停止时，
         * 也不再进入 CONNECTED，直接结束后台线程。
         */
        if (pfds[1].revents & POLLIN) {
            return consume_stop_event(client) == 0
                ? STEP_STOP : STEP_ERROR;
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return STEP_ERROR;
        }
        if (pfds[0].revents & POLLNVAL) {
            errno = EBADF;
            return STEP_ERROR;
        }
        /*
         * 这些事件都只能说明 TCP 连接“有结果了”。此处跳出等待循环，
         * 下一步统一通过 SO_ERROR 查询最终是成功还是失败。
         */
        if (pfds[0].revents & (POLLOUT | POLLERR | POLLHUP)) {
            break;
        }
    }

    int socket_error = 0;
    socklen_t size = sizeof(socket_error);
    /*
     * getsockopt 调用成功只表示成功读取了 socket_error：
     *   socket_error == 0：TCP 连接成功；
     *   socket_error != 0：TCP 连接失败，该值是真正的错误码。
     */
    if (getsockopt(client->fd, SOL_SOCKET, SO_ERROR,
                   &socket_error, &size) < 0) {
        return STEP_ERROR;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return STEP_ERROR;
    }

    client->state = NETWORK_CLIENT_CONNECTED;
    return STEP_OK;
}

/**
 * 计算 base*2^(attempt-1)，并对溢出和 max 上限做保护。
 *
 * @param attempt 连续失败次数，从 1 开始。
 * @param base_ms 第一次失败的基础等待毫秒数。
 * @param max_ms 基础退避的最大毫秒数。
 * @return 不超过 max_ms 的基础退避时间。
 */
static uint32_t backoff_base_delay(
    uint32_t attempt, uint32_t base_ms, uint32_t max_ms)
{
    uint32_t delay = base_ms;
    for (uint32_t i = 1; i < attempt && delay < max_ms; ++i) {
        if (delay > max_ms / 2) {
            return max_ms;
        }
        delay *= 2;
    }
    return delay > max_ms ? max_ms : delay;
}

/**
 * 返回 0 到基础退避 25% 的随机抖动，避免大量设备同时重连。
 *
 * @param delay_ms 尚未加入抖动的基础退避时间。
 * @param rng_state rand_r 使用并更新的客户端私有随机状态。
 * @return 需要追加的抖动毫秒数。
 */
static uint32_t backoff_jitter(
    uint32_t delay_ms, unsigned int *rng_state)
{
    uint32_t window = delay_ms / 4;
    return window == 0
        ? 0
        : (uint32_t)rand_r(rng_state) % (window + 1);
}

/**
 * 等待重连时间。等待对象仍然是 eventfd，所以用户停止时不必等完整个
 * backoff timeout。
 *
 * @param client 已连接失败且 attempt 至少为 1 的客户端。
 * @return STEP_OK 表示退避结束；STEP_STOP 表示收到停止通知；
 *         STEP_ERROR 表示等待过程失败。
 */
static int backoff_wait(network_client_t *client)
{
    uint32_t delay = backoff_base_delay(
        client->attempt,
        client->config.backoff_base_ms,
        client->config.backoff_max_ms);
    uint32_t jitter = backoff_jitter(delay, &client->rng_state);
    int timeout_ms = (int)((uint64_t)delay + jitter);
    struct pollfd pfd = { client->wake_fd, POLLIN, 0 };

    client->state = NETWORK_CLIENT_BACKOFF;
    client_log(client, "backoff=%d ms", timeout_ms);

    for (;;) {
        pfd.revents = 0;
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0) {
            return STEP_OK;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return STEP_ERROR;
        }
        if (pfd.revents & POLLIN) {
            return consume_stop_event(client) == 0
                ? STEP_STOP : STEP_ERROR;
        }
        errno = EIO;
        return STEP_ERROR;
    }
}

/**
 * 发送一个小型心跳。MSG_NOSIGNAL 避免对端断开时 SIGPIPE 杀死进程。
 * 非阻塞发送遇到 EAGAIN 时跳过本轮；其他错误交给状态机触发重连。
 *
 * @param client 处于 CONNECTED 状态且已启用心跳的客户端。
 * @return 已发送或可安全跳过返回 0；连接不可继续使用返回 -1。
 */
static int send_heartbeat(network_client_t *client)
{
    ssize_t sent = send(
        client->fd,
        client->config.heartbeat_data,
        client->config.heartbeat_size,
        MSG_NOSIGNAL);

    if (sent == (ssize_t)client->config.heartbeat_size) {
        client_log(client, "heartbeat sent");
        return 0;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        client_log(client, "heartbeat skipped: send buffer busy");
        return 0;
    }
    if (sent >= 0) {
        errno = EIO;
    }
    return -1;
}

/**
 * CONNECTED 状态的事件循环。
 *
 * pfds[0] 是 TCP socket：recv>0 为业务数据，recv=0 为对端正常关闭；
 * pfds[1] 是 eventfd：主线程请求停止；poll 超时则发送定时心跳。
 *
 * @param client 处于 CONNECTED 状态的客户端。
 * @return STEP_OK 表示断线后需要重连；STEP_STOP 表示本地停止；
 *         STEP_ERROR 表示读写或系统调用错误。
 */
static int connected_loop(network_client_t *client)
{
    struct pollfd pfds[2] = {
        { client->fd, POLLIN, 0 },
        { client->wake_fd, POLLIN, 0 }
    };
    int64_t next_heartbeat = -1;

    if (client->config.heartbeat_interval_ms > 0) {
        int64_t now = monotonic_ms();
        if (now < 0) {
            return STEP_ERROR;
        }
        next_heartbeat =
            now + (int64_t)client->config.heartbeat_interval_ms;
    }

    for (;;) {
        int timeout_ms = -1;
        if (next_heartbeat >= 0) {
            int64_t now = monotonic_ms();
            if (now < 0) {
                return STEP_ERROR;
            }
            int64_t remaining = next_heartbeat - now;
            timeout_ms = remaining <= 0 ? 0 :
                (remaining > INT_MAX ? INT_MAX : (int)remaining);
        }

        pfds[0].revents = 0;
        pfds[1].revents = 0;
        int rc = poll(pfds, 2, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return STEP_ERROR;
        }

        if (pfds[1].revents & POLLIN) {
            return consume_stop_event(client) == 0
                ? STEP_STOP : STEP_ERROR;
        }
        if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EIO;
            return STEP_ERROR;
        }
        if (pfds[0].revents & POLLNVAL) {
            errno = EBADF;
            return STEP_ERROR;
        }

        /*
         * 对 TCP socket 而言，“可读”有两种核心含义：
         * 1. 服务端发来了业务数据；
         * 2. 服务端发送 FIN，正常关闭了连接。
         *
         * poll 只能告诉我们 socket 可读，最终必须通过 recv 返回值区分。
         */
        if (pfds[0].revents & POLLIN) {
            unsigned char buffer[1024];
            ssize_t size = recv(client->fd, buffer, sizeof(buffer), 0);

            /*
             * recv > 0：真正收到业务数据，交给上层回调解析；
             * recv == 0：对端正常关闭，这不是“暂时没有数据”；
             * recv < 0：读取失败。非阻塞 socket 的 EAGAIN/EWOULDBLOCK
             *           才表示当前暂时没有数据。
             */
            if (size > 0) {
                if (client->config.on_data != NULL) {
                    client->config.on_data(
                        buffer, (size_t)size,
                        client->config.user_data);
                }
            } else if (size == 0) {
                /*
                 * 对端关闭后，旧 TCP socket 不能重新 connect。
                 * 返回工作线程，由它关闭旧 fd、执行退避并创建新 socket。
                 */
                client_log(client, "peer closed connection");
                return STEP_OK;
            } else if (errno != EAGAIN &&
                       errno != EWOULDBLOCK &&
                       errno != EINTR) {
                return STEP_ERROR;
            }
        }

        /*
         * POLLIN 和 POLLHUP 可能同时出现，例如服务端发送最后一批数据后关闭。
         * 因此代码先执行上面的 recv，尽量交付残留数据，再处理挂断/错误。
         */
        if (pfds[0].revents & (POLLERR | POLLHUP)) {
            client_log(client, "socket hangup/error");
            errno = ECONNRESET;
            return STEP_OK;
        }

        if (next_heartbeat >= 0) {
            int64_t now = monotonic_ms();
            if (now < 0) {
                return STEP_ERROR;
            }
            if (now >= next_heartbeat) {
                if (send_heartbeat(client) < 0) {
                    return STEP_ERROR;
                }
                next_heartbeat =
                    now + (int64_t)client->config.heartbeat_interval_ms;
            }
        }
    }
}

/**
 * 后台线程的主状态机：
 * CONNECTING -> CONNECTED -> 断线 -> BACKOFF -> CONNECTING。
 * 任意等待阶段收到 eventfd 后转为 STOPPING，最终统一清理为 STOPPED。
 *
 * @param argument 指向已初始化的 network_client_t。
 * @return 始终返回 NULL。
 */
static void *network_client_worker(void *argument)
{
    network_client_t *client = argument;

    for (;;) {
        /*
         * 阶段 1：发起连接。
         *
         * connect_begin 只负责创建非阻塞 socket 并调用 connect；
         * STEP_PENDING 表示 TCP 握手仍由内核进行，需要 connect_wait 使用
         * poll + SO_ERROR 取得最终结果。
         */
        client_log(client, "connecting, attempt=%u", client->attempt + 1);
        int rc = connect_begin(client);
        if (rc == STEP_PENDING) {
            rc = connect_wait(client);
        }

        /* connect_wait 收到 eventfd，直接走统一退出路径。 */
        if (rc == STEP_STOP) {
            break;
        }

        /*
         * 阶段 2：连接成功后的事件循环。
         *
         * connected_loop 同时负责业务数据、对端关闭、心跳和停止通知。
         * 成功建立连接后把 attempt 清零，因为连续失败已经结束。
         */
        if (rc == STEP_OK) {
            client->attempt = 0;
            client_log(client, "connected");
            rc = connected_loop(client);
            close_socket(client);
            if (rc == STEP_STOP) {
                break;
            }
            if (rc == STEP_OK) {
                client_log(client, "connection closed; reconnecting");
            } else {
                client_log(client, "connection error: %s", strerror(errno));
            }
        } else {
            int saved_errno = errno;
            close_socket(client);
            client_log(client, "connect failed: %s",
                       strerror(saved_errno));
        }

        /*
         * 阶段 3：连接失败或已建立的连接丢失。
         *
         * 增加连续失败次数后执行指数退避。backoff_wait 返回 STEP_OK
         * 表示等待结束；循环自然回到顶部，再次调用 connect_begin。
         * 因此自动重连的本质就是“退避结束后继续下一轮 for 循环”。
         */
        if (client->attempt < UINT32_MAX) {
            ++client->attempt;
        }
        rc = backoff_wait(client);
        if (rc != STEP_OK) {
            break;
        }
    }

    /*
     * 阶段 4：统一退出。
     *
     * 无论停止发生在连接、已连接还是退避阶段，都在这里关闭 socket，
     * 最终设置 STOPPED，避免多个错误分支分别维护资源清理代码。
     */
    client->state = NETWORK_CLIENT_STOPPING;
    close_socket(client);
    client->state = NETWORK_CLIENT_STOPPED;
    client_log(client, "stopped");
    return NULL;
}

/**
 * 只在模块入口验证配置。内部函数依赖这些已建立的不变量，避免在每个函数
 * 重复写一整套参数判断。
 *
 * @param config 调用者提供的客户端配置。
 * @return 配置有效返回 0；无效返回 -1，并设置 errno=EINVAL。
 */
static int validate_config(const network_client_config_t *config)
{
    if (config == NULL ||
        config->server_addrlen == 0 ||
        config->server_addrlen > sizeof(config->server_addr) ||
        config->connect_timeout_ms <= 0 ||
        config->backoff_base_ms == 0 ||
        config->backoff_max_ms < config->backoff_base_ms ||
        (config->heartbeat_interval_ms > 0 &&
         (config->heartbeat_data == NULL ||
          config->heartbeat_size == 0 ||
          config->heartbeat_size > (size_t)SSIZE_MAX))) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/**
 * 创建客户端资源，但不启动线程、不连接服务器。
 *
 * 此处集中验证外部配置，并创建停止通知所需的 eventfd。成功返回后，
 * client 归调用者所有，必须最终交给 network_client_destroy()。
 *
 * @param config 服务器地址、超时、退避、心跳与回调配置。
 * @return 成功返回客户端指针；失败返回 NULL，并设置 errno。
 */
network_client_t *network_client_create(
    const network_client_config_t *config)
{
    if (validate_config(config) < 0) {
        return NULL;
    }

    /* calloc(nmemb, size) 分配nmemb个元素，每个元素size字节 总大小 = nmemb * size */
    /* calloc会把分配出来的内存全部置0；malloc不会清零，内存是随机脏数据 */
    network_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }

    client->fd = -1;

    /*
     * eventfd 是控制线程与网络线程之间的唤醒通道：
     * - 初始计数为 0，此时不可读；
     * - 主线程写入 uint64_t 后计数增加，wake_fd 变为可读；
     * - 网络线程正在执行的 poll 会立即返回。
     *
     * EFD_NONBLOCK 防止意外 read/write 阻塞；
     * EFD_CLOEXEC 防止 exec 新程序后泄漏该 fd。
     */
    client->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    client->state = NETWORK_CLIENT_STOPPED;
    client->rng_state =
        (unsigned int)time(NULL) ^ (unsigned int)getpid();
    client->config = *config;

    if (client->wake_fd < 0) {
        int saved_errno = errno;
        free(client);
        errno = saved_errno;
        return NULL;
    }
    return client;
}

/**
 * 创建后台网络线程。
 *
 * 返回 0 只表示 pthread_create 成功，不代表 TCP 已连接成功；连接结果由
 * 后台线程异步处理。成功后 thread_started=true，必须 join 后才能释放对象。
 *
 * @param client 由 network_client_create 创建且尚未启动的客户端。
 * @return 成功返回 0；失败返回 -1，并设置 errno。
 */
int network_client_start(network_client_t *client)
{
    if (client == NULL || client->thread_started) {
        errno = EINVAL;
        return -1;
    }

    int rc = pthread_create(
        &client->thread, NULL, network_client_worker, client);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    client->thread_started = true;
    return 0;
}

/**
 * 异步请求网络线程停止。
 *
 * 本函数只写 eventfd，让阻塞在 poll 中的线程立即醒来；返回时后台线程
 * 可能仍在关闭 socket，因此调用者不能立即释放 client，之后必须 join。
 *
 * @param client 已启动且尚未 join 的客户端。
 * @return 通知成功返回 0；失败返回 -1，并设置 errno。
 */
int network_client_request_stop(network_client_t *client)
{
    if (client == NULL || !client->thread_started) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 写入数值 1，使 eventfd 的 64 位计数器增加。
     * 这次 write 不直接终止线程；它负责让监听 wake_fd 的 poll 立即醒来，
     * 网络线程随后自行清理 socket 并退出状态机。
     */
    uint64_t value = 1;
    ssize_t size;
    do {
        size = write(client->wake_fd, &value, sizeof(value));
    } while (size < 0 && errno == EINTR);

    if (size == (ssize_t)sizeof(value) ||
        (size < 0 && errno == EAGAIN)) {
        return 0;
    }
    if (size >= 0) {
        errno = EIO;
    }
    return -1;
}

/**
 * 等待后台线程真正退出。
 *
 * join 既回收 pthread 资源，也是生命周期同步点：成功返回后可以确认
 * 网络线程不会再访问 client，此时才允许 destroy。
 *
 * @param client 已启动且尚未 join 的客户端。
 * @return 成功返回 0；失败返回 -1，并设置 errno。
 */
int network_client_join(network_client_t *client)
{
    if (client == NULL || !client->thread_started) {
        errno = EINVAL;
        return -1;
    }

    int rc = pthread_join(client->thread, NULL);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    client->thread_started = false;
    return 0;
}

/**
 * 关闭剩余 fd 并释放客户端内存。
 *
 * 正常调用顺序是 request_stop -> join -> destroy。这里仍提供兜底：
 * 如果线程尚未 join，先尝试通知并回收线程，避免释放仍被线程访问的对象。
 *
 * @param client 待销毁的客户端，可为 NULL。
 */
void network_client_destroy(network_client_t *client)
{
    if (client == NULL) {
        return;
    }
    if (client->thread_started) {
        /*
         * 停止或 join 失败时不能继续 free，否则后台线程可能访问已释放内存。
         * 此时宁可保留资源供调用者排查，也不能制造 use-after-free。
         */
        if (network_client_request_stop(client) < 0 ||
            network_client_join(client) < 0) {
            return;
        }
    }
    close_socket(client);
    if (client->wake_fd >= 0) {
        close(client->wake_fd);
    }
    free(client);
}
