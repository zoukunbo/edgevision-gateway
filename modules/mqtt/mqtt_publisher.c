#define _POSIX_C_SOURCE 200809L

/*
 * 基于 libmosquitto 的同步 QoS 1 发布器实现。
 * Mosquitto 在后台网络线程中触发回调，公开接口则由业务线程调用；共享状态
 * 使用 mutex 保护，并通过条件变量通知连接变化和 PUBACK 到达。
 */

#include "mqtt_publisher.h"
#include "measurement_json.h"

#include <mosquitto.h>
#include <pthread.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 下列容量都包含字符串末尾的 '\0'。 */
#define MQTT_HOST_CAPACITY 256
#define MQTT_CLIENT_ID_CAPACITY 128
#define MQTT_TOPIC_PREFIX_CAPACITY 128
#define MQTT_TOPIC_CAPACITY 384

/* libmosquitto 的全局初始化不是线程安全的，需要在多个实例之间引用计数。 */
static pthread_mutex_t library_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int library_reference_count;

/* 发布器内部生命周期状态。DISCONNECTED 状态下网络线程仍会自动重连。 */
typedef enum
{
    /* 初始状态，或网络线程已经完全停止。 */
    MQTT_PUBLISHER_STOPPED = 0,
    /* connect_async 已提交，正在等待连接回调。 */
    MQTT_PUBLISHER_CONNECTING,
    /* Broker 已接受连接，可以提交发布。 */
    MQTT_PUBLISHER_CONNECTED,
    /* 连接失败或意外断开，后台线程将按配置重连。 */
    MQTT_PUBLISHER_DISCONNECTED,
    /* 调用方正在主动停止，迟到的回调不得恢复连接状态。 */
    MQTT_PUBLISHER_STOPPING
} mqtt_publisher_state_t;

/* 头文件只做了前置声明，结构体的具体布局保留在实现文件中。 */
struct mqtt_publisher
{
    /* libmosquitto 客户端；回调中的 userdata 指回本结构体。 */
    struct mosquitto *client;

    /* mutex 保护 state、expected_mid、publish_completed 和 loop_started。 */
    pthread_mutex_t mutex;
    /* 同一发布器只允许一个线程同步等待 PUBACK。 */
    pthread_mutex_t publish_mutex;
    /* 同时通知连接状态变化和发布完成事件，等待者需循环检查自身条件。 */
    pthread_cond_t state_changed;

    /* 当前连接生命周期状态。 */
    mqtt_publisher_state_t state;
    /* 当前同步发布等待的 Mosquitto 消息 ID；-1 表示没有等待。 */
    int expected_mid;
    /* on_publish 收到匹配的 PUBACK 后置为 true。 */
    bool publish_completed;
    /* 记录 loop_start 是否成功，决定 stop 时是否需要回收网络线程。 */
    bool loop_started;

    /* 创建时复制配置，避免依赖调用方字符串的生命周期。 */
    char host[MQTT_HOST_CAPACITY];
    char client_id[MQTT_CLIENT_ID_CAPACITY];
    char topic_prefix[MQTT_TOPIC_PREFIX_CAPACITY];

    /* Broker 连接参数及指数退避范围。 */
    int port;
    int keepalive_seconds;
    unsigned int reconnect_delay_seconds;
    unsigned int reconnect_delay_max_seconds;
};

/* 校验非空字符串及其结尾 '\0' 所需的容量。 */
static bool string_fits(const char *value, size_t capacity)
{
    return value != NULL && value[0] != '\0' && strlen(value) < capacity;
}

/* 获取一个全局库引用；第一个实例负责执行 mosquitto_lib_init。 */
static bool library_acquire(void)
{
    int result = MOSQ_ERR_SUCCESS;

    pthread_mutex_lock(&library_mutex);

    /* 只有 0 -> 1 时需要真正初始化全局库。 */
    if (library_reference_count == 0)
        result = mosquitto_lib_init();

    if (result == MOSQ_ERR_SUCCESS)
        ++library_reference_count;

    pthread_mutex_unlock(&library_mutex);
    return result == MOSQ_ERR_SUCCESS;
}

