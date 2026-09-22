#define _POSIX_C_SOURCE 200809L

#include "mqtt_command_receiver.h"
#include "mqtt_library_runtime.h"

#include <mosquitto.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MQTT_RECEIVER_HOST_CAPACITY 256u
#define MQTT_RECEIVER_CLIENT_ID_CAPACITY 128u
#define MQTT_RECEIVER_TOPIC_CAPACITY 384u

/*
 * STOPPED：尚未启动或网络线程已经停止。
 * CONNECTING：正在等待 CONNACK 或 SUBACK。
 * READY：Broker 已接受连接和订阅。
 * FAILED：连接或订阅失败。
 * STOPPING：调用方正在停止，迟到的回调不能恢复 READY。
 */
typedef enum
{
    MQTT_COMMAND_RECEIVER_STOPPED = 0,
    MQTT_COMMAND_RECEIVER_CONNECTING,
    MQTT_COMMAND_RECEIVER_READY,
    MQTT_COMMAND_RECEIVER_FAILED,
    MQTT_COMMAND_RECEIVER_STOPPING
} mqtt_command_receiver_state_t;

struct mqtt_command_receiver
{
    struct mosquitto *client;

    /*
     * 网络回调修改 state，业务线程在 wait_ready 中读取，
     * 所以需要 mutex 和 condition。
     */
    pthread_mutex_t mutex;
    pthread_cond_t state_changed;

    mqtt_command_receiver_state_t state;
    bool loop_started;

    /*
     * 创建时复制字符串，避免继续依赖调用方配置变量的生命周期。
     */
    char host[MQTT_RECEIVER_HOST_CAPACITY];
    char client_id[MQTT_RECEIVER_CLIENT_ID_CAPACITY];
    char request_topic[MQTT_RECEIVER_TOPIC_CAPACITY];

    int port;
    int keepalive_seconds;

    /*
     * 创建后保持不变。on_message 在网络线程中调用 handler。
     */
    mqtt_command_message_handler_t message_handler;
    void *message_handler_context;
};

/* 在锁内修改状态并唤醒 wait_ready。 */
static void receiver_set_state(
    mqtt_command_receiver_t *receiver,
    mqtt_command_receiver_state_t state)
{
    pthread_mutex_lock(&receiver->mutex);

    /*
     * 停止一旦开始，迟到的连接或订阅回调不能重新发布 READY。
     */
    if (receiver->state != MQTT_COMMAND_RECEIVER_STOPPING)
        receiver->state = state;

    pthread_cond_broadcast(&receiver->state_changed);
    pthread_mutex_unlock(&receiver->mutex);
}

/*
 * 收到 CONNACK 后执行。
 * 连接成功只说明 Broker 接受客户端，还需要订阅请求主题。
 */
static void on_connect(struct mosquitto *client,
                       void *userdata,
                       int result)
{
    mqtt_command_receiver_t *receiver = userdata;

    if (result != 0)
    {
        receiver_set_state(
            receiver, MQTT_COMMAND_RECEIVER_FAILED);
        return;
    }

    int rc = mosquitto_subscribe(
        client,
        NULL,                    /* 本版不关联订阅 mid。 */
        receiver->request_topic,
        1);                      /* 请求 QoS 1。 */

    if (rc != MOSQ_ERR_SUCCESS)
    {
        receiver_set_state(
            receiver, MQTT_COMMAND_RECEIVER_FAILED);
    }
}

/*
 * 收到 SUBACK 后执行。
 * 本版只订阅一个主题并请求 QoS 1，所以只接受一个 QoS 1 结果。
 */
static void on_subscribe(struct mosquitto *client,
                         void *userdata,
                         int mid,
                         int count,
                         const int *granted_qos)
{
    mqtt_command_receiver_t *receiver = userdata;

    (void)client;
    (void)mid;

    if (count != 1 ||
        granted_qos == NULL ||
        granted_qos[0] != 1)
    {
        receiver_set_state(
            receiver, MQTT_COMMAND_RECEIVER_FAILED);
        return;
    }

    receiver_set_state(
        receiver, MQTT_COMMAND_RECEIVER_READY);
}

