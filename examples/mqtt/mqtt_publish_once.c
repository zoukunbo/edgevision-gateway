#define _POSIX_C_SOURCE 200809L

/*
 * MQTT 单次发布示例：
 * 1. 将一条 measurement_t 测量数据序列化为 JSON；
 * 2. 以 QoS 1 发布到本地 MQTT Broker；
 * 3. 等待 Broker 返回 PUBACK 后再退出。
 *
 * Mosquitto 的网络循环运行在后台线程中，因此发布回调与 main 线程之间
 * 通过互斥量和条件变量同步。
 */

#include "measurement.h"
#include "measurement_json.h"

#include <mosquitto.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MQTT_BROKER_HOST "127.0.0.1"
#define MQTT_BROKER_PORT 1883
#define MQTT_KEEPALIVE_SECONDS 30
#define MQTT_PUBLISH_TIMEOUT_SECONDS 5

#define MQTT_CLIENT_ID "edgevision-host-d34"
#define MQTT_TOPIC \
    "edgevision/v1/devices/sim-temperature-01/measurements"

/* main 线程等待发布回调时使用的共享状态。 */
typedef struct
{
    /* 保护 expected_mid 和 completed，并配合 condition 等待。 */
    pthread_mutex_t mutex;
    pthread_cond_t condition;

    /* 本次发布的消息 ID；只接受与其匹配的 PUBACK。 */
    int expected_mid;
    bool completed;
} publish_wait_context_t;

/*
 * 发布 JSON 消息，并通过 mid_output 返回 Mosquitto 分配的消息 ID。
 * QoS 设为 1，表示 Broker 至少接收一次，并以 PUBACK 确认。
 */
static int mqtt_publish_json(struct mosquitto *client,
                             const char *topic,
                             const char *json,
                             size_t json_size,
                             int *mid_output)
{
    int result = mosquitto_publish(client,
                                   mid_output,
                                   topic,
                                   (int)json_size,
                                   json,
                                   1,
                                   false);

    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr,
                "mqtt_publish_json: publish failed: %s\n",
                mosquitto_strerror(result));
    }

    return result;
}

/* Mosquitto 在收到 QoS 1 的 PUBACK 后从网络循环线程调用此函数。 */
static void on_publish(struct mosquitto *client,
                       void *userdata,
                       int mid)
{
    publish_wait_context_t *context = userdata;

    (void)client;

    pthread_mutex_lock(&context->mutex);

    if (mid == context->expected_mid)
    {
        context->completed = true;
        pthread_cond_signal(&context->condition);
    }

    pthread_mutex_unlock(&context->mutex);
}

/* 在指定秒数内等待目标消息的发布确认，成功返回 0，失败返回 -1。 */
static int wait_for_publish(publish_wait_context_t *context,
                            int timeout_seconds)
{
    struct timespec deadline;
    int result = 0;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
    {
        perror("clock_gettime");
        return -1;
    }

    deadline.tv_sec += timeout_seconds;

    pthread_mutex_lock(&context->mutex);

    /*
     * 必须使用while，而不是if：
     * 条件变量允许无理由唤醒，醒来后必须重新检查业务条件。
     */
    while (!context->completed)
    {
        result = pthread_cond_timedwait(&context->condition,
                                        &context->mutex,
                                        &deadline);

        if (result == ETIMEDOUT)
        {
            fprintf(stderr, "wait_for_publish: PUBACK timeout\n");
            pthread_mutex_unlock(&context->mutex);
            return -1;
        }

        if (result != 0)
        {
            fprintf(stderr,
                    "wait_for_publish: %s\n",
                    strerror(result));
            pthread_mutex_unlock(&context->mutex);
            return -1;
        }
    }

    pthread_mutex_unlock(&context->mutex);
    return 0;
}

