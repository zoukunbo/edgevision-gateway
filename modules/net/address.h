#ifndef NET_ADDRESS_H
#define NET_ADDRESS_H

#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

typedef struct
{
    struct sockaddr_in addr;
} net_address_t;

/*
 * 根据 IPv4 字符串和端口构造地址。
 *
 * 成功：
 *     return 0
 *
 * 失败：
 *     return -1
 */
int net_address_ipv4(
    net_address_t *address,
    const char *ip,
    uint16_t port);

/* 只读地址，给 bind/connect/sendto。
*/
const struct sockaddr *net_address_sockaddr(
    const net_address_t *address);

socklen_t net_address_length(
    const net_address_t *address);

/* 可写地址，给 recvfrom/accept 这种需要把地址填回来的 API。
*/
struct sockaddr *net_address_sockaddr_mut(
    net_address_t *address);

#endif