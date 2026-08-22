#ifndef EDGEVISION_NETWORK_CLIENT_H
#define EDGEVISION_NETWORK_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 客户端运行状态。业务层通常只关心 CONNECTED 与 STOPPED。 */
typedef enum {
    NETWORK_CLIENT_STOPPED = 0,
    NETWORK_CLIENT_CONNECTING,
    NETWORK_CLIENT_CONNECTED,
    NETWORK_CLIENT_BACKOFF,
    NETWORK_CLIENT_STOPPING
} network_client_state_t;

/** 收到服务器业务数据时调用；数据只在回调期间有效。 */
typedef void (*network_client_data_callback_t)(
    const void *data, size_t size, void *user_data);

/**
 * NetworkClient 配置。
 *
 * heartbeat_interval_ms 为 0 时关闭心跳。启用时 heartbeat_data 指向的内存
 * 必须至少保持到客户端停止。模块负责 TCP 连接与重连，业务协议由 on_data
 * 回调处理。
 */
typedef struct {
    /** 服务器 socket 地址及其实际长度。 */
    struct sockaddr_storage server_addr;
    socklen_t server_addrlen;

    /** 单次非阻塞连接的总超时预算，单位为毫秒。 */
    int connect_timeout_ms;

    /** 指数退避的初始值与基础值上限，单位为毫秒。 */
    uint32_t backoff_base_ms;
    uint32_t backoff_max_ms;

    /** 心跳周期；设为 0 时关闭心跳。 */
    uint32_t heartbeat_interval_ms;

    /** 心跳报文及长度；启用心跳时内存必须保持有效。 */
    const void *heartbeat_data;
    size_t heartbeat_size;

    /** 可选的业务数据回调及其调用者上下文。 */
    network_client_data_callback_t on_data;
    void *user_data;

    /** true 时向 stderr 输出连接、退避、心跳和停止日志。 */
    bool enable_logging;
} network_client_config_t;

/* 隐藏 fd、pthread 和状态机，防止业务代码破坏内部不变量。 */
typedef struct network_client network_client_t;

/** 创建客户端；失败返回 NULL 并设置 errno。 */
network_client_t *network_client_create(
    const network_client_config_t *config);

/** 启动后台线程；成功返回 0，失败返回 -1。 */
int network_client_start(network_client_t *client);

/** 通过 eventfd 请求停止，可立即打断 connect/backoff/connected 的 poll。 */
int network_client_request_stop(network_client_t *client);

/** 等待后台线程退出并回收 pthread 资源。 */
int network_client_join(network_client_t *client);

/**
 * 销毁客户端。若线程仍在运行，会先请求停止并 join。
 * network_client_destroy(NULL) 是安全的。
 */
void network_client_destroy(network_client_t *client);

#ifdef __cplusplus
}
#endif
#endif