/*
 * 每收到一条订阅消息执行一次。
 *
 * 这里不解析 JSON、不调用 gateway_handle_command，也不等待发布结果。
 * handler 必须在返回前复制需要保留的 topic 和 payload。
 */
static void on_message(
    struct mosquitto *client,
    void *userdata,
    const struct mosquitto_message *message)
{
    mqtt_command_receiver_t *receiver = userdata;

    (void)client;

    if (message == NULL ||
        message->topic == NULL ||
        message->payloadlen < 0)
    {
        return;
    }

    receiver->message_handler(
        receiver->message_handler_context,
        message->topic,
        message->payload,
        (size_t)message->payloadlen);
}

/* 校验非空字符串及其结尾 '\0' 所需的容量。 */
static bool string_fits(const char *value, size_t capacity)
{
    return value != NULL && value[0] != '\0' && strlen(value)  < capacity;
}

/* 复制配置并创建对象；此时尚未连接 Broker。 */
mqtt_command_receiver_t *mqtt_command_receiver_create(
    const mqtt_command_receiver_config_t *config)
{
    if (config == NULL || 
        !string_fits(config->client_id, MQTT_RECEIVER_CLIENT_ID_CAPACITY) ||
        !string_fits(config->host, MQTT_RECEIVER_HOST_CAPACITY) ||
        !string_fits(config->request_topic, MQTT_RECEIVER_TOPIC_CAPACITY) ||
        config->port < 1 || config->port > UINT16_MAX ||
        config->keepalive_seconds < 5 ||
        config->message_handler == NULL 
        )
    {
        return NULL;
    }

    if (!mqtt_library_acquire())
    {
        return NULL;
    }
    
    
    mqtt_command_receiver_t *receiver = calloc(1, sizeof(*receiver));
    if (receiver == NULL)
    {
        goto RELEASE_LIBRARY;
    }

    memcpy(receiver->host, config->host, strlen(config->host) + 1);
    memcpy(receiver->client_id, config->client_id, strlen(config->client_id) + 1);
    memcpy(receiver->request_topic, config->request_topic, strlen(config->request_topic) + 1);

    receiver->port = config->port;
    receiver->keepalive_seconds = config->keepalive_seconds;
    receiver->message_handler = config->message_handler;
    receiver->message_handler_context = config->message_handler_context;
    
    int ret = pthread_mutex_init(&receiver->mutex, NULL);
    if (ret != 0)
    {
        goto FREE_RECEIVER;
    }

    ret = pthread_cond_init(&receiver->state_changed, NULL);
    if (ret != 0)
    {
        goto DESTROY_MUTEX;
    }

    receiver->client = mosquitto_new(config->client_id, true, receiver);

    if (receiver->client == NULL)
    {
        goto DESTROY_CONDITION;
    }

    mosquitto_connect_callback_set(receiver->client, on_connect);
    mosquitto_subscribe_callback_set(receiver->client, on_subscribe);
    mosquitto_message_callback_set(receiver->client, on_message);

    return receiver;
RELEASE_LIBRARY:
    mqtt_library_release();
    return NULL;

FREE_RECEIVER:
    free(receiver);
    goto RELEASE_LIBRARY;

DESTROY_MUTEX:
    pthread_mutex_destroy(&receiver->mutex);
    goto FREE_RECEIVER;

DESTROY_CONDITION:
    pthread_cond_destroy(&receiver->state_changed);
    goto DESTROY_MUTEX;
}

