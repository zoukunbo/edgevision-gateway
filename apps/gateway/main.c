#define _POSIX_C_SOURCE 200809L

#include "address.h"
#include "async_logger.h"
#include "graceful_shutdown.h"
#include "socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static int recv_exact(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    size_t received = 0;

    /* TCP 是字节流，单次 recv 不保证返回完整应用消息。 */
    while (received < length)
    {
        ssize_t count = net_recv(fd, cursor + received, length - received);

        if (count <= 0)
            return -1;
        received += (size_t)count;
    }
    return 0;
}

static void close_if_open(int *fd)
{
    if (fd != NULL && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

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

static int run_network_smoke(async_logger_t *logger)
{
    static const char request[] = "edgevision-smoke-request";
    static const char response[] = "edgevision-smoke-response";
    net_address_t bind_address;
    net_address_t bound_address;
    net_address_t connect_address;
    socklen_t bound_length = sizeof(bound_address.addr);
    unsigned char buffer[64];
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

    /* client -> gateway ingress */
    if (net_send_all(client_fd, request, sizeof(request)) != 0 ||
        recv_exact(accepted_fd, buffer, sizeof(request)) != 0 ||
        memcmp(buffer, request, sizeof(request)) != 0)
    {
        errno = EPROTO;
        (void)log_smoke_error(logger, "request transfer");
        goto DONE;
    }

    /* gateway egress -> client：双向都验证才算整链通过。 */
    if (net_send_all(accepted_fd, response, sizeof(response)) != 0 ||
        recv_exact(client_fd, buffer, sizeof(response)) != 0 ||
        memcmp(buffer, response, sizeof(response)) != 0)
    {
        errno = EPROTO;
        (void)log_smoke_error(logger, "response transfer");
        goto DONE;
    }

    if (async_logger_log(logger,
                         LOG_LEVEL_INFO,
                         "gateway smoke passed: loopback TCP request/response") != 0)
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
