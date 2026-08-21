#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024

static int write_all(int fd, const void *buffer, size_t length)
{
    /* TCP 是字节流，一次 send 成功也可能只接收部分数据。 */
    const unsigned char *cursor = buffer;
    size_t written = 0;

    while (written < length)
    {
        ssize_t n = send(fd,
                         cursor + written,
                         length - written,
                         /* Prevent peer close from terminating the process. */
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

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    int listen_fd = -1;
    int client_fd = -1;
    unsigned char buffer[BUFFER_SIZE];

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    /* socket 只创建端点；随后还需要 bind 和 listen 才能接收连接。 */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }

    if (bind(listen_fd,
             (struct sockaddr *)&endpoint.sockaddr,
             sizeof(endpoint.sockaddr)) != 0)
    {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, 16) != 0)
    {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    printf("server listening on %s:%u\n", endpoint.address, endpoint.port);
    fflush(stdout);

    do {
        /* accept 返回新的已连接 fd；listen_fd 仍只负责监听。 */
        client_fd = accept(listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    if (client_fd < 0)
    {
        perror("accept");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    for (;;)
    {
        /* TCP 不保留消息边界：每次读到多少字节，就完整回写多少字节。 */
        ssize_t n = read(client_fd, buffer, sizeof(buffer));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("read");
            close(client_fd);
            close(listen_fd);
            return EXIT_FAILURE;
        }
        /* read 返回 0 表示对端正常关闭写方向，不是系统调用错误。 */
        if (n == 0)
            break;
        if (write_all(client_fd, buffer, (size_t)n) != 0)
        {
            perror("send");
            close(client_fd);
            close(listen_fd);
            return EXIT_FAILURE;
        }
    }

    close(client_fd);
    close(listen_fd);
    return EXIT_SUCCESS;
}
