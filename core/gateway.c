#define _POSIX_C_SOURCE 200809L

#ifdef EDGEVISION_ENABLE_MQTT
#include "measurement_source.h"
#include "mqtt_publisher.h"
#include "simulated_source.h"
#endif

#include "gateway.h"

#include "address.h"
#include "async_logger.h"
#include "frame.h"
#include "graceful_shutdown.h"
#include "measurement.h"
#include "measurement_json.h"
#include "socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Smoke 模式在同一条 TCP 连接上连续发送和处理的模拟测量数量。 */
#define GATEWAY_SMOKE_MEASUREMENT_COUNT 100u
#define GATEWAY_MQTT_SMOKE_MEASUREMENT_COUNT 100u

#ifdef EDGEVISION_ENABLE_MQTT

static int gateway_publish_with_retry(
    mqtt_publisher_t *publisher,
    const measurement_t *measurement,
    unsigned int max_attempts)
{
    unsigned int attempt;

    if (publisher == NULL ||
        measurement == NULL ||
        max_attempts == 0u)
    {
        return -1;
    }


    for (attempt = 1u; attempt <= max_attempts; ++attempt)
    {
        mqtt_publisher_result_t result;

        if (graceful_shutdown_requested())
        {
            return -1;
        }
        result = mqtt_publisher_wait_connected(publisher, 1);

        if (graceful_shutdown_requested())
        {
            return -1;
        }
        if (result == MQTT_PUBLISHER_TIMEOUT ||
            result == MQTT_PUBLISHER_NOT_CONNECTED)
        {
            fprintf(stderr,
                    "MQTT unavailable; retry %u/%u\n",
                    attempt,
                    max_attempts);
            continue;
        }

        if (result != MQTT_PUBLISHER_OK)
        {
            return -1;
        }

        result = mqtt_publisher_publish(
            publisher,
            measurement,
            1);

        if (result == MQTT_PUBLISHER_OK)
        {
            return 0;
        }

        if (result == MQTT_PUBLISHER_TIMEOUT ||
            result == MQTT_PUBLISHER_NOT_CONNECTED)
        {
            fprintf(stderr,
                    "MQTT publish interrupted; retry %u/%u\n",
                    attempt,
                    max_attempts);
            continue;
        }

        /* 参数、序列化或底层库错误不进行盲目重试。 */
        return -1;
    }

    return -1;
}

static int run_mqtt_smoke(const gateway_config_t *gateway_config)
{
    const mqtt_publisher_config_t publisher_config = {
        .host = gateway_config->mqtt_host,
        .port = gateway_config->mqtt_port,
        .client_id = "edgevision-gateway-d35",
        .topic_prefix = "edgevision/v1/devices",
        .keepalive_seconds = 30,
        .reconnect_delay_seconds = 1u,
        .reconnect_delay_max_seconds = 8u
    };

    simulated_source_t simulated;
    measurement_source_t source;
    mqtt_publisher_t *publisher = NULL;
    int result = -1;

    simulated_source_init(&simulated);
    source = simulated_source_as_measurement_source(&simulated);

    publisher = mqtt_publisher_create(&publisher_config);
    if (publisher == NULL)
    {
        fprintf(stderr, "failed to create MQTT publisher\n");
        goto DONE;
    }

    if (mqtt_publisher_start(publisher) != MQTT_PUBLISHER_OK)
    {
        fprintf(stderr, "failed to start MQTT publisher\n");
        goto DONE;
    }
    for (size_t i = 0; i < GATEWAY_MQTT_SMOKE_MEASUREMENT_COUNT; i++)
    {
        measurement_t pending_measurement;

        if (measurement_source_next(
                &source,
                &pending_measurement) != MEASUREMENT_SOURCE_OK)
        {
            fprintf(stderr, "failed to obtain simulated Measurement\n");
            goto DONE;
        }

        if (gateway_publish_with_retry(
                publisher,
                &pending_measurement,
                3u) != 0)
        {
            fprintf(stderr, "failed to publish simulated Measurement\n");
            goto DONE;
        }
    }
    printf("published %u simulated Measurements\n",
           GATEWAY_MQTT_SMOKE_MEASUREMENT_COUNT);

    result = 0;

DONE:
    mqtt_publisher_destroy(publisher);
    return result;
}

#endif

