#define _POSIX_C_SOURCE 200809L

#include "address.h"
#include "async_logger.h"
#include "frame.h"
#include "graceful_shutdown.h"
#include "socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Gateway 命令行用法：
 *   ./gateway [log-path]
 *       以常驻模式运行，收到 SIGINT/SIGTERM 后安全退出。
 *   ./gateway --smoke [log-path]
 *       执行一次本地 TCP + 协议帧整链自检并退出。
 *
 * 不传日志路径时，常驻模式使用 gateway.log，smoke 模式使用
 * gateway-smoke.log。
 */

/**
 * @brief 等待进程收到 SIGINT 或 SIGTERM 停止请求。
 *
 * 每 100 ms 检查一次 graceful_shutdown_requested()。nanosleep() 被普通
 * 信号中断时会继续睡完剩余时间；如果停止信号已经到达则立即返回。
 *
 * @return 0 表示收到停止请求；-1 表示 nanosleep() 发生非 EINTR 错误，
 *         errno 保留具体原因。
 *
 * @note 仅由 main() 的常驻模式调用；smoke 模式不会进入此函数。
 */
static int wait_for_stop(void)
{
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000L};

    while (!graceful_shutdown_requested())
    {
        struct timespec remaining = interval;

        while (nanosleep(&remaining, &remaining) != 0)
        {
            if (errno == EINTR)
            {
                if (graceful_shutdown_requested())
                    return 0;
                continue;
            }
            return -1;
        }
    }
    return 0;
}

typedef struct
{
    int fd;                 /* 已连接的 TCP socket。 */
    frame_parser_t parser;  /* 此连接独占的增量帧解析状态。 */
} gateway_connection_t;

/**
 * @brief 初始化一条 Gateway TCP 连接的协议解析上下文。
 *
 * @param connection 调用方分配的连接上下文，不能为空。
 * @param fd 已连接的 TCP socket 文件描述符；所有权仍属于调用方。
 *
 * @note 初始化后调用 gateway_connection_recv_frame() 接收应用层帧；关闭
 *       socket 时仍应调用 close_if_open()，本函数不会复制或关闭 fd。
 */
static void gateway_connection_init(gateway_connection_t *connection, int fd)
{
    connection->fd = fd;
    frame_parser_init(&connection->parser);
}

/**
 * @brief 从 TCP 字节流中阻塞读取并还原一个完整应用层 payload。
 *
 * 函数会先调用 frame_parser_next() 排空连接中已经缓存的数据；数据不足时
 * 再调用 net_recv()，并把收到的字节交给 frame_parser_feed()。坏长度或坏
 * CRC 会被解析器拒绝并自动重同步，函数继续等待下一帧。
 *
 * @param connection 已由 gateway_connection_init() 初始化的连接上下文。
 * @param payload 接收 payload 的输出缓冲区，容量至少为 FRAME_MAX_PAYLOAD。
 * @param payload_size 输出实际 payload 字节数，不能为空。
 * @return 0 表示成功得到一帧；-1 表示连接关闭、recv 失败或解析缓冲区溢出。
 *
 * @note 典型调用：先初始化 connection，再在业务循环中反复调用本函数。
 */
static int gateway_connection_recv_frame(gateway_connection_t *connection,
                                         unsigned char *payload,
                                         size_t *payload_size)
{
    unsigned char recv_buffer[7];

    for (;;)
    {
        frame_result_t parse_result;
        ssize_t received;

        /* 先排空已缓存帧，支持一次 recv() 带回多个应用帧。 */
        parse_result = frame_parser_next(&connection->parser,
                                         payload,
                                         payload_size);
        if (parse_result == FRAME_READY)
            return 0;
        if (parse_result == FRAME_INVALID)
            continue;

        /* 小接收块让 smoke 稳定覆盖半帧；生产代码可使用更大的块。 */
        received = net_recv(connection->fd,
                            recv_buffer,
                            sizeof(recv_buffer));
        if (received <= 0)
            return -1;
        if (frame_parser_feed(&connection->parser,
                              recv_buffer,
                              (size_t)received) != FRAME_READY)
        {
            errno = EMSGSIZE;
            return -1;
        }
    }
}

/**
 * @brief 编码一个 payload 并通过 TCP 完整发送协议帧。
 *
 * @param fd 已连接的 TCP socket 文件描述符。
 * @param payload 待发送的业务数据；payload_size 大于 0 时不能为空。
 * @param payload_size 业务数据字节数，不能超过 FRAME_MAX_PAYLOAD。
 * @return 0 表示完整发送成功；-1 表示编码或 net_send_all() 失败。
 *
 * @note 编码格式为 MAGIC + LEN + PAYLOAD + CRC16；调用方无需自行拼帧。
 */
static int send_frame(int fd,
                      const unsigned char *payload,
                      size_t payload_size)
{
    unsigned char frame[FRAME_MAX_SIZE];
    size_t frame_size = 0;

    if (frame_encode(payload,
                     payload_size,
                     frame,
                     sizeof(frame),
                     &frame_size) != FRAME_READY)
    {
        errno = EMSGSIZE;
        return -1;
    }
    return net_send_all(fd, frame, frame_size);
}