/* 发起连接并启动 Mosquitto 后台网络线程。 */
mqtt_command_receiver_result_t mqtt_command_receiver_start(
    mqtt_command_receiver_t *receiver)
{
    if (receiver == NULL)
    {
        return MQTT_COMMAND_RECEIVER_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&receiver->mutex);

    if (receiver->state != MQTT_COMMAND_RECEIVER_STOPPED)
    {
        pthread_mutex_unlock(&receiver->mutex);
        return MQTT_COMMAND_RECEIVER_INVALID_ARGUMENT;
    }
    
    receiver->state = MQTT_COMMAND_RECEIVER_CONNECTING;

    int result = mosquitto_connect_async(receiver->client, 
        receiver->host, 
        receiver->port, 
        receiver->keepalive_seconds);


    if (result != MOSQ_ERR_SUCCESS)
    {
        receiver->state = MQTT_COMMAND_RECEIVER_FAILED;
        pthread_cond_broadcast(&receiver->state_changed);
        pthread_mutex_unlock(&receiver->mutex);
        return MQTT_COMMAND_RECEIVER_LIBRARY_ERROR;
    }
        
    result = mosquitto_loop_start(receiver->client);


    if (result != MOSQ_ERR_SUCCESS)
    {
        receiver->state = MQTT_COMMAND_RECEIVER_FAILED;
        pthread_cond_broadcast(&receiver->state_changed);
        pthread_mutex_unlock(&receiver->mutex);
        return MQTT_COMMAND_RECEIVER_LIBRARY_ERROR;
    }

    receiver->loop_started = true;
    pthread_mutex_unlock(&receiver->mutex);
    
    return MQTT_COMMAND_RECEIVER_OK;
}
    
    

/*
 * 等待 Broker 接受连接并确认请求主题订阅。
 * timeout_seconds 必须大于 0。
 */
mqtt_command_receiver_result_t
mqtt_command_receiver_wait_ready(
    mqtt_command_receiver_t *receiver,
    int timeout_seconds)
{
    if (receiver == NULL || timeout_seconds <= 0)
        return MQTT_COMMAND_RECEIVER_INVALID_ARGUMENT;

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        return MQTT_COMMAND_RECEIVER_LIBRARY_ERROR;
    }

    deadline.tv_sec += timeout_seconds;

    pthread_mutex_lock(&receiver->mutex);

    while (receiver->state == MQTT_COMMAND_RECEIVER_CONNECTING)
    {
        int result = pthread_cond_timedwait(
            &receiver->state_changed,
            &receiver->mutex,
            &deadline
        );

        if (result == ETIMEDOUT)
        {
            pthread_mutex_unlock(&receiver->mutex);
            return MQTT_COMMAND_RECEIVER_TIMEOUT;
        }

        if (result != 0)
        {
            pthread_mutex_unlock(&receiver->mutex);
            return MQTT_COMMAND_RECEIVER_LIBRARY_ERROR;
        }
    }
    
    mqtt_command_receiver_result_t outcome = receiver->state == MQTT_COMMAND_RECEIVER_READY ?
        MQTT_COMMAND_RECEIVER_OK : MQTT_COMMAND_RECEIVER_NOT_CONNECTED;

    pthread_mutex_unlock(&receiver->mutex);

    return outcome;
}

/*
 * 停止网络线程并释放对象。
 * 返回后不再执行 message_handler。
 */
void mqtt_command_receiver_destroy(
    mqtt_command_receiver_t *receiver)
{
    if (receiver == NULL)
    {
        return;
    }
    
    pthread_mutex_lock(&receiver->mutex);

    receiver->state = MQTT_COMMAND_RECEIVER_STOPPING;
    bool loop_started = receiver->loop_started;
    pthread_cond_broadcast(&receiver->state_changed);

    pthread_mutex_unlock(&receiver->mutex);

    (void)mosquitto_disconnect(receiver->client);

    if (loop_started)
        (void)mosquitto_loop_stop(receiver->client, true);

    mosquitto_destroy(receiver->client);
    pthread_cond_destroy(&receiver->state_changed);
    pthread_mutex_destroy(&receiver->mutex);
    free(receiver);
    mqtt_library_release();

}