/**
 * @brief 等待进程收到 SIGINT 或 SIGTERM 停止请求。
 *
 * 每 100 ms 检查一次 graceful_shutdown_requested()。nanosleep() 被普通
 * 信号中断时会继续睡完剩余时间；如果停止信号已经到达则立即返回。
 *
 * @return 0 表示收到停止请求；-1 表示 nanosleep() 发生非 EINTR 错误，
 *         errno 保留具体原因。
 */
static int wait_for_stop(void)
{
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000L};

    while (!graceful_shutdown_requested())
    {
        struct timespec remaining = interval;

        while (nanosleep(&remaining, &remaining) != 0)
        {
            if (errno == EINTR)
            {
                if (graceful_shutdown_requested())
                    return 0;
                continue;
            }
            return -1;
        }
    }
    return 0;
}

/** 一条 TCP 连接及其独占的增量帧解析状态。 */
typedef struct
{
    int fd;                 /* 已连接的 TCP socket。 */
    frame_parser_t parser;  /* 此连接独占的增量帧解析状态。 */
} gateway_connection_t;

/**
 * @brief 判断两条 Measurement 的业务字段是否完全一致。
 *
 * 不直接 memcmp() 整个结构体，避免结构体填充字节影响比较结果。
 * 该函数用于 smoke 测试确认 JSON 编解码没有遗漏或篡改任何字段。
 */
static int measurement_equals(const measurement_t *left,
                              const measurement_t *right)
{
    return left->schema_version == right->schema_version &&
           strcmp(left->device_id, right->device_id) == 0 &&
           left->sequence == right->sequence &&
           left->timestamp_ms == right->timestamp_ms &&
           strcmp(left->metric, right->metric) == 0 &&
           left->value == right->value &&
           strcmp(left->unit, right->unit) == 0 &&
           left->quality == right->quality;
}

/**
 * @brief 按下标生成一条可重复验证的模拟 Measurement。
 *
 * 序号从 1 连续增长到 100，时间戳和数值也随下标变化，quality 则循环经过
 * good、uncertain 和 bad。这样接收端不能靠比较同一个固定样本“碰巧通过”。
 * 0.125 可以被二进制浮点数精确表示，适合在 JSON 往返后直接比较。
 *
 * @param index 从 0 开始的模拟数据下标。
 * @param measurement 输出的 Measurement，不能为空。
 */
static void build_simulated_measurement(size_t index,
                                        measurement_t *measurement)
{
    const measurement_t simulated = {
        .schema_version = MEASUREMENT_SCHEMA_VERSION,
        .device_id = "sim-temperature-01",
        .sequence = (uint32_t)(index + 1u),
        .timestamp_ms = INT64_C(1787623200000) + (int64_t)index,
        .metric = "temperature",
        .value = 20.0 + (double)index * 0.125,
        .unit = "celsius",
        .quality = (measurement_quality_t)(index % 3u)
    };

    *measurement = simulated;
}

/**
 * @brief 初始化一条 Gateway TCP 连接的协议解析上下文。
 *
 * @param connection 调用方分配的连接上下文，不能为空。
 * @param fd 已连接的 TCP socket 文件描述符；所有权仍属于调用方。
 */
static void gateway_connection_init(gateway_connection_t *connection, int fd)
{
    connection->fd = fd;
    frame_parser_init(&connection->parser);
}

/**
 * @brief 从 TCP 字节流中阻塞读取并还原一个完整应用层 payload。
 *
 * @param connection 已由 gateway_connection_init() 初始化的连接上下文。
 * @param payload 接收 payload 的输出缓冲区，容量至少为 FRAME_MAX_PAYLOAD。
 * @param payload_size 输出实际 payload 字节数，不能为空。
 * @return 0 表示成功得到一帧；-1 表示连接关闭、recv 失败或解析缓冲区溢出。
 */
static int gateway_connection_recv_frame(gateway_connection_t *connection,
                                         unsigned char *payload,
                                         size_t *payload_size)
{
    unsigned char recv_buffer[7];

    for (;;)
    {
        frame_result_t parse_result;
        ssize_t received;

        /* 先排空已缓存帧，支持一次 recv() 带回多个应用帧。 */
        parse_result = frame_parser_next(&connection->parser,
                                         payload,
                                         payload_size);
        if (parse_result == FRAME_READY)
            return 0;
        if (parse_result == FRAME_INVALID)
            continue;

        /* 小接收块让 smoke 稳定覆盖半帧；生产代码可使用更大的块。 */
        received = net_recv(connection->fd,
                            recv_buffer,
                            sizeof(recv_buffer));
        if (received <= 0)
            return -1;
        if (frame_parser_feed(&connection->parser,
                              recv_buffer,
                              (size_t)received) != FRAME_READY)
        {
            errno = EMSGSIZE;
            return -1;
        }
    }
}

