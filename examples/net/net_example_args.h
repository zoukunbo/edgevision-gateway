#ifndef EDGEVISION_NET_EXAMPLE_ARGS_H
#define EDGEVISION_NET_EXAMPLE_ARGS_H

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    /* sockaddr_in 是 socket API 使用的 IPv4 二进制地址结构。 */
    struct sockaddr_in sockaddr;
    const char *address;
    uint16_t port;
} net_example_endpoint_t;

/*
 * 解析可选参数：<IPv4地址> <端口>。
 * argc==1 使用全部默认值；argc==2 只替换地址；argc==3 同时替换地址和端口。
 * 这里统一做校验，避免六个示例对非法参数产生不同的行为。
 */
static int net_example_parse_endpoint(int argc,
                                      char **argv,
                                      const char *default_address,
                                      uint16_t default_port,
                                      net_example_endpoint_t *endpoint)
{
    char *end = NULL;
    unsigned long parsed_port = default_port;

    if (endpoint == NULL || default_address == NULL || argc < 1 || argc > 3)
    {
        if (argc > 0)
            fprintf(stderr, "usage: %s [IPv4-address] [port]\n", argv[0]);
        return -1;
    }

    endpoint->address = argc >= 2 ? argv[1] : default_address;
    if (argc == 3)
    {
        /* strtoul 配合 errno 和 end 指针才能区分 0、非法字符及数值溢出。 */
        errno = 0;
        parsed_port = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' ||
            parsed_port == 0 || parsed_port > UINT16_MAX)
        {
            fprintf(stderr, "invalid port: %s\n", argv[2]);
            return -1;
        }
    }

    memset(&endpoint->sockaddr, 0, sizeof(endpoint->sockaddr));
    endpoint->sockaddr.sin_family = AF_INET;
    /* 端口在协议头中使用网络字节序（大端），因此必须调用 htons。 */
    endpoint->sockaddr.sin_port = htons((uint16_t)parsed_port);

    /* inet_pton returning 0 means a syntactically invalid IPv4 string. */
    int rc = inet_pton(AF_INET,
                       endpoint->address,
                       &endpoint->sockaddr.sin_addr);
    if (rc == 0)
    {
        fprintf(stderr, "invalid IPv4 address: %s\n", endpoint->address);
        return -1;
    }
    if (rc < 0)
    {
        perror("inet_pton");
        return -1;
    }

    endpoint->port = (uint16_t)parsed_port;
    return 0;
}

#endif
