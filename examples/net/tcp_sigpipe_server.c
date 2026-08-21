#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    int listen_fd;
    int client_fd;

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

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

    printf("waiting on %s:%u\n", endpoint.address, endpoint.port);
    fflush(stdout);
    do {
        client_fd = accept(listen_fd, NULL, NULL);
    } while (client_fd < 0 && errno == EINTR);

    if (client_fd < 0)
    {
        perror("accept");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /*
     * 不读取数据就关闭连接。根据 FIN/RST 到达时机，客户端可能要到后续
     * send 才观察到 EPIPE 或 ECONNRESET，这正是该实验要验证的行为。
     */
    close(client_fd);
    close(listen_fd);
    return EXIT_SUCCESS;
}
