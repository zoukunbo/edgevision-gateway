#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/time.h>

/* 防止客户端在异常服务端上无期阻塞。 */
static int set_io_timeout(int fd)
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


/* send 可能只写入部分数据；循环直到整条命令发送完成。 */
static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
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

/* 读取一行回复，并将换行符替换为 C 字符串结束符。 */
static int recv_line(int fd, char *buffer, size_t capacity)
{
    if (fd < 0 || buffer == NULL || capacity == 0)
    {
        errno = EINVAL;
        return -1;
    }

    size_t written = 0;

    while (1)
    {
        if (written + 1 >= capacity)
        {
            errno = EMSGSIZE;
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

        written += 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    char command[64];
    int n;

    if (argc == 2)
    {
        n = snprintf(command, sizeof(command), "%s", argv[1]);
    }
    else if (argc == 3 && strcmp(argv[1], "set_interval") == 0)
    {
        n = snprintf(command, sizeof(command), "%s %s", argv[1], argv[2]);
    }
    else
    {
        fprintf(stderr, "Usage: %s <command> | set_interval <ms>\n", argv[0]);
        return 1;
    }

    if (n < 0 || (size_t)n >= sizeof(command))
    {
        fprintf(stderr, "Command too long or formatting failed\n");
        return 1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
    {
        perror("socket");
        return 1;
    }

    // 2. 填写要连接的网关地址。
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s", "/tmp/edgevision-study.sock");

    // 3. 向网关发起连接。
    if (connect(fd, (const struct sockaddr *)&addr,
                sizeof(addr)) == -1) {
        perror("connect");
        close(fd);
        return 1;
    }

    puts("已连接到网关");

    // 下一步在这里发送命令，并接收回答。
    char reply[128];

    if (set_io_timeout(fd) == -1) {
        perror("set_io_timeout");
        close(fd);
        return 1;
    }

    if (send_all(fd, command, strlen(command)) != 0) {
        fprintf(stderr, "发送命令失败\n");
        close(fd);
        return 1;
    }

    if (send_all(fd, "\n", 1) != 0) {
        fprintf(stderr, "发送命令失败\n");
        close(fd);
        return 1;
    }

    if (recv_line(fd, reply, sizeof(reply)) != 0) {
        fprintf(stderr, "接收回答失败\n");
        close(fd);
        return 1;
    }

    puts(reply);

    close(fd);
    return 0;
}
#define _POSIX_C_SOURCE 200809L
