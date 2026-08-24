#ifndef NET_EPOLL_ECHO_SERVER_H
#define NET_EPOLL_ECHO_SERVER_H

#include "address.h"

/*
 * 启动一个基于 epoll 的非阻塞 TCP echo 诊断服务。
 * 收到的数据会原样返回；函数在事件循环中持续运行，致命错误返回 -1。
 */
int net_epoll_echo_server_run(const net_address_t *address, int backlog);

#endif
