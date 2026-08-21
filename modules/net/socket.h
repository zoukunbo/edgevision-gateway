#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <sys/types.h>

#include "address.h"

/*
 * 创建 IPv4 TCP socket
 *
 * 成功：
 *     返回 fd >= 0
 *
 * 失败：
 *     返回 -1
 */
int net_tcp_socket(void);


/*
 * 创建 IPv4 UDP socket
 */
int net_udp_socket(void);


/*
 * 把 socket 绑定到 address
 *
 * 成功：0
 * 失败：-1
 */
int net_bind(
    int fd,
    const net_address_t *address);


/*
 * TCP socket 开始监听
 *
 * 成功：0
 * 失败：-1
 */
int net_listen(
    int fd,
    int backlog);

/*
 * 接受一个 TCP 客户端连接
 *
 * 成功：
 *     返回 client fd >= 0
 *
 * 失败：
 *     返回 -1
 */
int net_accept(int listen_fd);

/*
 * 连接到远端地址。
 *
 * 成功：0
 * 失败：-1
 */
int net_connect(
    int fd,
    const net_address_t *address);


/*
 * 完整发送 length 个字节。
 *
 * 成功：
 *     return 0
 *
 * 失败：
 *     return -1
 */
int net_send_all(
    int fd,
    const void *buffer,
    size_t length);


/*
 * 从 TCP socket 接收最多 length 个字节。
 *
 * 返回：
 *
 * > 0
 *     实际接收到的字节数
 *
 * == 0
 *     对端正常关闭连接
 *
 * == -1
 *     接收失败
 */
ssize_t net_recv(
    int fd,
    void *buffer,
    size_t length);

/*
* UDP
* → 发送这一条 datagram
* → 返回实际 sendto() 结果
* buffer 收到的数据
* source 谁发来的
*/
ssize_t net_sendto(
    int fd,
    const void *buffer,
    size_t length,
    const net_address_t *address);
/* 这条 Datagram 是谁发来的？
*/
ssize_t net_recvfrom(
    int fd,
    void *buffer,
    size_t length,
    net_address_t *source);


#endif