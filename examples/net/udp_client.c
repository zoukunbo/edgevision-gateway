#define _POSIX_C_SOURCE 200809L

#include "net_example_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUFFER_SIZE 1024

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    const char *message = "hello udp";
    const size_t message_length = strlen(message);
    unsigned char buffer[BUFFER_SIZE];
    const struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    int fd;

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    /* UDP 没有连接握手；设置接收超时可避免无服务端时永久阻塞。 */
    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout)) != 0)
    {
        perror("setsockopt");
        close(fd);
        return EXIT_FAILURE;
    }

    ssize_t sent = sendto(fd,
                          message,
                          message_length,
                          0,
                          (struct sockaddr *)&endpoint.sockaddr,
                          sizeof(endpoint.sockaddr));
    if (sent < 0 || (size_t)sent != message_length)
    {
        if (sent < 0)
            perror("sendto");
        else
            fprintf(stderr, "partial UDP datagram send\n");
        close(fd);
        return EXIT_FAILURE;
    }

    memset(&peer_addr, 0, sizeof(peer_addr));
    /* peer_addr_len 是输入/输出参数：输入容量，输出实际地址长度。 */
    ssize_t received = recvfrom(fd,
                                buffer,
                                sizeof(buffer),
                                0,
                                (struct sockaddr *)&peer_addr,
                                &peer_addr_len);
    if (received < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "recvfrom timeout\n");
        else
            perror("recvfrom");
        close(fd);
        return EXIT_FAILURE;
    }

    /* UDP is connectionless, so validate both sender and payload. */
    if (peer_addr.sin_family != AF_INET ||
        peer_addr.sin_port != endpoint.sockaddr.sin_port ||
        peer_addr.sin_addr.s_addr != endpoint.sockaddr.sin_addr.s_addr)
    {
        fprintf(stderr, "unexpected UDP echo source\n");
        close(fd);
        return EXIT_FAILURE;
    }
    if ((size_t)received != message_length ||
        memcmp(buffer, message, message_length) != 0)
    {
        fprintf(stderr, "UDP echo payload mismatch\n");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("echo: %.*s\n", (int)received, (char *)buffer);
    close(fd);
    return EXIT_SUCCESS;
}
