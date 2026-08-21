#define _POSIX_C_SOURCE 200809L

#include "address.h"
#include "socket.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static int test_address(void)
{
    net_address_t address;
    struct in_addr expected_ip;

    /* 公共接口必须拒绝空指针和非法 IPv4 文本。 */
    if (net_address_ipv4(NULL, "127.0.0.1", 8080) == 0 ||
        net_address_ipv4(&address, NULL, 8080) == 0 ||
        net_address_ipv4(&address, "not-an-ip", 8080) == 0)
    {
        fprintf(stderr, "address invalid-input test failed\n");
        return -1;
    }

    if (net_address_ipv4(&address, "127.0.0.1", 8080) != 0 ||
        inet_pton(AF_INET, "127.0.0.1", &expected_ip) != 1)
    {
        fprintf(stderr, "address construction failed\n");
        return -1;
    }

    /* sockaddr 内保存网络字节序，比较端口时要用 ntohs 转回主机字节序。 */
    if (address.addr.sin_family != AF_INET ||
        ntohs(address.addr.sin_port) != 8080 ||
        address.addr.sin_addr.s_addr != expected_ip.s_addr ||
        net_address_sockaddr(&address) == NULL ||
        net_address_length(&address) != sizeof(struct sockaddr_in) ||
        net_address_sockaddr(NULL) != NULL ||
        net_address_length(NULL) != 0)
    {
        fprintf(stderr, "address representation test failed\n");
        return -1;
    }

    return 0;
}

static int verify_socket_type(int fd, int expected_type)
{
    int actual_type = 0;
    socklen_t length = sizeof(actual_type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &actual_type, &length) != 0)
    {
        perror("getsockopt SO_TYPE");
        return -1;
    }
    return actual_type == expected_type ? 0 : -1;
}

static int test_socket_creation(void)
{
    int tcp_fd = net_tcp_socket();
    int udp_fd = net_udp_socket();
    int result = 0;

    if (tcp_fd < 0 || udp_fd < 0 ||
        verify_socket_type(tcp_fd, SOCK_STREAM) != 0 ||
        verify_socket_type(udp_fd, SOCK_DGRAM) != 0)
    {
        fprintf(stderr, "socket type test failed\n");
        result = -1;
    }

    if (tcp_fd >= 0)
        close(tcp_fd);
    if (udp_fd >= 0)
        close(udp_fd);
    return result;
}

static int test_bind_and_listen(void)
{
    net_address_t address;
    struct sockaddr_in actual_address;
    socklen_t actual_length = sizeof(actual_address);
    int fd = -1;
    int result = -1;

    /* 端口 0 表示让内核选择一个当前可用的临时端口。 */
    if (net_address_ipv4(&address, "127.0.0.1", 0) != 0)
        goto DONE;

    fd = net_tcp_socket();
    if (fd < 0 || net_bind(fd, &address) != 0 || net_listen(fd, 4) != 0)
        goto DONE;

    if (getsockname(fd,
                    (struct sockaddr *)&actual_address,
                    &actual_length) != 0 ||
        ntohs(actual_address.sin_port) == 0)
    {
        goto DONE;
    }

    result = 0;

DONE:
    if (fd >= 0)
        close(fd);
    if (result != 0)
        fprintf(stderr, "bind/listen test failed\n");
    return result;
}

static int test_invalid_socket_arguments(void)
{
    net_address_t address;

    if (net_address_ipv4(&address, "127.0.0.1", 0) != 0 ||
        net_bind(-1, &address) == 0 ||
        net_bind(0, NULL) == 0 ||
        net_listen(-1, 4) == 0 ||
        net_listen(0, 0) == 0 ||
        net_accept(-1) >= 0)
    {
        fprintf(stderr, "socket invalid-input test failed\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    if (test_address() != 0 ||
        test_socket_creation() != 0 ||
        test_bind_and_listen() != 0 ||
        test_invalid_socket_arguments() != 0)
    {
        return EXIT_FAILURE;
    }

    printf("net module tests passed\n");
    return EXIT_SUCCESS;
}
