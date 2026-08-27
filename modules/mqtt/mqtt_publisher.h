#ifndef EDGEVISION_MQTT_PUBLISHER_H
#define EDGEVISION_MQTT_PUBLISHER_H

#include "measurement.h"

#include <stdbool.h>

/*
 * MQTT 测量数据发布器。
 *
 * 发布器在内部维护 Mosquitto 网络线程和自动重连状态。调用方先 create，
 * 再 start 和 wait_connected；不再使用时调用 destroy。publish 使用 QoS 1，
 * 只有收到对应的 PUBACK 才返回成功。
 *
 * publish、wait_connected 和 is_connected 可由业务线程调用；create、start、
 * stop、destroy 等生命周期操作应由管理线程串行执行。destroy 前必须确保
 * 其他线程不再访问该实例。
 */

/* 隐藏实现细节，实例只能通过本头文件提供的接口操作。 */
typedef struct mqtt_publisher mqtt_publisher_t;

/* 创建发布器所需的连接及重连配置。所有字符串都会在创建时复制。 */
typedef struct
{
    /* Broker 主机名或 IP 地址，非空，最长 255 个字符。 */
    const char *host;
    /* Broker 端口，取值范围为 1～65535。 */
    int port;
    /* MQTT 客户端 ID，非空，最长 127 个字符。 */
    const char *client_id;
    /* 主题前缀，非空，最长 127 个字符。 */
    const char *topic_prefix;
    /* MQTT Keep Alive 秒数，不能小于 5。 */
    int keepalive_seconds;
    /* 首次重连等待秒数。 */
    unsigned int reconnect_delay_seconds;
    /* 最大重连等待秒数，不能小于首次重连等待时间。 */
    unsigned int reconnect_delay_max_seconds;
} mqtt_publisher_config_t;

/* 发布器操作的统一返回值；create 通过指针或 NULL 表示结果。 */
typedef enum
{
    /* 操作成功。 */
    MQTT_PUBLISHER_OK = 0,
    /* 指针为空、数值越界或字符串过长。 */
    MQTT_PUBLISHER_INVALID_ARGUMENT = -1,
    /* 内存分配失败。 */
    MQTT_PUBLISHER_NO_MEMORY = -2,
    /* 系统调用、线程库或 Mosquitto 调用失败。 */
    MQTT_PUBLISHER_LIBRARY_ERROR = -3,
    /* 发布器当前未连接 Broker。 */
    MQTT_PUBLISHER_NOT_CONNECTED = -4,
    /* 等待连接或 PUBACK 超时。 */
    MQTT_PUBLISHER_TIMEOUT = -5,
    /* 测量数据无法序列化为 JSON。 */
    MQTT_PUBLISHER_SERIALIZATION_ERROR = -6
} mqtt_publisher_result_t;

/*
 * 创建处于 STOPPED 状态的发布器。
 * 成功返回实例指针；配置无效、内存不足或 Mosquitto 初始化失败时返回 NULL。
 */
mqtt_publisher_t *mqtt_publisher_create(
    const mqtt_publisher_config_t *config);

/* 异步启动连接和网络线程；实际连接结果通过 wait_connected 等待。 */
mqtt_publisher_result_t mqtt_publisher_start(
    mqtt_publisher_t *publisher);

/* 等待连接成功，timeout_seconds 必须大于 0。 */
mqtt_publisher_result_t mqtt_publisher_wait_connected(
    mqtt_publisher_t *publisher,
    int timeout_seconds);

/*
 * 将 measurement 发布到
 * "<topic_prefix>/<device_id>/measurements"，并等待 QoS 1 PUBACK。
 * 同一实例上的并发发布会被串行处理。
 */
mqtt_publisher_result_t mqtt_publisher_publish(
    mqtt_publisher_t *publisher,
    const measurement_t *measurement,
    int timeout_seconds);

/* 线程安全地查询当前是否已连接；publisher 为 NULL 时返回 false。 */
bool mqtt_publisher_is_connected(
    mqtt_publisher_t *publisher);

/* 停止自动重连和网络线程；publisher 为 NULL 或已停止时不执行操作。 */
void mqtt_publisher_stop(
    mqtt_publisher_t *publisher);

/*
 * 停止并释放发布器。调用后 publisher 失效。
 * 销毁时，调用方必须保证没有其他线程仍在使用该实例。
 */
void mqtt_publisher_destroy(
    mqtt_publisher_t *publisher);

#endif /* EDGEVISION_MQTT_PUBLISHER_H */
