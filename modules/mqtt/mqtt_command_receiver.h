#ifndef EDGEVISION_MQTT_COMMAND_RECEIVER_H
#define EDGEVISION_MQTT_COMMAND_RECEIVER_H

#include <stddef.h>

/*
 * 收到 MQTT 请求时调用。
 *
 * context：
 *   创建 Receiver 时由调用方提供的自定义上下文。
 *
 * topic、payload：
 *   只在本次回调期间有效。需要异步处理时，handler 必须在返回前复制。
 *
 * payload_length：
 *   payload 的字节数；payload 不保证以 '\0' 结尾。
 *
 * handler 运行在 Mosquitto 网络线程中，不能执行阻塞业务、等待其他
 * worker 或等待 MQTT 发布确认。
 */
typedef void(*mqtt_command_message_handler_t) (
    void *context,
    const char *topic,
    const void *payload,
    size_t payload_length
);

typedef struct
{
    const char *host;
    int port;

    /* 必须与 Publisher 使用不同的客户端 ID。 */
    const char *client_id;

    const char *request_topic;
    int keepalive_seconds;

    mqtt_command_message_handler_t message_handler;
    void *message_handler_context;
} mqtt_command_receiver_config_t;

/* 隐藏 Mosquitto 客户端和线程状态。 */

typedef struct mqtt_command_receiver mqtt_command_receiver_t;

typedef enum
{
    MQTT_COMMAND_RECEIVER_OK = 0,
    MQTT_COMMAND_RECEIVER_INVALID_ARGUMENT = -1,
    MQTT_COMMAND_RECEIVER_NO_MEMORY = -2,
    MQTT_COMMAND_RECEIVER_LIBRARY_ERROR = -3,
    MQTT_COMMAND_RECEIVER_NOT_CONNECTED = -4,
     MQTT_COMMAND_RECEIVER_TIMEOUT = -5
} mqtt_command_receiver_result_t;

/* 复制配置并创建对象；此时尚未连接 Broker。 */
mqtt_command_receiver_t *mqtt_command_receiver_create(
    const mqtt_command_receiver_config_t *config);

/* 发起连接并启动 Mosquitto 后台网络线程。 */
mqtt_command_receiver_result_t mqtt_command_receiver_start(
    mqtt_command_receiver_t *receiver);

/*
 * 等待 Broker 接受连接并确认请求主题订阅。
 * timeout_seconds 必须大于 0。
 */
mqtt_command_receiver_result_t
mqtt_command_receiver_wait_ready(
    mqtt_command_receiver_t *receiver,
    int timeout_seconds);

/*
 * 停止网络线程并释放对象。
 * 返回后不再执行 message_handler。
 */
void mqtt_command_receiver_destroy(
    mqtt_command_receiver_t *receiver);

#endif // !EDGEVISION_MQTT_COMMAND_RECEIVER_H
