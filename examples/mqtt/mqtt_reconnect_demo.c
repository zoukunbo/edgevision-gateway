#define _POSIX_C_SOURCE 200809L

/*
 * MQTT 自动重连示例：连续成功发布 10 条温度 Measurement，每条间隔 2 秒。
 * 连接中断时等待 mqtt_publisher 的后台线程自动重连，并重试尚未确认的消息。
 */

#include "mqtt_publisher.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Broker 地址和主题命名参数。最终主题会追加 device_id/measurements。 */
#define MQTT_BROKER_HOST "127.0.0.1"
#define MQTT_BROKER_PORT 1883
#define MQTT_CLIENT_ID "edgevision-reconnect-demo"
#define MQTT_TOPIC_PREFIX "edgevision/v1/devices"

/* 连接保活、指数重连以及单次连接/发布等待时间。 */
#define MQTT_KEEPALIVE_SECONDS 30
#define MQTT_RECONNECT_DELAY_SECONDS 1u
#define MQTT_RECONNECT_DELAY_MAX_SECONDS 8u
#define MQTT_OPERATION_TIMEOUT_SECONDS 5

/* 示例要求：确认 10 条消息，相邻成功消息之间休眠 2 秒。 */
#define MEASUREMENT_COUNT 10u
#define PUBLISH_INTERVAL_SECONDS 2

/* 将 POSIX 的秒/纳秒时间转换为 Measurement 契约使用的 Unix 毫秒。 */
static int64_t current_timestamp_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;

    return (int64_t)now.tv_sec * INT64_C(1000) +
           (int64_t)now.tv_nsec / INT64_C(1000000);
}

/* 成功返回 0；nanosleep 发生非信号类错误时返回 -1。 */
static int sleep_between_publishes(void)
{
    struct timespec remaining = {
        .tv_sec = PUBLISH_INTERVAL_SECONDS,
        .tv_nsec = 0
    };

    /* 被信号中断时继续休眠剩余时间，确保成功消息之间约为 2 秒。 */
    while (nanosleep(&remaining, &remaining) != 0)
    {
        if (errno != EINTR)
            return -1;
    }

    return 0;
}

/*
 * 每次最多等待 5 秒；超时只打印进度并继续，因为后台网络线程仍在重连。
 * STOPPED 等非超时错误表示无法继续，由调用方结束程序。
 */
static int wait_until_connected(mqtt_publisher_t *publisher)
{
    mqtt_publisher_result_t result;

    for (;;)
    {
        result = mqtt_publisher_wait_connected(
            publisher,
            MQTT_OPERATION_TIMEOUT_SECONDS);

        if (result == MQTT_PUBLISHER_OK)
            return 0;

        if (result != MQTT_PUBLISHER_TIMEOUT)
        {
            fprintf(stderr, "wait for MQTT connection failed: %d\n", result);
            return -1;
        }

        fprintf(stderr, "MQTT connection unavailable; waiting for reconnect\n");
    }
}

/* 为当前序号创建一条合法的模拟温度数据，并按值返回完整快照。 */
static measurement_t create_measurement(uint32_t sequence,
                                        int64_t timestamp_ms)
{
    return (measurement_t){
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "sim-temperature-01",
        .sequence = sequence,
        .timestamp_ms = timestamp_ms,
        .metric = "temperature",
        .value = 26.0 + (double)sequence * 0.1,
        .unit = "celsius",
        .quality = MEASUREMENT_QUALITY_GOOD
    };
}