int main(void)
{
    const measurement_t measurement = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "sim-temperature-01",
        .sequence = 1,
        .timestamp_ms = INT64_C(1787623200000),
        .metric = "temperature",
        .value = 26.5,
        .unit = "celsius",
        .quality = MEASUREMENT_QUALITY_GOOD
    };

    /*
     * 资源按“互斥量 -> 条件变量 -> JSON -> Mosquitto 库 -> 客户端”
     * 的顺序创建，退出时通过下方标签按相反顺序释放。
     */
    publish_wait_context_t context;
    struct mosquitto *client = NULL;
    char *json = NULL;
    size_t json_size = 0;
    int mid = -1;
    int result;
    int exit_code = EXIT_FAILURE;
    bool loop_started = false;

    result = pthread_mutex_init(&context.mutex, NULL);
    if (result != 0)
    {
        fprintf(stderr, "pthread_mutex_init: %s\n", strerror(result));
        return EXIT_FAILURE;
    }

    result = pthread_cond_init(&context.condition, NULL);
    if (result != 0)
    {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(result));
        pthread_mutex_destroy(&context.mutex);
        return EXIT_FAILURE;
    }

    context.expected_mid = -1;
    context.completed = false;

    /* measurement_to_json 会分配 json，最终由 measurement_json_free 释放。 */
    result = measurement_to_json(&measurement, &json, &json_size);
    if (result != MEASUREMENT_JSON_OK)
    {
        fprintf(stderr, "measurement_to_json failed: %d\n", result);
        goto CLEANUP;
    }

    /* Mosquitto 全局库初始化；成功后必须调用 mosquitto_lib_cleanup。 */
    result = mosquitto_lib_init();
    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr,
                "mosquitto_lib_init: %s\n",
                mosquitto_strerror(result));
        goto CLEANUP;
    }

    /* true 表示使用 clean session；context 会作为 userdata 传给回调。 */
    client = mosquitto_new(MQTT_CLIENT_ID, true, &context);
    if (client == NULL)
    {
        fprintf(stderr, "mosquitto_new failed\n");
        goto MQTT_CLEANUP;
    }

    mosquitto_publish_callback_set(client, on_publish);

    /* 建立到 Broker 的 TCP/MQTT 连接。 */
    result = mosquitto_connect(client,
                               MQTT_BROKER_HOST,
                               MQTT_BROKER_PORT,
                               MQTT_KEEPALIVE_SECONDS);
    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr,
                "mosquitto_connect: %s\n",
                mosquitto_strerror(result));
        goto CLIENT_CLEANUP;
    }

    /* 启动后台网络线程，用于收发 MQTT 报文并触发 on_publish。 */
    result = mosquitto_loop_start(client);
    if (result != MOSQ_ERR_SUCCESS)
    {
        fprintf(stderr,
                "mosquitto_loop_start: %s\n",
                mosquitto_strerror(result));
        goto CLIENT_CLEANUP;
    }

    loop_started = true;

    /*
     * 持锁完成“提交发布 + 保存expected_mid”。
     * 如果PUBACK很快到达，回调线程会先阻塞在同一把锁上，
     * 不会在expected_mid赋值前完成错误判断。
     */
    pthread_mutex_lock(&context.mutex);

    context.completed = false;

    result = mqtt_publish_json(client,
                               MQTT_TOPIC,
                               json,
                               json_size,
                               &mid);

    if (result == MOSQ_ERR_SUCCESS)
        context.expected_mid = mid;

    pthread_mutex_unlock(&context.mutex);

    if (result != MOSQ_ERR_SUCCESS)
        goto CLIENT_CLEANUP;

    /* 收到与 mid 匹配的 PUBACK，才认为本次 QoS 1 发布完成。 */
    if (wait_for_publish(&context,
                         MQTT_PUBLISH_TIMEOUT_SECONDS) != 0)
    {
        goto CLIENT_CLEANUP;
    }

    printf("PUBLISH_CONFIRMED mid=%d business_id=(%s,%u)\n",
           mid,
           measurement.device_id,
           measurement.sequence);
    printf("payload=%s\n", json);

    exit_code = EXIT_SUCCESS;

CLIENT_CLEANUP:
    /* 客户端清理：断开连接、停止后台线程，再销毁客户端对象。 */
    if (client != NULL)
    {
        (void)mosquitto_disconnect(client);

        if (loop_started)
            (void)mosquitto_loop_stop(client, false);

        mosquitto_destroy(client);
    }

MQTT_CLEANUP:
    /* 与 mosquitto_lib_init 成对。 */
    mosquitto_lib_cleanup();

CLEANUP:
    /* 这些资源在进入 Mosquitto 初始化前已经创建，因此统一在此释放。 */
    measurement_json_free(json);
    pthread_cond_destroy(&context.condition);
    pthread_mutex_destroy(&context.mutex);

    return exit_code;
}
