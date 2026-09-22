#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/time.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

int set_io_timeout(int fd)
{
    struct timeval timeout = {
        .tv_sec = 3,
        .tv_usec = 0
    };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1)
        return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) == -1)
        return -1;

    return 0;
}


int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {

        if (stop_requested) {
            errno = ECANCELED;
            return -1;
        }

        ssize_t n = send(fd, data + sent,
                         length - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n == -1 && errno == EINTR)
            continue;

        // 返回 0 表示没有进展，也作为失败，避免原地循环。
        return -1;
    }

    return 0;
}

int recv_line(int fd, char *buffer, size_t capacity)
{
    size_t written = 0;

    while (1)
    {
        if (stop_requested) {
            errno = ECANCELED;
            return -1;
        }

        ssize_t n = read(fd, buffer + written, 1);
        if (n < 0)
        {
           if (errno == EINTR)
           {
                continue;
           }
           return -1;
        }
        else if (n == 0)
        {
            return -1;
        }
        if (buffer[written] == '\n')
        {
            buffer[written] = '\0';
            break;
        }

        if (written == (capacity - 1))
        {
            return -1;
        }

        written += 1;
    }
    return 0;
}

typedef struct
{
    int interval_ms;
    int collected_count;
} gateway_status_t;

/**
 * command：调用者想做什么；strcmp(...) == 0 表示字符串内容相同。
 * interval_ms：本次查询使用的当前状态，不能在函数里写死成 1000。
 * reply、reply_size：回答写在哪里、最多能写多少字节。数组由调用者持有，函数只是借用。
 */
int handle_command(const char *command, const gateway_status_t *status,
                   char *reply, size_t reply_size)
{
    int n;

    if (strcmp(command, "status") == 0) 
    {
        n = snprintf(reply, reply_size,
                     "interval_ms=%d collected_count=%d",
                      status->interval_ms,
                      status->collected_count);
    } 
    else 
    {
        n = snprintf(reply, reply_size, "error=unknown_command");
    }

    if (n < 0 || (size_t)n >= reply_size)
        return -1;

    return 0;
}

int serve_client(int client_fd, const gateway_status_t *status)
{
    char command[64];
    char reply[128];

    if (set_io_timeout(client_fd) == -1)
    {
        perror("set_io_timeout");
        return -1;
    }

    if (recv_line(client_fd, command,sizeof(command)) == -1)
    {
        if (!stop_requested)
            fprintf(stderr, "接收命令失败\n");
        return -1;
    }

    if (handle_command(command, status, reply, sizeof(reply)) == -1)
    {
        return -1;
    }

    if (send_all(client_fd, reply, strlen(reply)) == -1)
    {
        perror("send");
        return -1;
    }

    if (send_all(client_fd, "\n", 1) == -1)
    {
        perror("send");
        return -1;
    }

    return 0;
}

int main(void)
{
    const char *socket_path = "/tmp/edgevision-study.sock";
    struct sigaction action = {0};
    action.sa_handler = request_stop;

    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    // 1. 创建本地流式 Socket。
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        perror("socket");
        return 1;
    }

    // 2. 准备本地地址。
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s", socket_path);

    // 3. 将 Socket 绑定到这个路径。
    if (bind(listen_fd, (const struct sockaddr *)&addr,
             sizeof(addr)) == -1) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    int result = 1;

    // 4. 开始监听；允许少量连接排队等待 accept。
    if (listen(listen_fd, 4) == -1) {
        perror("listen");
        goto cleanup;
    }

    puts("网关端已启动，等待客户端连接……");
    fflush(stdout);

    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags == -1 ||
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        goto cleanup;
    }

    
    // 下一步在这里接收命令、调用 handle_command、发送回答。
    gateway_status_t status = {
        .interval_ms = 1000,
        .collected_count = 12
    };
    

    struct pollfd listener = {
        .fd = listen_fd,
        .events = POLLIN
    };

    while (!stop_requested) {
        int ready = poll(&listener, 1, 200);

        if (ready == -1) {
            if (errno == EINTR)
                continue;

            perror("poll");
            goto cleanup;
        }

        if (stop_requested)
            break;

        if (ready == 0)
            continue;

        if (listener.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "监听 Socket 异常\n");
            goto cleanup;
        }

        if (!(listener.revents & POLLIN))
            continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR ||
                errno == EAGAIN ||
                errno == EWOULDBLOCK)
                continue;

            perror("accept");
            goto cleanup;
        }

        if (!stop_requested) {
            int rc = serve_client(client_fd, &status);
            if (rc != 0 && !stop_requested)
                fprintf(stderr, "本次客户端请求处理失败\n");
        }

        close(client_fd);
    }

    result = 0;

cleanup:
    close(listen_fd);

    // bind 成功创建的路径，由本进程在退出时清理。
    if (unlink(socket_path) == -1) {
        perror("unlink");
        result = 1;
    }

    return result;
}