/* 将可选命令行端口转换为 MQTT 配置需要的 int。 */
static int parse_port(const char *text, int *port_output)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value <= 0 || value > UINT16_MAX)
    {
        return -1;
    }

    *port_output = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    int broker_port = MQTT_BROKER_PORT;

    /* 默认连接 1883；恢复实验可传入独立端口，避免影响已有 Broker。 */
    if (argc > 2 ||
        (argc == 2 && parse_port(argv[1], &broker_port) != 0))
    {
        fprintf(stderr, "usage: %s [broker-port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* reconnect_delay 采用 1、2、4、8、8... 秒的指数退避。 */
    const mqtt_publisher_config_t config = {
        .host = MQTT_BROKER_HOST,
        .port = broker_port,
        .client_id = MQTT_CLIENT_ID,
        .topic_prefix = MQTT_TOPIC_PREFIX,
        .keepalive_seconds = MQTT_KEEPALIVE_SECONDS,
        .reconnect_delay_seconds = MQTT_RECONNECT_DELAY_SECONDS,
        .reconnect_delay_max_seconds = MQTT_RECONNECT_DELAY_MAX_SECONDS
    };
    mqtt_publisher_t *publisher = NULL;
    uint32_t sequence = 1;
    int exit_code = EXIT_FAILURE;

    /* create 复制配置并创建同步对象，但此时尚未发起网络连接。 */
    publisher = mqtt_publisher_create(&config);
    if (publisher == NULL)
    {
        fprintf(stderr, "mqtt_publisher_create failed\n");
        return EXIT_FAILURE;
    }

    /* start 启动异步连接和后台网络循环，连接结果稍后由回调报告。 */
    if (mqtt_publisher_start(publisher) != MQTT_PUBLISHER_OK)
    {
        fprintf(stderr, "mqtt_publisher_start failed\n");
        goto CLEANUP;
    }

    /*
     * 外层循环负责“采集一次并创建一条新业务消息”。只要 sequence 不变，
     * measurement 就不再修改；这样同一个 (device_id, sequence) 永远对应
     * 相同的 timestamp、value，重新序列化后也得到相同的完整 JSON。
     */
    while (sequence <= MEASUREMENT_COUNT)
    {
        /* 采集时间和值只发生在外层；重试期间禁止重新采集。 */
        const int64_t timestamp_ms = current_timestamp_ms();
        if (timestamp_ms <= 0)
        {
            perror("clock_gettime");
            goto CLEANUP;
        }

        /* const 从类型层面保证内层重试不能修改这条业务消息。 */
        const measurement_t measurement =
            create_measurement(sequence, timestamp_ms);
        printf("MEASUREMENT_CREATED sequence=%u timestamp_ms=%lld "
               "value=%.1f\n",
               measurement.sequence,
               (long long)measurement.timestamp_ms,
               measurement.value);
        fflush(stdout);

        /*
         * 内层循环只重试上面已经创建好的 measurement。若采集了新时间或
         * 新值，就必须退出本次业务消息并使用新的 sequence。
         */
        for (;;)
        {
            mqtt_publisher_result_t result;

            /* 首次连接及后续自动重连都通过同一等待函数处理。 */
            if (wait_until_connected(publisher) != 0)
                goto CLEANUP;

            /* publish 使用 QoS 1，并同步等待对应消息 ID 的 PUBACK。 */
            result = mqtt_publisher_publish(
                publisher,
                &measurement,
                MQTT_OPERATION_TIMEOUT_SECONDS);

            if (result == MQTT_PUBLISHER_OK)
                break;

            /*
             * 超时或断线时不修改 measurement，而是重试同一业务消息。
             * QoS 1 可能重复投递，接收端可用 (device_id, sequence) 去重。
             */
            if (result == MQTT_PUBLISHER_TIMEOUT ||
                result == MQTT_PUBLISHER_NOT_CONNECTED)
            {
                fprintf(stderr,
                        "publish sequence=%u interrupted; "
                        "retrying same measurement after reconnect\n",
                        sequence);
                continue;
            }

            fprintf(stderr,
                    "publish sequence=%u failed: %d\n",
                    sequence,
                    result);
            goto CLEANUP;
        }

        printf("PUBLISH_CONFIRMED sequence=%u timestamp_ms=%lld "
               "value=%.1f\n",
               measurement.sequence,
               (long long)measurement.timestamp_ms,
               measurement.value);
        fflush(stdout);

        /* 只有收到 PUBACK 后才完成当前业务消息并分配下一个 sequence。 */
        ++sequence;
        if (sequence <= MEASUREMENT_COUNT &&
            sleep_between_publishes() != 0)
        {
            perror("nanosleep");
            goto CLEANUP;
        }
    }

    puts("published 10 measurements successfully");
    exit_code = EXIT_SUCCESS;

CLEANUP:
    /* destroy 内部会停止网络线程并释放 libmosquitto 客户端。 */
    mqtt_publisher_destroy(publisher);
    return exit_code;
}