/* 最后一个发布器销毁后再释放 libmosquitto 的全局资源。 */
static void library_release(void)
{
    pthread_mutex_lock(&library_mutex);

    if (library_reference_count > 0)
    {
        --library_reference_count;
        if (library_reference_count == 0)
            (void)mosquitto_lib_cleanup();
    }

    pthread_mutex_unlock(&library_mutex);
}

/* 网络线程报告连接结果，并唤醒等待连接的业务线程。 */
static void on_connect(struct mosquitto *client,
                       void *userdata,
                       int result)
{
    mqtt_publisher_t *publisher = userdata;

    (void)client;

    pthread_mutex_lock(&publisher->mutex);

    /* 停止流程一旦开始，迟到的连接回调不能将状态改回 CONNECTED。 */
    if (publisher->state != MQTT_PUBLISHER_STOPPING)
    {
        publisher->state = result == 0
                               ? MQTT_PUBLISHER_CONNECTED
                               : MQTT_PUBLISHER_DISCONNECTED;
    }

    pthread_cond_broadcast(&publisher->state_changed);
    pthread_mutex_unlock(&publisher->mutex);
}

/* 区分主动停止和意外断线；意外断线随后由 Mosquitto 自动重连。 */
static void on_disconnect(struct mosquitto *client,
                          void *userdata,
                          int result)
{
    mqtt_publisher_t *publisher = userdata;

    (void)client;
    (void)result;

    pthread_mutex_lock(&publisher->mutex);

    publisher->state = publisher->state == MQTT_PUBLISHER_STOPPING
                           ? MQTT_PUBLISHER_STOPPED
                           : MQTT_PUBLISHER_DISCONNECTED;

    pthread_cond_broadcast(&publisher->state_changed);
    pthread_mutex_unlock(&publisher->mutex);
}

/* 仅确认当前同步发布正在等待的消息，忽略超时后到达的旧 PUBACK。 */
static void on_publish(struct mosquitto *client,
                       void *userdata,
                       int mid)
{
    mqtt_publisher_t *publisher = userdata;

    (void)client;

    pthread_mutex_lock(&publisher->mutex);

    if (mid == publisher->expected_mid)
    {
        publisher->publish_completed = true;
        pthread_cond_broadcast(&publisher->state_changed);
    }

    pthread_mutex_unlock(&publisher->mutex);
}

