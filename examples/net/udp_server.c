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

int main(int argc, char **argv)
{
    net_example_endpoint_t endpoint;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    unsigned char buffer[BUFFER_SIZE];
    int fd;
    ssize_t received;

    if (net_example_parse_endpoint(argc,
                                   argv,
                                   DEFAULT_ADDRESS,
                                   DEFAULT_PORT,
                                   &endpoint) != 0)
        return EXIT_FAILURE;

    /* SOCK_DGRAM 保留报文边界：一次 sendto 对应一个 UDP 数据报。 */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (bind(fd,
             (struct sockaddr *)&endpoint.sockaddr,
             sizeof(endpoint.sockaddr)) != 0)
    {
        perror("bind");
        close(fd);
        return EXIT_FAILURE;
    }

    printf("UDP server bound to %s:%u\n", endpoint.address, endpoint.port);
    fflush(stdout);
    do {
        /* recvfrom 同时返回报文内容和发送方地址，后者用于原路回显。 */
        received = recvfrom(fd,
                            buffer,
                            sizeof(buffer),
                            0,
                            (struct sockaddr *)&client_addr,
                            &client_addr_len);
    } while (received < 0 && errno == EINTR);

    if (received < 0)
    {
        perror("recvfrom");
        close(fd);
        return EXIT_FAILURE;
    }

    ssize_t sent = sendto(fd,
                          buffer,
                          (size_t)received,
                          0,
                          (struct sockaddr *)&client_addr,
                          client_addr_len);
    /* UDP sends one complete datagram or reports an error. */
    if (sent < 0 || sent != received)
    {
        if (sent < 0)
            perror("sendto");
        else
            fprintf(stderr, "partial UDP datagram send\n");
        close(fd);
        return EXIT_FAILURE;
    }

    close(fd);
    return EXIT_SUCCESS;
}
