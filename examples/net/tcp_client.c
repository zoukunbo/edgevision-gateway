#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *cursor = buffer;
    size_t written = 0;

    while (written < length)
    {
        ssize_t n = send(fd,
                         cursor + written,
                         length - written,
                         MSG_NOSIGNAL);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

/*
 * 当前协议已知回显长度，所以循环读满 length。
 * 真实协议通常通过固定头、长度字段、分隔符或 EOF 确定消息边界。
 */
static int read_exact(int fd, void *buffer, size_t length)
{
    unsigned char *cursor = buffer;
    size_t received = 0;

    while (received < length)
    {
        ssize_t n = read(fd, cursor + received, length - received);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        received += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    const char *message = "hello";
    const size_t message_length = strlen(message);
    unsigned char buffer[32];
    int fd;

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    /* connect 完成 TCP 握手；无监听服务时通常返回 ECONNREFUSED。 */
    if (connect(fd,
                (struct sockaddr *)&endpoint.sockaddr,
                sizeof(endpoint.sockaddr)) != 0)
    {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("connected to %s:%u\n", endpoint.address, endpoint.port);
    if (write_all(fd, message, message_length) != 0)
    {
        perror("send");
        close(fd);
        return EXIT_FAILURE;
    }
    if (read_exact(fd, buffer, message_length) != 0)
    {
        fprintf(stderr, "failed to read complete echo\n");
        close(fd);
        return EXIT_FAILURE;
    }

    /* A same-length response is not sufficient; verify every byte. */
    if (memcmp(buffer, message, message_length) != 0)
    {
        fprintf(stderr, "echo payload mismatch\n");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("echo: %.*s\n", (int)message_length, (char *)buffer);
    close(fd);
    return EXIT_SUCCESS;
}