mqtt_publisher_t *mqtt_publisher_create(
    const mqtt_publisher_config_t *config)
{
    mqtt_publisher_t *publisher;
    int result;

    /* 在分配资源前完成全部配置校验，失败时没有清理负担。 */
    if (config == NULL ||
        !string_fits(config->host, MQTT_HOST_CAPACITY) ||
        !string_fits(config->client_id, MQTT_CLIENT_ID_CAPACITY) ||
        !string_fits(config->topic_prefix, MQTT_TOPIC_PREFIX_CAPACITY) ||
        config->port <= 0 || config->port > UINT16_MAX ||
        config->keepalive_seconds < 5 ||
        config->reconnect_delay_seconds >
            config->reconnect_delay_max_seconds)
    {
        return NULL;
    }

    /* libmosquitto 必须先于 mosquitto_new 初始化。 */
    if (!library_acquire())
        return NULL;

    /* calloc 将布尔值和指针初始化为 0/NULL，便于错误路径清理。 */
    publisher = calloc(1, sizeof(*publisher));
    if (publisher == NULL)
    {
        library_release();
        return NULL;
    }

    /* 线程资源逐个创建；任一步失败都跳到对应的逆序清理标签。 */
    result = pthread_mutex_init(&publisher->mutex, NULL);
    if (result != 0)
        goto FREE_PUBLISHER;

    result = pthread_mutex_init(&publisher->publish_mutex, NULL);
    if (result != 0)
        goto DESTROY_MUTEX;

    result = pthread_cond_init(&publisher->state_changed, NULL);
    if (result != 0)
        goto DESTROY_PUBLISH_MUTEX;

    /* 配置由实例持有，创建完成后调用方无需继续保存原字符串。 */
    memcpy(publisher->host, config->host, strlen(config->host) + 1);
    memcpy(publisher->client_id,
           config->client_id,
           strlen(config->client_id) + 1);
    memcpy(publisher->topic_prefix,
           config->topic_prefix,
           strlen(config->topic_prefix) + 1);

    publisher->port = config->port;
    publisher->keepalive_seconds = config->keepalive_seconds;
    publisher->reconnect_delay_seconds = config->reconnect_delay_seconds;
    publisher->reconnect_delay_max_seconds =
        config->reconnect_delay_max_seconds;
    publisher->state = MQTT_PUBLISHER_STOPPED;
    publisher->expected_mid = -1;

    /* true 使用 clean session；publisher 作为 userdata 传递给全部回调。 */
    publisher->client = mosquitto_new(publisher->client_id, true, publisher);
    if (publisher->client == NULL)
        goto DESTROY_CONDITION;

    /* 注册回调后，网络线程才能把异步事件同步回业务线程。 */
    mosquitto_connect_callback_set(publisher->client, on_connect);
    mosquitto_disconnect_callback_set(publisher->client, on_disconnect);
    mosquitto_publish_callback_set(publisher->client, on_publish);

    /* true 表示使用指数退避，等待时间不会超过配置的最大值。 */
    result = mosquitto_reconnect_delay_set(
        publisher->client,
        publisher->reconnect_delay_seconds,
        publisher->reconnect_delay_max_seconds,
        true);
    if (result != MOSQ_ERR_SUCCESS)
        goto DESTROY_CLIENT;

    return publisher;

/* 初始化中途失败时，只逆序销毁已经成功创建的资源。 */
DESTROY_CLIENT:
    mosquitto_destroy(publisher->client);
DESTROY_CONDITION:
    pthread_cond_destroy(&publisher->state_changed);
DESTROY_PUBLISH_MUTEX:
    pthread_mutex_destroy(&publisher->publish_mutex);
DESTROY_MUTEX:
    pthread_mutex_destroy(&publisher->mutex);
FREE_PUBLISHER:
    free(publisher);
    library_release();
    return NULL;
}

mqtt_publisher_result_t mqtt_publisher_start(
    mqtt_publisher_t *publisher)
{
    int result;

    if (publisher == NULL)
        return MQTT_PUBLISHER_INVALID_ARGUMENT;

    /* 生命周期状态在锁内检查，避免重复启动同一个客户端。 */
    pthread_mutex_lock(&publisher->mutex);

    if (publisher->state != MQTT_PUBLISHER_STOPPED)
    {
        pthread_mutex_unlock(&publisher->mutex);
        return MQTT_PUBLISHER_INVALID_ARGUMENT;
    }

    publisher->state = MQTT_PUBLISHER_CONNECTING;

    /* 回调在线程启动后异步报告真正的连接结果。 */
    result = mosquitto_connect_async(publisher->client,
                                     publisher->host,
                                     publisher->port,
                                     publisher->keepalive_seconds);
    /* loop_start 创建后台网络线程，后续连接和重连均由它驱动。 */
    if (result == MOSQ_ERR_SUCCESS)
        result = mosquitto_loop_start(publisher->client);

    if (result != MOSQ_ERR_SUCCESS)
    {
        publisher->state = MQTT_PUBLISHER_DISCONNECTED;
        pthread_cond_broadcast(&publisher->state_changed);
        pthread_mutex_unlock(&publisher->mutex);
        return MQTT_PUBLISHER_LIBRARY_ERROR;
    }

    publisher->loop_started = true;
    pthread_mutex_unlock(&publisher->mutex);

    return MQTT_PUBLISHER_OK;
}