/**
 * @brief 安全关闭一个可能已经打开的文件描述符。
 *
 * @param fd 文件描述符变量的地址；允许为 NULL。仅当 *fd >= 0 时调用
 *           close()，随后把 *fd 设为 -1，防止清理路径重复关闭。
 */
static void close_if_open(int *fd)
{
    if (fd != NULL && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

/**
 * @brief 记录并打印 smoke 测试某一步的 errno 错误。
 *
 * @param logger 已初始化的异步日志器。
 * @param step 失败步骤名称，例如 "connect" 或 "framed Measurement ingress"。
 * @return 固定返回 -1，便于调用方在错误分支中直接传播失败。
 *
 * @note 调用前应先设置 errno；函数同时写异步日志和标准错误。
 */
static int log_smoke_error(async_logger_t *logger, const char *step)
{
    char message[256];

    snprintf(message,
             sizeof(message),
             "gateway smoke failed at %s: %s",
             step,
             strerror(errno));
    (void)async_logger_log(logger, LOG_LEVEL_ERROR, message);
    fprintf(stderr, "%s\n", message);
    return -1;
}

/**
 * @brief 执行 Gateway 的本地 TCP + 协议帧双向整链自检。
 *
 * 测试在 127.0.0.1 的内核分配端口上建立客户端和服务端连接：客户端发送
 * Measurement 帧，Gateway 增量解析后再返回 MeasurementAck 帧。收发双方都
 * 校验 payload 内容，并把成功帧计数写入日志。
 *
 * @param logger 已初始化的异步日志器，用于记录成功证据和失败步骤。
 * @return 0 表示请求与响应整链通过；-1 表示任一步失败。
 *
 * @note 由 main() 在 --smoke 模式调用，也可通过 `ctest -R gateway_smoke`
 *       间接执行。
 */
static int run_network_smoke(async_logger_t *logger)
{
    static const unsigned char request[] =
        "Measurement{sequence=32,temperature_mC=25375}";
    static const unsigned char response[] =
        "MeasurementAck{sequence=32,status=accepted}";
    net_address_t bind_address;
    net_address_t bound_address;
    net_address_t connect_address;
    gateway_connection_t gateway_connection;
    gateway_connection_t client_connection;
    socklen_t bound_length = sizeof(bound_address.addr);
    unsigned char payload[FRAME_MAX_PAYLOAD];
    size_t payload_size = 0;
    char success_message[256];
    int listen_fd = -1;
    int client_fd = -1;
    int accepted_fd = -1;
    int result = -1;

    /* 端口 0 让内核选择空闲端口，避免 smoke test 与本机服务冲突。 */
    if (net_address_ipv4(&bind_address, "127.0.0.1", 0) != 0)
    {
        (void)log_smoke_error(logger, "address setup");
        goto DONE;
    }

    listen_fd = net_tcp_socket();
    if (listen_fd < 0)
    {
        (void)log_smoke_error(logger, "socket");
        goto DONE;
    }
    if (net_bind(listen_fd, &bind_address) != 0)
    {
        (void)log_smoke_error(logger, "bind");
        goto DONE;
    }
    if (net_listen(listen_fd, 4) != 0)
    {
        (void)log_smoke_error(logger, "listen");
        goto DONE;
    }

    /* getsockname 取得内核为端口 0 实际分配的端口。 */
    if (getsockname(listen_fd,
                    net_address_sockaddr_mut(&bound_address),
                    &bound_length) != 0 ||
        net_address_ipv4(&connect_address,
                         "127.0.0.1",
                         ntohs(bound_address.addr.sin_port)) != 0)
    {
        (void)log_smoke_error(logger, "resolve bound port");
        goto DONE;
    }

    client_fd = net_tcp_socket();
    if (client_fd < 0 || net_connect(client_fd, &connect_address) != 0)
    {
        (void)log_smoke_error(logger, "connect");
        goto DONE;
    }
    accepted_fd = net_accept(listen_fd);
    if (accepted_fd < 0)
    {
        (void)log_smoke_error(logger, "accept");
        goto DONE;
    }

    gateway_connection_init(&gateway_connection, accepted_fd);
    gateway_connection_init(&client_connection, client_fd);

    /* client -> gateway ingress：Measurement 先编码，再从 TCP 字节流增量恢复。 */
    if (send_frame(client_fd, request, sizeof(request) - 1u) != 0 ||
        gateway_connection_recv_frame(&gateway_connection,
                                      payload,
                                      &payload_size) != 0 ||
        payload_size != sizeof(request) - 1u ||
        memcmp(payload, request, payload_size) != 0)
    {
        errno = EPROTO;
        (void)log_smoke_error(logger, "framed Measurement ingress");
        goto DONE;
    }

    /* gateway egress -> client：响应同样经过编码、TCP 和增量解析。 */
    if (send_frame(accepted_fd, response, sizeof(response) - 1u) != 0 ||
        gateway_connection_recv_frame(&client_connection,
                                      payload,
                                      &payload_size) != 0 ||
        payload_size != sizeof(response) - 1u ||
        memcmp(payload, response, payload_size) != 0)
    {
        errno = EPROTO;
        (void)log_smoke_error(logger, "framed Measurement response");
        goto DONE;
    }

    snprintf(success_message,
             sizeof(success_message),
             "gateway smoke passed: framed Measurement loopback; "
             "ingress_frames=%zu response_frames=%zu",
             gateway_connection.parser.stats.frame_ok,
             client_connection.parser.stats.frame_ok);
    if (async_logger_log(logger, LOG_LEVEL_INFO, success_message) != 0)
    {
        fprintf(stderr, "failed to submit smoke success log\n");
        goto DONE;
    }
    result = 0;

DONE:
    close_if_open(&accepted_fd);
    close_if_open(&client_fd);
    close_if_open(&listen_fd);
    return result;
}

/**
 * @brief 解析 Gateway 命令行参数。
 *
 * 支持 `gateway [log-path]` 和 `gateway --smoke [log-path]` 两种形式。
 *
 * @param argc main() 收到的参数数量。
 * @param argv main() 收到的参数数组，argv[0] 用于打印程序名。
 * @param smoke_mode 输出运行模式：1 表示 smoke，0 表示常驻模式。
 * @param log_path 输出日志路径指针，指向 argv 中的字符串或默认常量。
 * @return 0 表示参数合法；-1 表示输出参数为空或命令行格式不支持。
 */
static int parse_arguments(int argc,
                           char **argv,
                           int *smoke_mode,
                           const char **log_path)
{
    if (smoke_mode == NULL || log_path == NULL)
        return -1;

    *smoke_mode = 0;
    *log_path = "gateway.log";

    if (argc == 1)
        return 0;
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0)
    {
        *smoke_mode = 1;
        *log_path = "gateway-smoke.log";
        return 0;
    }
    if (argc == 2)
    {
        *log_path = argv[1];
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0)
    {
        *smoke_mode = 1;
        *log_path = argv[2];
        return 0;
    }

    fprintf(stderr, "usage: %s [log-path]\n", argv[0]);
    fprintf(stderr, "       %s --smoke [log-path]\n", argv[0]);
    return -1;
}

/**
 * @brief Gateway 程序入口，负责参数、信号、日志和运行模式的完整生命周期。
 *
 * @param argc 命令行参数数量。
 * @param argv 命令行参数数组。
 * @return EXIT_SUCCESS 表示正常退出或 smoke 通过；EXIT_FAILURE 表示初始化、
 *         运行或清理阶段失败。
 *
 * @note 常驻模式调用链：parse_arguments() -> 日志/信号初始化 ->
 *       wait_for_stop() -> 日志排空。smoke 模式把 wait_for_stop() 替换为
 *       run_network_smoke()。
 */
int main(int argc, char **argv)
{
    const char *log_path;
    async_logger_t logger;
    int smoke_mode;
    int result = EXIT_FAILURE;

    if (parse_arguments(argc, argv, &smoke_mode, &log_path) != 0)
        return EXIT_FAILURE;
    if (graceful_shutdown_install() != 0)
    {
        perror("graceful_shutdown_install");
        return EXIT_FAILURE;
    }
    if (async_logger_init(&logger, log_path, 256) != 0)
    {
        fprintf(stderr, "async_logger_init failed for %s\n", log_path);
        return EXIT_FAILURE;
    }
    if (async_logger_log(&logger, LOG_LEVEL_INFO, "gateway started") != 0)
    {
        fprintf(stderr, "failed to submit startup log\n");
        goto SHUTDOWN;
    }

    if (smoke_mode)
    {
        result = run_network_smoke(&logger) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        goto SHUTDOWN;
    }

    printf("gateway running; send SIGINT or SIGTERM to stop\n");
    fflush(stdout);
    if (wait_for_stop() != 0)
    {
        fprintf(stderr, "wait_for_stop failed: %s\n", strerror(errno));
        goto SHUTDOWN;
    }
    if (async_logger_log(&logger,
                         LOG_LEVEL_INFO,
                         "shutdown requested; draining logger") != 0)
    {
        fprintf(stderr, "failed to submit shutdown log\n");
        goto SHUTDOWN;
    }
    result = EXIT_SUCCESS;

SHUTDOWN:
    if (async_logger_shutdown(&logger) != 0)
    {
        fprintf(stderr, "async_logger_shutdown failed\n");
        result = EXIT_FAILURE;
    }
    if (async_logger_destroy(&logger) != 0)
    {
        fprintf(stderr, "async_logger_destroy failed\n");
        result = EXIT_FAILURE;
    }

    if (smoke_mode)
        printf("%s\n", result == EXIT_SUCCESS ? "SMOKE_PASS" : "SMOKE_FAIL");
    else if (result == EXIT_SUCCESS)
        printf("gateway stopped cleanly\n");
    return result;
}