/**
 * @brief 编码一个 payload 并通过 TCP 完整发送协议帧。
 *
 * @param fd 已连接的 TCP socket 文件描述符。
 * @param payload 待发送的业务数据。
 * @param payload_size 业务数据字节数，不能超过 FRAME_MAX_PAYLOAD。
 * @return 0 表示完整发送成功；-1 表示编码或发送失败。
 */
static int send_frame(int fd,
                      const unsigned char *payload,
                      size_t payload_size)
{
    unsigned char frame[FRAME_MAX_SIZE];
    size_t frame_size = 0;

    if (frame_encode(payload,
                     payload_size,
                     frame,
                     sizeof(frame),
                     &frame_size) != FRAME_READY)
    {
        errno = EMSGSIZE;
        return -1;
    }
    return net_send_all(fd, frame, frame_size);
}

/**
 * @brief 安全关闭一个可能已经打开的文件描述符。
 *
 * @param fd 文件描述符变量的地址；允许为 NULL。关闭后会把 *fd 设为 -1。
 */
static void close_if_open(int *fd)
{
    if (fd != NULL && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

/**
 * @brief 记录并打印 smoke 测试某一步的 errno 错误。
 *
 * @param logger 已初始化的异步日志器。
 * @param step 失败步骤名称。
 * @return 固定返回 -1。
 */
static int log_smoke_error(async_logger_t *logger, const char *step)
{
    char message[256];

    snprintf(message,
             sizeof(message),
             "gateway smoke failed at %s: %s",
             step,
             strerror(errno));
    (void)async_logger_log(logger, LOG_LEVEL_ERROR, message);
    fprintf(stderr, "%s\n", message);
    return -1;
}

/**
 * @brief 执行 Gateway 的本地 TCP + 协议帧双向整链自检。
 *
 * 客户端在同一条 TCP 连接上连续发送 100 个 Measurement 帧，Gateway 逐条
 * 增量解析并返回 100 个 MeasurementAck 帧。三段式“先连续发送、再连续
 * 处理、最后连续接收响应”会让内核 TCP 缓冲区中同时存在多个帧，从而覆盖
 * 跨帧边界的连续流处理，而不仅是重复执行 100 次单帧测试。
 *
 * @param logger 已初始化的异步日志器。
 * @return 0 表示请求与响应整链通过；-1 表示任一步失败。
 */
static int run_network_smoke(async_logger_t *logger)
{
    measurement_t request_measurement;
    measurement_t received_measurement = {0};
    char *request_json = NULL;
    size_t request_json_size = 0;
    char response[64];
    int response_length;
    net_address_t bind_address;
    net_address_t bound_address;
    net_address_t connect_address;
    gateway_connection_t gateway_connection;
    gateway_connection_t client_connection;
    socklen_t bound_length = sizeof(bound_address.addr);
    unsigned char payload[FRAME_MAX_PAYLOAD];
    size_t payload_size = 0;
    char success_message[256];
    int listen_fd = -1;
    int client_fd = -1;
    int accepted_fd = -1;
    int result = -1;

    /* 端口 0 让内核选择空闲端口，避免 smoke test 与本机服务冲突。 */
    if (net_address_ipv4(&bind_address, "127.0.0.1", 0) != 0)
    {
        (void)log_smoke_error(logger, "address setup");
        goto DONE;
    }

    listen_fd = net_tcp_socket();
    if (listen_fd < 0)
    {
        (void)log_smoke_error(logger, "socket");
        goto DONE;
    }
    if (net_bind(listen_fd, &bind_address) != 0)
    {
        (void)log_smoke_error(logger, "bind");
        goto DONE;
    }
    if (net_listen(listen_fd, 4) != 0)
    {
        (void)log_smoke_error(logger, "listen");
        goto DONE;
    }

    /* 读取端口 0 实际分配到的端口，供本地客户端建立回环连接。 */
    if (getsockname(listen_fd,
                    net_address_sockaddr_mut(&bound_address),
                    &bound_length) != 0 ||
        net_address_ipv4(&connect_address,
                         "127.0.0.1",
                         ntohs(bound_address.addr.sin_port)) != 0)
    {
        (void)log_smoke_error(logger, "resolve bound port");
        goto DONE;
    }

    client_fd = net_tcp_socket();
    if (client_fd < 0 || net_connect(client_fd, &connect_address) != 0)
    {
        (void)log_smoke_error(logger, "connect");
        goto DONE;
    }
    accepted_fd = net_accept(listen_fd);
    if (accepted_fd < 0)
    {
        (void)log_smoke_error(logger, "accept");
        goto DONE;
    }

    gateway_connection_init(&gateway_connection, accepted_fd);
    gateway_connection_init(&client_connection, client_fd);

    /*
     * 第一阶段：模拟设备连续发送 100 帧，中间不等待 Ack。每条结构体都先
     * 转 JSON 再封帧；JSON 动态内存在 send_frame() 返回后即可释放，因为
     * net_send_all() 已经把完整帧复制进内核的 TCP 发送缓冲区。
     */
    for (size_t index = 0;
         index < GATEWAY_SMOKE_MEASUREMENT_COUNT;
         ++index)
    {
        build_simulated_measurement(index, &request_measurement);

        if (measurement_to_json(&request_measurement,
                                &request_json,
                                &request_json_size) != MEASUREMENT_JSON_OK ||
            send_frame(client_fd,
                       (const unsigned char *)request_json,
                       request_json_size) != 0)
        {
            errno = EPROTO;
            (void)log_smoke_error(logger, "continuous Measurement send");
            goto DONE;
        }

        measurement_json_free(request_json);
        request_json = NULL;
    }

    /*
     * 第二阶段：Gateway 从同一字节流逐条恢复帧、解析 JSON、核对业务字段，
     * 并用实际收到的 sequence 生成对应 Ack。
     */
    for (size_t index = 0;
         index < GATEWAY_SMOKE_MEASUREMENT_COUNT;
         ++index)
    {
        build_simulated_measurement(index, &request_measurement);

        if (gateway_connection_recv_frame(&gateway_connection,
                                          payload,
                                          &payload_size) != 0 ||
            measurement_from_json((const char *)payload,
                                  payload_size,
                                  &received_measurement) != MEASUREMENT_JSON_OK ||
            !measurement_equals(&received_measurement, &request_measurement))
        {
            errno = EPROTO;
            (void)log_smoke_error(logger, "continuous Measurement receive");
            goto DONE;
        }

        response_length = snprintf(
            response,
            sizeof(response),
            "MeasurementAck{sequence=%u,status=accepted}",
            received_measurement.sequence);
        if (response_length < 0 ||
            (size_t)response_length >= sizeof(response) ||
            send_frame(accepted_fd,
                       (const unsigned char *)response,
                       (size_t)response_length) != 0)
        {
            errno = EPROTO;
            (void)log_smoke_error(logger, "continuous Measurement Ack send");
            goto DONE;
        }
    }

    /* 第三阶段：模拟设备按序接收并校验 100 个 Ack，防止漏帧或乱序。 */
    for (size_t index = 0;
         index < GATEWAY_SMOKE_MEASUREMENT_COUNT;
         ++index)
    {
        response_length = snprintf(
            response,
            sizeof(response),
            "MeasurementAck{sequence=%u,status=accepted}",
            (unsigned int)(index + 1u));
        if (response_length < 0 ||
            (size_t)response_length >= sizeof(response) ||
            gateway_connection_recv_frame(&client_connection,
                                          payload,
                                          &payload_size) != 0 ||
            payload_size != (size_t)response_length ||
            memcmp(payload, response, payload_size) != 0)
        {
            errno = EPROTO;
            (void)log_smoke_error(logger, "continuous Measurement Ack receive");
            goto DONE;
        }
    }

    /* 计数是本次连续处理验收的一部分，避免循环提前结束却仍报告成功。 */
    if (gateway_connection.parser.stats.frame_ok !=
            GATEWAY_SMOKE_MEASUREMENT_COUNT ||
        client_connection.parser.stats.frame_ok !=
            GATEWAY_SMOKE_MEASUREMENT_COUNT)
    {
        errno = EPROTO;
        (void)log_smoke_error(logger, "continuous Measurement frame counts");
        goto DONE;
    }

    snprintf(success_message,
             sizeof(success_message),
             "gateway smoke passed: framed Measurement loopback; "
             "measurements=%u ingress_frames=%zu response_frames=%zu",
             GATEWAY_SMOKE_MEASUREMENT_COUNT,
             gateway_connection.parser.stats.frame_ok,
             client_connection.parser.stats.frame_ok);
    if (async_logger_log(logger, LOG_LEVEL_INFO, success_message) != 0)
    {
        fprintf(stderr, "failed to submit smoke success log\n");
        goto DONE;
    }
    result = 0;

DONE:
    /* measurement_to_json() 返回动态内存，所有成功/失败路径统一释放。 */
    measurement_json_free(request_json);
    close_if_open(&accepted_fd);
    close_if_open(&client_fd);
    close_if_open(&listen_fd);
    return result;
}

/**
 * @brief 按配置执行 Gateway 的完整生命周期。
 *
 * @param config 运行模式、日志路径和队列容量。
 * @return 0 表示正常退出或 smoke 通过；-1 表示失败。
 */
int gateway_run(const gateway_config_t *config)
{
    async_logger_t logger;
    int result = -1;
    int valid_mode;
    valid_mode = config != NULL && (config->mode == GATEWAY_MODE_SERVICE || config->mode == GATEWAY_MODE_SMOKE);
#ifdef EDGEVISION_ENABLE_MQTT
    if (config != NULL &&
        config->mode == GATEWAY_MODE_MQTT_SMOKE)
    {
        valid_mode = 1;
    }
#endif

    if (config == NULL ||
        config->log_path == NULL ||
        config->log_queue_capacity == 0 ||
        !valid_mode)
    {
        errno = EINVAL;
        return -1;
    }

#ifdef EDGEVISION_ENABLE_MQTT
    if (config->mode == GATEWAY_MODE_MQTT_SMOKE &&
        (config->mqtt_host == NULL ||
         config->mqtt_host[0] == '\0' ||
         config->mqtt_port < 1 ||
         config->mqtt_port > 65535))
    {
        errno = EINVAL;
        return -1;
    }
#endif

    if (graceful_shutdown_install() != 0)
    {
        perror("graceful_shutdown_install");
        return -1;
    }
    if (async_logger_init(&logger,
                          config->log_path,
                          config->log_queue_capacity) != 0)
    {
        fprintf(stderr,
                "async_logger_init failed for %s\n",
                config->log_path);
        return -1;
    }
    if (async_logger_log(&logger, LOG_LEVEL_INFO, "gateway started") != 0)
    {
        fprintf(stderr, "failed to submit startup log\n");
        goto SHUTDOWN;
    }

    if (config->mode == GATEWAY_MODE_SMOKE)
    {
        result = run_network_smoke(&logger);
        goto SHUTDOWN;
    }

#ifdef EDGEVISION_ENABLE_MQTT
    if (config->mode == GATEWAY_MODE_MQTT_SMOKE)
    {
        result = run_mqtt_smoke(config);
        goto SHUTDOWN;
    }
#endif

    printf("gateway running; send SIGINT or SIGTERM to stop\n");
    fflush(stdout);
    if (wait_for_stop() != 0)
    {
        fprintf(stderr, "wait_for_stop failed: %s\n", strerror(errno));
        goto SHUTDOWN;
    }
    if (async_logger_log(&logger,
                         LOG_LEVEL_INFO,
                         "shutdown requested; draining logger") != 0)
    {
        fprintf(stderr, "failed to submit shutdown log\n");
        goto SHUTDOWN;
    }
    result = 0;

SHUTDOWN:
    if (async_logger_shutdown(&logger) != 0)
    {
        fprintf(stderr, "async_logger_shutdown failed\n");
        result = -1;
    }
    if (async_logger_destroy(&logger) != 0)
    {
        fprintf(stderr, "async_logger_destroy failed\n");
        result = -1;
    }

    if (config->mode == GATEWAY_MODE_SMOKE ||
        config->mode == GATEWAY_MODE_MQTT_SMOKE)
    {
        printf("%s\n", result == 0 ? "SMOKE_PASS" : "SMOKE_FAIL");
    }
    else if (result == 0)
    {
        printf("gateway stopped cleanly\n");
    }
    return result;
}