mqtt_publisher_result_t mqtt_publisher_wait_connected(
    mqtt_publisher_t *publisher,
    int timeout_seconds)
{
    struct timespec deadline;
    mqtt_publisher_result_t outcome;
    int result;

    if (publisher == NULL || timeout_seconds <= 0)
        return MQTT_PUBLISHER_INVALID_ARGUMENT;

    /* pthread_cond_timedwait 接收绝对时间，而不是相对等待秒数。 */
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        return MQTT_PUBLISHER_LIBRARY_ERROR;

    deadline.tv_sec += timeout_seconds;
    pthread_mutex_lock(&publisher->mutex);

    /* DISCONNECTED 仍可能自动重连，因此继续等待直到成功、停止或超时。 */
    while (publisher->state == MQTT_PUBLISHER_CONNECTING ||
           publisher->state == MQTT_PUBLISHER_DISCONNECTED)
    {
        /* 条件变量可能无理由唤醒，因此必须回到 while 重新检查状态。 */
        result = pthread_cond_timedwait(&publisher->state_changed,
                                        &publisher->mutex,
                                        &deadline);
        if (result == ETIMEDOUT)
        {
            pthread_mutex_unlock(&publisher->mutex);
            return MQTT_PUBLISHER_TIMEOUT;
        }

        if (result != 0)
        {
            pthread_mutex_unlock(&publisher->mutex);
            return MQTT_PUBLISHER_LIBRARY_ERROR;
        }
    }

    outcome = publisher->state == MQTT_PUBLISHER_CONNECTED
                  ? MQTT_PUBLISHER_OK
                  : MQTT_PUBLISHER_NOT_CONNECTED;

    pthread_mutex_unlock(&publisher->mutex);
    return outcome;
}

mqtt_publisher_result_t mqtt_publisher_publish(
    mqtt_publisher_t *publisher,
    const measurement_t *measurement,
    int timeout_seconds)
{
    char topic[MQTT_TOPIC_CAPACITY];
    char *json = NULL;
    size_t json_size = 0;
    struct timespec deadline;
    mqtt_publisher_result_t outcome = MQTT_PUBLISHER_OK;
    int mid = -1;
    int result;

    if (publisher == NULL || measurement == NULL || timeout_seconds <= 0)
        return MQTT_PUBLISHER_INVALID_ARGUMENT;

    /* 序列化函数同时验证 Measurement，并为 JSON 分配内存。 */
    if (measurement_to_json(measurement, &json, &json_size) !=
        MEASUREMENT_JSON_OK)
    {
        return MQTT_PUBLISHER_SERIALIZATION_ERROR;
    }

    /* 每台设备使用独立主题；snprintf 的返回值用于检测截断。 */
    result = snprintf(topic,
                      sizeof(topic),
                      "%s/%s/measurements",
                      publisher->topic_prefix,
                      measurement->device_id);
    if (result < 0 || (size_t)result >= sizeof(topic) || json_size > INT_MAX)
    {
        measurement_json_free(json);
        return MQTT_PUBLISHER_INVALID_ARGUMENT;
    }

    /* 超时覆盖等待发布锁以及等待 PUBACK 的整个同步操作。 */
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        measurement_json_free(json);
        return MQTT_PUBLISHER_LIBRARY_ERROR;
    }

    deadline.tv_sec += timeout_seconds;

    /* 序列化同一实例的发布，避免多个调用者争用唯一的 expected_mid。 */
    /* 固定加锁顺序为 publish_mutex -> mutex，避免线程间锁顺序反转。 */
    pthread_mutex_lock(&publisher->publish_mutex);
    pthread_mutex_lock(&publisher->mutex);

    if (publisher->state != MQTT_PUBLISHER_CONNECTED)
    {
        outcome = MQTT_PUBLISHER_NOT_CONNECTED;
        goto FINISH;
    }

    /* 持锁设置 expected_mid，避免 PUBACK 先于消息 ID 的保存到达。 */
    publisher->publish_completed = false;
    publisher->expected_mid = -1;

    /* QoS=1、retain=false；mid 由 libmosquitto 写入。 */
    result = mosquitto_publish(publisher->client,
                               &mid,
                               topic,
                               (int)json_size,
                               json,
                               1,
                               false);
    if (result != MOSQ_ERR_SUCCESS)
    {
        /* 断开回调可能尚未取得 mutex，优先使用 Mosquitto 的即时结果。 */
        outcome = result == MOSQ_ERR_NO_CONN
                      ? MQTT_PUBLISHER_NOT_CONNECTED
                      : MQTT_PUBLISHER_LIBRARY_ERROR;
        goto FINISH;
    }

    publisher->expected_mid = mid;

    /* 断线回调也会广播条件变量，因此循环同时检查连接状态。 */
    while (!publisher->publish_completed &&
           publisher->state == MQTT_PUBLISHER_CONNECTED)
    {
        result = pthread_cond_timedwait(&publisher->state_changed,
                                        &publisher->mutex,
                                        &deadline);
        if (result == ETIMEDOUT)
        {
            outcome = MQTT_PUBLISHER_TIMEOUT;
            goto FINISH;
        }

        if (result != 0)
        {
            outcome = MQTT_PUBLISHER_LIBRARY_ERROR;
            goto FINISH;
        }
    }

    if (!publisher->publish_completed)
        outcome = MQTT_PUBLISHER_NOT_CONNECTED;

FINISH:
    /* 清除消息 ID，使超时后迟到的 PUBACK 不会完成下一次发布。 */
    publisher->expected_mid = -1;
    pthread_mutex_unlock(&publisher->mutex);
    pthread_mutex_unlock(&publisher->publish_mutex);
    measurement_json_free(json);

    return outcome;
}

bool mqtt_publisher_is_connected(mqtt_publisher_t *publisher)
{
    bool connected;

    if (publisher == NULL)
        return false;

    /* 即使只是读取，也必须加锁以避免与网络回调发生数据竞争。 */
    pthread_mutex_lock(&publisher->mutex);
    connected = publisher->state == MQTT_PUBLISHER_CONNECTED;
    pthread_mutex_unlock(&publisher->mutex);

    return connected;
}

void mqtt_publisher_stop(mqtt_publisher_t *publisher)
{
    bool loop_started;

    if (publisher == NULL)
        return;

    pthread_mutex_lock(&publisher->mutex);

    /* STOPPED 重复调用直接返回，使 stop 具备幂等性。 */
    if (publisher->state == MQTT_PUBLISHER_STOPPED)
    {
        pthread_mutex_unlock(&publisher->mutex);
        return;
    }

    /* 先发布 STOPPING，立即唤醒正在等待连接或 PUBACK 的线程。 */
    publisher->state = MQTT_PUBLISHER_STOPPING;
    loop_started = publisher->loop_started;
    pthread_cond_broadcast(&publisher->state_changed);
    pthread_mutex_unlock(&publisher->mutex);

    /* 先尝试发送 DISCONNECT，再强制结束线程，避免断线状态下永久等待。 */
    (void)mosquitto_disconnect(publisher->client);
    if (loop_started)
        (void)mosquitto_loop_stop(publisher->client, true);

    /* loop_stop 返回后网络回调不会再访问状态，可以最终标记为 STOPPED。 */
    pthread_mutex_lock(&publisher->mutex);
    publisher->loop_started = false;
    publisher->state = MQTT_PUBLISHER_STOPPED;
    pthread_cond_broadcast(&publisher->state_changed);
    pthread_mutex_unlock(&publisher->mutex);
}

void mqtt_publisher_destroy(mqtt_publisher_t *publisher)
{
    if (publisher == NULL)
        return;

    /* 先停止网络线程，再按依赖关系逆序销毁客户端和线程同步对象。 */
    mqtt_publisher_stop(publisher);
    mosquitto_destroy(publisher->client);
    pthread_cond_destroy(&publisher->state_changed);
    pthread_mutex_destroy(&publisher->publish_mutex);
    pthread_mutex_destroy(&publisher->mutex);
    free(publisher);
    library_release();
}
