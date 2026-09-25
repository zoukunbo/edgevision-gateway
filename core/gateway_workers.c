#define _POSIX_C_SOURCE 200809L

#include "gateway_workers.h"

#include "blocking_queue.h"
#include "command_socket_path.h"
#include "cJSON.h"
#include "graceful_shutdown.h"
#include "mqtt_command_receiver.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>

#define GATEWAY_MEASUREMENT_QUEUE_CAPACITY 32u
#define GATEWAY_DELIVERY_QUEUE_CAPACITY 1u
#define GATEWAY_RESULT_QUEUE_CAPACITY 1u
#define GATEWAY_QUEUE_POLL_MS 100
#define GATEWAY_SOURCE_INTERVAL_MS 1000
#define GATEWAY_DELIVERY_RETRY_MS 1000
#define GATEWAY_PUBLISH_TIMEOUT_SECONDS 3
#define GATEWAY_COMMAND_IO_TIMEOUT_SECONDS 3
#define GATEWAY_MQTT_COMMAND_QUEUE_CAPACITY 8u
#define GATEWAY_MQTT_COMMAND_TOPIC_CAPACITY 384u
#define GATEWAY_MQTT_COMMAND_PAYLOAD_CAPACITY 1024u
#define GATEWAY_COMMAND_RESPONSE_CAPACITY 256u
#define GATEWAY_COMMAND_DEDUP_CAPACITY 16u
#define GATEWAY_COMMAND_REQUEST_ID_CAPACITY 64u

#ifndef EDGEVISION_VERSION
#define EDGEVISION_VERSION "development"
#endif

/* storage worker 交给 MQTT worker 的待投递任务。 */
typedef struct
{
    outbox_item_t item;
} gateway_delivery_job_t;

/* MQTT worker 返回给 storage worker 的投递结果。 */
typedef struct
{
    int64_t outbox_id;
    mqtt_publisher_result_t publish_result;
    int64_t sent_at_ms;
} gateway_delivery_report_t;

typedef enum
{
    GATEWAY_STORAGE_CONTROL_SET_INTERVAL,
    GATEWAY_STORAGE_CONTROL_GET_STATS
}gateway_storage_control_kind_t;

typedef struct
{
    gateway_storage_control_kind_t kind;
    int interval_ms;
} gateway_storage_control_request_t;

typedef struct
{
    gateway_storage_control_kind_t kind;
    outbox_store_result_t storage_result;
    int interval_ms;
    outbox_store_stats_t stats;
} gateway_storage_control_result_t;


typedef struct
{
    char topic[GATEWAY_MQTT_COMMAND_TOPIC_CAPACITY];

    /*
     * payload_length 不包含末尾 '\0'。
     * payload 数组额外保留 '\0'，方便 worker 使用 cJSON 解析。
     */
    char payload[GATEWAY_MQTT_COMMAND_PAYLOAD_CAPACITY];
    size_t payload_length;
} gateway_mqtt_command_request_t;

/*
 * 最近处理过的远程请求。
 *
 * QoS 1 可能重复投递同一请求。缓存 request_id、原命令和已经生成的响应：
 * 同一 ID、同一命令只重发原响应；同一 ID、不同命令视为 ID 冲突。
 * 记录只在当前进程内有效，写满后按插入顺序覆盖，不提供重启后去重。
 */
typedef struct
{
    bool occupied;
    char request_id[GATEWAY_COMMAND_REQUEST_ID_CAPACITY];
    char command[GATEWAY_MQTT_COMMAND_PAYLOAD_CAPACITY];
    char response[GATEWAY_COMMAND_RESPONSE_CAPACITY];
} gateway_command_dedup_entry_t;

typedef enum
{
    /* 内部参数或查询过程无效，调用者不能继续执行命令。 */
    GATEWAY_COMMAND_DEDUP_ERROR = -1,
    /* 没有找到 request_id，调用者可以执行并缓存新命令。 */
    GATEWAY_COMMAND_DEDUP_MISS = 0,
    /* request_id 和 command 都相同，response_out 指向原响应。 */
    GATEWAY_COMMAND_DEDUP_MATCH = 1,
    /* request_id 相同但 command 不同，调用者应拒绝执行。 */
    GATEWAY_COMMAND_DEDUP_CONFLICT = 2
} gateway_command_dedup_result_t;

/*
 * 三个 worker 共享的运行上下文。
 *
 * 数据流如下：
 * MeasurementSource -> measurement_queue -> Outbox Store
 * Outbox Store       -> delivery_queue    -> MQTT Publisher
 * MQTT Publisher     -> result_queue      -> Outbox Store
 *
 * Store 只由 storage worker 访问，无需额外的并发控制。
 * Publisher 由数据投递 worker 和远程命令 worker 共享；
 * mqtt_publisher 内部使用 publish_mutex 串行发布。
 */
typedef struct
{
    outbox_store_t *store;
    measurement_source_t *source;
    mqtt_publisher_t *publisher;
    mqtt_command_receiver_t *command_receiver;

    bounded_queue_t measurement_queue;
    bounded_queue_t delivery_queue;
    bounded_queue_t result_queue;
    bounded_queue_t storage_control_request_queue;
    bounded_queue_t storage_control_result_queue;
    bounded_queue_t mqtt_command_queue;

    pthread_mutex_t status_mutex;
    pthread_mutex_t command_mutex;
    int stop_requested;
    int fatal_error;
    uint64_t collected_count;
    int interval_ms;

    const char *mqtt_command_response_topic;
    gateway_command_dedup_entry_t
    command_dedup_entries[GATEWAY_COMMAND_DEDUP_CAPACITY];

    /* 下次写入的缓存位置；写满后从头覆盖最旧记录。 */
    size_t command_dedup_next;

    bool collection_paused;
} gateway_workers_t;

/*
 * MATCH 时 response_out 借用缓存槽位中的字符串，不需要调用者释放；
 * 该指针只在对应槽位被环形缓存覆盖前有效。其他结果保持为 NULL。
 */
static gateway_command_dedup_result_t
gateway_find_cached_command_response(
    const gateway_workers_t *workers,
    const char *request_id,
    const char *command,
    const char **response_out)
{
    if (workers == NULL ||
        request_id == NULL ||
        command == NULL ||
        response_out == NULL)
    {
        return GATEWAY_COMMAND_DEDUP_ERROR;
    }
    *response_out = NULL;

    for (size_t index = 0;
     index < GATEWAY_COMMAND_DEDUP_CAPACITY;
     ++index)
    {
        const gateway_command_dedup_entry_t *entry =
            &workers->command_dedup_entries[index];

        /* 在这里判断 occupied 和 request_id */
        if (entry->occupied && strcmp(entry->request_id, request_id) == 0)
        {
            if (strcmp(entry->command, command) == 0)
            {
                *response_out = entry->response;
                return GATEWAY_COMMAND_DEDUP_MATCH;
            }
            return GATEWAY_COMMAND_DEDUP_CONFLICT;
        }

    }
    return GATEWAY_COMMAND_DEDUP_MISS;
}

/*
 * 先验证三个字符串都能完整放入槽位，再一次性写入当前槽位。
 * 保存成功后才推进 command_dedup_next，失败不会占用缓存位置。
 */
static bool gateway_cache_command_response(
    gateway_workers_t *workers,
    const char *request_id,
    const char *command,
    const char *response)
{
    if (workers == NULL ||
        request_id == NULL ||
        response == NULL ||
        command == NULL ||
        request_id[0] == '\0' ||
        response[0] == '\0' ||
        command[0] == '\0'
    )
    {
        return false;
    }

    gateway_command_dedup_entry_t *entry = &workers->command_dedup_entries[workers->command_dedup_next];

    // strnlen() 最多检查目标数组容量，不会无限向后寻找 '\0'。
    // 返回值等于容量，表示在可容纳范围内没有找到结尾。
    size_t request_id_length =
    strnlen(request_id, sizeof(entry->request_id));
    size_t command_length =
    strnlen(command, sizeof(entry->command));

    size_t response_length =
        strnlen(response, sizeof(entry->response));

    if (request_id_length == sizeof(entry->request_id) ||
        response_length == sizeof(entry->response)||
        command_length == sizeof(entry->command))
    {
        return false;
    }

     // length + 1 会把字符串结尾的 '\0' 一起复制。
    memcpy(entry->request_id, request_id, request_id_length + 1);
    memcpy(entry->command, command, command_length + 1);
    memcpy(entry->response, response, response_length + 1);

    entry->occupied = true;


    workers->command_dedup_next =
    (workers->command_dedup_next + 1) %
    GATEWAY_COMMAND_DEDUP_CAPACITY;
    return true;
}

static void gateway_mqtt_command_received(
    void *context,
    const char *topic,
    const void *payload,
    size_t payload_length)
{
    if (context == NULL ||
        topic == NULL ||
        (payload_length > 0 && payload == NULL))
    {
        return;
    }
    gateway_workers_t * workers = context;
    gateway_mqtt_command_request_t request = {0};
    size_t topic_length = strnlen(topic, sizeof(request.topic));

    if (topic_length == sizeof(request.topic)||
        payload_length >= sizeof(request.payload))
    {
        return;
    }

    memcpy(request.topic, topic, topic_length);
    request.topic[topic_length] = '\0';

    if (payload_length > 0)
    {
        memcpy(request.payload, payload, payload_length);
    }

    request.payload[payload_length] = '\0';
    request.payload_length = payload_length;

    (void)bq_push(&workers->mqtt_command_queue, &request, 0);
}

typedef struct
{
    int interval_ms;
    uint64_t collected_count;
    bool collection_paused;
} gateway_status_snapshot_t;

/* 在同一个锁保护期内复制状态，避免 command worker 读到不一致数据。 */
static int gateway_workers_get_status(
    gateway_workers_t *workers,
    gateway_status_snapshot_t *snapshot)
{
    if (workers == NULL || snapshot == NULL)
    {
        return -1;
    }

    if (pthread_mutex_lock(&workers->status_mutex) != 0)
    {
        return -1;
    }

    snapshot->interval_ms = workers->interval_ms;
    snapshot->collected_count = workers->collected_count;
    snapshot->collection_paused = workers->collection_paused;

    if (pthread_mutex_unlock(&workers->status_mutex) != 0)
    {
        return -1;
    }

    return 0;
}

static int gateway_workers_set_interval(gateway_workers_t *workers, int requested_ms)
{
    if (workers == NULL)
    {
        return -1;
    }

    if (requested_ms < 100 || requested_ms > 60000)
    {
        return 1;
    }


    if (pthread_mutex_lock(&workers->status_mutex) != 0)
        return -1;

    workers->interval_ms = requested_ms;


    if (pthread_mutex_unlock(&workers->status_mutex) != 0)
        return -1;

    return 0;
}

/* 成功返回 0；格式错误或超出 int 范围返回 1。 */
static int parse_interval_ms(const char *text, int *value)
{
    if (text == NULL || value == NULL || text[0] == '\0')
        return 1;

    /* 第一版只接受数字，不接受正负号、空格或其他字符。 */
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return 1;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);

    if (errno == ERANGE || *end != '\0' || parsed > INT_MAX)
        return 1;

    *value = (int)parsed;
    return 0;
}

static bool gateway_workers_should_stop(gateway_workers_t *workers)
{
    int requested;

    pthread_mutex_lock(&workers->status_mutex);
    requested = workers->stop_requested;
    pthread_mutex_unlock(&workers->status_mutex);

    return requested != 0 || graceful_shutdown_requested();
}

static int gateway_workers_set_collection_paused(
    gateway_workers_t *workers,
    bool paused)
{
    if (pthread_mutex_lock(&workers->status_mutex) != 0)
        return -1;
    workers->collection_paused = paused;
    if (pthread_mutex_unlock(&workers->status_mutex))
        return -1;
    return 0;
}

static int gateway_handle_command(
    gateway_workers_t *workers,
    const char *command,
    char *reply,
    size_t reply_size)
{
    if (workers == NULL || command == NULL || command[0] == '\0' || reply == NULL || reply_size == 0)
    {
        return -1;
    }

    int n;
    int requested_ms;
    if (strcmp(command, "status") == 0)
    {
        gateway_status_snapshot_t snapshot = {0};

        if (gateway_workers_get_status(workers, &snapshot) != 0)
        {
            return -1;
        }

        /* snprintf 返回完整输出所需字符数，不含结尾 '\0'。 */
        n = snprintf(reply,
                     reply_size,
                     "interval_ms=%d collected_count=%" PRIu64
                     " collection=%s",
                     snapshot.interval_ms,
                     snapshot.collected_count,
                    snapshot.collection_paused ? "paused" : "running");
    }
    else if (strcmp(command, "set_interval") == 0 ||
             strncmp(command, "set_interval ", 13) == 0)
    {
        if (strcmp(command, "set_interval") == 0 ||
            parse_interval_ms(command + 13, &requested_ms) != 0 ||
            requested_ms < 100 ||
            requested_ms > 60000)
        {
            n = snprintf(reply, reply_size, "error=invalid_interval");
        }
        else
        {
            gateway_storage_control_request_t request_item ={
                .interval_ms = requested_ms,
                .kind = GATEWAY_STORAGE_CONTROL_SET_INTERVAL};
            bq_result_t requested_result = bq_push(&workers->storage_control_request_queue,
                &request_item,
                GATEWAY_QUEUE_POLL_MS);

            if (requested_result != BQ_OK)
            {
                return -1;
            }

            gateway_storage_control_result_t control_result = {0};

            while (1)
            {
                bq_result_t queue_result =
                    bq_pop(&workers->storage_control_result_queue,
                    &control_result,
                    GATEWAY_QUEUE_POLL_MS);

                if (queue_result == BQ_OK)
                    break;

                if (queue_result != BQ_TIMEOUT)
                {
                    return -1;
                }

                if (gateway_workers_should_stop(workers))
                {
                    return -1;
                }
            }
            if (control_result.kind != GATEWAY_STORAGE_CONTROL_SET_INTERVAL)
            {
                return -1;
            }


            if (control_result.storage_result != OUTBOX_STORE_OK)
            {
                n = snprintf(reply, reply_size, "error=persist_failed");
            }
            else
            {
                if (control_result.kind == GATEWAY_STORAGE_CONTROL_SET_INTERVAL)
                {
                    int rc = gateway_workers_set_interval(
                        workers, control_result.interval_ms);

                    if (rc != 0)
                        return -1;

                    n = snprintf(reply, reply_size,
                                "ok interval_ms=%d",
                                control_result.interval_ms);
                }
            }
        }
    }
    else if (strcmp(command, "get_config") == 0)
    {
        gateway_status_snapshot_t snapshot = {0};

        if (gateway_workers_get_status(workers, &snapshot) != 0)
        {
            return -1;
        }

        n = snprintf(reply, reply_size,
                            "interval_ms=%d",
                            snapshot.interval_ms);
    }
    else if (strcmp(command, "pause_collection") == 0 ||
            strcmp(command, "resume_collection") == 0)
    {
        bool collection = false;
        if (strcmp(command, "pause_collection") == 0)
        {
            collection = true;
        }

        if (gateway_workers_set_collection_paused(workers, collection) != 0)
        {
            return -1;
        }

        n = snprintf(reply, reply_size,
                            "ok collection=%s",
                            collection ? "paused" : "running");
    }
    else if (strcmp(command, "get_storage_stats") == 0)
    {
        {
            gateway_storage_control_request_t request_item ={
                .kind = GATEWAY_STORAGE_CONTROL_GET_STATS};
            bq_result_t requested_result = bq_push(
                &workers->storage_control_request_queue,
                &request_item,
                GATEWAY_QUEUE_POLL_MS);

            if (requested_result != BQ_OK)
            {
                return -1;
            }

            gateway_storage_control_result_t control_result = {0};

            while (1)
            {
                bq_result_t queue_result =
                    bq_pop(&workers->storage_control_result_queue,
                    &control_result,
                    GATEWAY_QUEUE_POLL_MS);

                if (queue_result == BQ_OK)
                    break;

                if (queue_result != BQ_TIMEOUT)
                {
                    return -1;
                }

                if (gateway_workers_should_stop(workers))
                {
                    return -1;
                }
            }

            if (control_result.kind != GATEWAY_STORAGE_CONTROL_GET_STATS)
                return -1;

            if (control_result.storage_result != OUTBOX_STORE_OK)
            {
                n = snprintf(reply, reply_size, "error=stats_failed");
            }
            else
            {
                if (control_result.kind == GATEWAY_STORAGE_CONTROL_GET_STATS)
                {
                     n = snprintf(reply,
                        reply_size,
                        "measurements=%"PRIu64
                                " pending=%"PRIu64
                                " sent=%"PRIu64,
                                 control_result.stats.measurement_total,
                                control_result.stats.pending_count,
                                control_result.stats.sent_count);
                }
            }
        }
    }
    else if (strcmp(command, "get_version") == 0)
    {
        n = snprintf(reply,
                        reply_size,
                        "ok version=%s",
                        EDGEVISION_VERSION);
    }
    else
    {
        n = snprintf(reply, reply_size, "error=unknown_command");
    }

    if (n < 0 || (size_t)n >= reply_size)
    {
        return -1;
    }
    return 0;
}

static int gateway_execute_command(
    gateway_workers_t *workers,
    const char *command,
    char *reply,
    size_t reply_size
)
{
    if (pthread_mutex_lock(
            &workers->command_mutex) != 0)
    {
        return -1;
    }

    int result = gateway_handle_command(
        workers,
        command,
        reply,
        reply_size);

    if (pthread_mutex_unlock(
            &workers->command_mutex) != 0)
    {
        return -1;
    }

    return result;
}

/* 设置协作式停止标志；fatal 同时决定 gateway_workers_run() 的最终返回值。 */
static void gateway_workers_request_stop(gateway_workers_t *workers,
                                         bool fatal)
{
    pthread_mutex_lock(&workers->status_mutex);
    workers->stop_requested = 1;
    if (fatal)
        workers->fatal_error = 1;
    pthread_mutex_unlock(&workers->status_mutex);
}



static int set_io_timeout(int fd)
{
    struct timeval timeout = {
        .tv_sec = GATEWAY_COMMAND_IO_TIMEOUT_SECONDS,
        .tv_usec = 0
    };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) == -1)
        return -1;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) == -1)
        return -1;

    return 0;
}


static int send_all(gateway_workers_t *workers, int fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {

        if (gateway_workers_should_stop(workers)) {
            errno = ECANCELED;
            return -1;
        }

        ssize_t n = send(fd, data + sent,
                         length - sent, MSG_NOSIGNAL);

        if (n > 0) {
            sent += (size_t)n;
            continue;
        }

        if (n == -1 && errno == EINTR)
            continue;

        // 返回 0 表示没有进展，也作为失败，避免原地循环。
        return -1;
    }

    return 0;
}

static int recv_line(gateway_workers_t *workers, int fd, char *buffer, size_t capacity)
{
    if (workers == NULL || fd < 0 || buffer == NULL || capacity == 0)
    {
        errno = EINVAL;
        return -1;
    }

    size_t written = 0;

    while (1)
    {
        if (gateway_workers_should_stop(workers)) {
            errno = ECANCELED;
            return -1;
        }

        /* 为末尾 '\0' 保留一字节，超长命令直接拒绝。 */
        if (written + 1 >= capacity)
        {
            errno = EMSGSIZE;
            return -1;
        }

        ssize_t n = read(fd, buffer + written, 1);
        if (n < 0)
        {
           if (errno == EINTR)
           {
                continue;
           }
           return -1;
        }
        else if (n == 0)
        {
            return -1;
        }
        if (buffer[written] == '\n')
        {
            buffer[written] = '\0';
            break;
        }

        written += 1;
    }
    return 0;
}

static int gateway_serve_client(
    gateway_workers_t *workers,
    int client_fd)
{
    if (workers == NULL || client_fd < 0)
    {
        return -1;
    }

    char command[64];
    char reply[128];

    if (set_io_timeout(client_fd) == -1)
    {
        return -1;
    }

    if (recv_line(workers, client_fd, command, sizeof(command)) != 0 || gateway_workers_should_stop(workers) != 0)
    {
        return -1;
    }

    if (gateway_execute_command(workers, command, reply, sizeof(reply)) == -1)
    {
        return -1;
    }

    if (send_all(workers, client_fd, reply, strlen(reply)) == -1)
    {
        return -1;
    }

    if (send_all(workers, client_fd, "\n", 1) == -1)
    {
        return -1;
    }

    return 0;
}

static bool gateway_workers_failed(gateway_workers_t *workers)
{
    int failed;

    pthread_mutex_lock(&workers->status_mutex);
    failed = workers->fatal_error;
    pthread_mutex_unlock(&workers->status_mutex);
    return failed != 0;
}

static void gateway_worker_fail(gateway_workers_t *workers,
                                const char *message)
{
    fprintf(stderr, "%s\n", message);
    gateway_workers_request_stop(workers, true);
}

static void *gateway_command_worker(void *argument)
{
    gateway_workers_t *workers = argument;
    const char *socket_path = edgevision_command_socket_path();
    bool path_bound = false;

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd == -1) {
        gateway_worker_fail(workers, "command socket failed");
        return NULL;
    }

    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        gateway_worker_fail(workers, "command socket path too long");
        goto CLEANUP;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path),
             "%s", socket_path);

    if (bind(listen_fd, (const struct sockaddr *)&addr,
             sizeof(addr)) == -1) {
        gateway_worker_fail(workers, "command bind failed");
        goto CLEANUP;
    }
    path_bound = true;

    if (listen(listen_fd, 4) == -1) {
        gateway_worker_fail(workers, "command listen failed");
        goto CLEANUP;
    }

    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags == -1 ||
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        gateway_worker_fail(workers, "command nonblocking setup failed");
        goto CLEANUP;
    }

    struct pollfd listener = {
        .fd = listen_fd,
        .events = POLLIN
    };

    while (!gateway_workers_should_stop(workers)) {
        int ready = poll(&listener, 1, GATEWAY_QUEUE_POLL_MS);

        if (ready == -1) {
            if (errno == EINTR)
                continue;

            gateway_worker_fail(workers, "command poll failed");
            break;
        }

        if (gateway_workers_should_stop(workers))
            break;

        if (ready == 0)
            continue;

        if (listener.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            gateway_worker_fail(workers, "command listener failed");
            break;
        }

        if (!(listener.revents & POLLIN))
            continue;

        /*
         * 你在这里完成一次接待：
         * accept → gateway_serve_client → close
         */
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR ||
                errno == EAGAIN ||
                errno == EWOULDBLOCK) {
                continue;
            }
            gateway_worker_fail(workers, "command accept failed");
            goto CLEANUP;
        }

        if (!gateway_workers_should_stop(workers))
        {
            int rc = gateway_serve_client(workers, client_fd);
            if (rc != 0 && !gateway_workers_should_stop(workers))
            {
                fprintf(stderr, "client request failed\n");
            }
        }

        close(client_fd);
    }

CLEANUP:
    close(listen_fd);

    if (path_bound && unlink(socket_path) == -1)
        gateway_worker_fail(workers, "command socket cleanup failed");

    return NULL;
}

static int gateway_worker_sleep(gateway_workers_t *workers, int total_ms)
{
    /*
     * 将长等待拆成短周期，以便收到进程信号或其他 worker 的失败通知后
     * 最多在一个轮询周期内退出。
     */

    for (int elapsed = 0; elapsed < total_ms;)
    {
        if (gateway_workers_should_stop(workers))
            return 0;

        int chunk_ms = total_ms - elapsed;
        if (chunk_ms > GATEWAY_QUEUE_POLL_MS)
        {
            chunk_ms = GATEWAY_QUEUE_POLL_MS;
        }

        struct timespec remaining = {
            .tv_sec = 0,
            .tv_nsec = chunk_ms * 1000000L
        };

        while (nanosleep(&remaining, &remaining) != 0)
        {
            if (errno == EINTR)
            {
                if (gateway_workers_should_stop(workers))
                    return 0;
                continue;
            }
            return -1;
        }
        elapsed += chunk_ms;
    }

    return 0;
}

static int64_t gateway_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;

    return (int64_t)now.tv_sec * INT64_C(1000) +
           now.tv_nsec / 1000000L;
}

/* 从数据源采集 Measurement，并通过有界队列交给 storage worker。 */
static void *gateway_source_worker(void *argument)
{
    gateway_workers_t *workers = argument;

    while (!gateway_workers_should_stop(workers))
    {
        measurement_t measurement = {0};

        gateway_status_snapshot_t snapshot = {0};

        if (gateway_workers_get_status(workers, &snapshot) != 0)
        {
            gateway_worker_fail(workers, "gateway workers get status failed");
            break;
        }

        if (snapshot.collection_paused)
        {
            if (gateway_worker_sleep(workers, GATEWAY_QUEUE_POLL_MS) != 0)
            {
                gateway_worker_fail(workers, "source worker sleep failed");
                break;
            }
            continue;
        }
        measurement_source_result_t source_result =
            measurement_source_next(workers->source, &measurement);

        if (source_result == MEASUREMENT_SOURCE_NO_DATA)
        {
            if (gateway_worker_sleep(workers, GATEWAY_QUEUE_POLL_MS) != 0)
            {
                gateway_worker_fail(workers, "source worker sleep failed");
                break;
            }
            continue;
        }

        if (source_result != MEASUREMENT_SOURCE_OK)
        {
            gateway_worker_fail(workers,
                                "source worker failed to obtain Measurement");
            break;
        }

        pthread_mutex_lock(&workers->status_mutex);
        workers->collected_count++;
        pthread_mutex_unlock(&workers->status_mutex);

        for (;;)
        {
            /* 队列满时周期性重试，同时保留响应停机请求的机会。 */
            bq_result_t queue_result =
                bq_push(&workers->measurement_queue,
                        &measurement,
                        GATEWAY_QUEUE_POLL_MS);

            if (queue_result == BQ_OK)
                break;
            if (queue_result == BQ_CLOSED)
                return NULL;
            if (queue_result != BQ_TIMEOUT)
            {
                gateway_worker_fail(workers,
                                    "source worker measurement push failed");
                return NULL;
            }
            if (gateway_workers_should_stop(workers))
                return NULL;
        }


        if (gateway_workers_get_status(workers, &snapshot) != 0)
        {
            gateway_worker_fail(workers, "source worker get status failed");
            break;
        }

        if (gateway_worker_sleep(workers, snapshot.interval_ms) != 0)
        {
            gateway_worker_fail(workers, "source worker sleep failed");
            break;
        }
    }

    return NULL;
}

static int gateway_storage_save(gateway_workers_t *workers,
                                const measurement_t *measurement)
{
    /* topic 在入库时固化，后续重试无需再次依赖 MeasurementSource。 */
    char topic[256];
    int topic_length = snprintf(
        topic,
        sizeof(topic),
        "edgevision/v1/devices/%s/measurements",
        measurement->device_id);

    if (topic_length < 0 || (size_t)topic_length >= sizeof(topic))
        return -1;

    return outbox_store_save_measurement(
               workers->store, measurement, topic) == OUTBOX_STORE_OK
               ? 0
               : -1;
}

static int gateway_storage_apply_report(
    gateway_workers_t *workers,
    const gateway_delivery_report_t *report,
    bool *retry_needed)
{
    outbox_store_result_t storage_result;

    /* 发布失败会先持久化失败原因，再由调度逻辑延迟重试同一条记录。 */
    *retry_needed = report->publish_result != MQTT_PUBLISHER_OK;

    if (report->publish_result == MQTT_PUBLISHER_OK)
    {
        if (report->sent_at_ms <= 0)
            return -1;
        storage_result = outbox_store_mark_sent(
            workers->store, report->outbox_id, report->sent_at_ms);
    }
    else
    {
        char error_text[64];
        int length = snprintf(
            error_text,
            sizeof(error_text),
            "mqtt publish failed: %d",
            (int)report->publish_result);

        if (length < 0 || (size_t)length >= sizeof(error_text))
            return -1;

        storage_result = outbox_store_record_delivery_failure(
            workers->store, report->outbox_id, error_text);
    }

    return storage_result == OUTBOX_STORE_OK ? 0 : -1;
}

static int gateway_storage_schedule_delivery(
    gateway_workers_t *workers,
    bool *delivery_inflight)
{
    /* Outbox 保证按最早待发送记录的顺序进行投递。 */
    outbox_item_t item = {0};
    outbox_store_result_t storage_result =
        outbox_store_read_earliest_pending(workers->store, &item);

    if (storage_result == OUTBOX_STORE_EMPTY)
        return 0;
    if (storage_result != OUTBOX_STORE_OK)
        return -1;

    gateway_delivery_job_t job = {.item = item};
    bq_result_t queue_result =
        bq_push(&workers->delivery_queue, &job, 0);

    if (queue_result == BQ_OK)
    {
        /*
         * bounded_queue 只浅拷贝结构体。成功入队后，topic/payload_json
         * 的所有权一次性转给 MQTT worker。
         */
        *delivery_inflight = true;
        return 0;
    }

    outbox_store_free_outbox_item(&item);
    if (queue_result == BQ_TIMEOUT || queue_result == BQ_CLOSED)
        return 0;
    return -1;
}

static void *gateway_storage_worker(void *argument)
{
    gateway_workers_t *workers = argument;
    bool measurement_closed = false;
    bool result_closed = false;
    bool delivery_inflight = false;
    int64_t retry_not_before_ms = 0;

    /*
     * storage worker 是 Store 的唯一访问者。每轮优先处理投递回执，然后
     * 保存新采集数据，最后在没有在途任务时调度下一条 Outbox 记录。
     * delivery_queue/result_queue 容量均为 1，配合 delivery_inflight 保证
     * 任意时刻最多只有一条记录处于发布过程，维持 Outbox 的发送顺序。
     */
    while (!measurement_closed || !result_closed)
    {
        gateway_storage_control_request_t request = {0};

        bq_result_t queue_result = bq_pop(&workers->storage_control_request_queue, &request, 0);

        if (queue_result == BQ_OK)
        {
            gateway_storage_control_result_t control_result = {0};
            if (request.kind == GATEWAY_STORAGE_CONTROL_SET_INTERVAL)
            {
                control_result.kind = GATEWAY_STORAGE_CONTROL_SET_INTERVAL;
                control_result.interval_ms = request.interval_ms;
                control_result.storage_result = outbox_store_save_interval(workers->store, request.interval_ms);
            }
            else if (request.kind == GATEWAY_STORAGE_CONTROL_GET_STATS)
            {
                control_result.kind = GATEWAY_STORAGE_CONTROL_GET_STATS;
                control_result.storage_result = outbox_store_get_stats(workers->store, &control_result.stats);
            }
            else
            {
                gateway_worker_fail(workers, "config result kind unknowed");
                return NULL;
            }


            queue_result = bq_push(&workers->storage_control_result_queue, &control_result, 0);

            if (queue_result != BQ_OK)
            {
                gateway_worker_fail(workers, "storage config result push failed");
                return NULL;
            }
        }
        else if (queue_result != BQ_TIMEOUT && queue_result != BQ_CLOSED)
        {
            gateway_worker_fail(
                workers, "storage config request pop failed");
            return NULL;
        }

        gateway_delivery_report_t report = {0};
        bq_result_t result_queue_result =
            bq_pop(&workers->result_queue, &report, 0);

        if (result_queue_result == BQ_OK)
        {
            bool retry_needed = false;
            if (gateway_storage_apply_report(
                    workers, &report, &retry_needed) != 0)
            {
                gateway_worker_fail(
                    workers, "storage worker delivery update failed");
                return NULL;
            }

            delivery_inflight = false;
            if (retry_needed)
            {
                /* 使用单调时钟计算退避期限，避免系统时间调整影响等待。 */
                int64_t now_ms = gateway_monotonic_ms();
                if (now_ms < 0)
                {
                    gateway_worker_fail(
                        workers, "storage worker clock failed");
                    return NULL;
                }
                retry_not_before_ms = now_ms + GATEWAY_DELIVERY_RETRY_MS;
            }
            else
            {
                retry_not_before_ms = 0;
            }
        }
        else if (result_queue_result == BQ_CLOSED)
        {
            result_closed = true;
            delivery_inflight = false;
        }
        else if (result_queue_result != BQ_TIMEOUT)
        {
            gateway_worker_fail(workers,
                                "storage worker result pop failed");
            return NULL;
        }

        if (!measurement_closed)
        {
            measurement_t measurement = {0};
            bq_result_t measurement_result =
                bq_pop(&workers->measurement_queue,
                       &measurement,
                       GATEWAY_QUEUE_POLL_MS);

            if (measurement_result == BQ_OK)
            {
                if (gateway_storage_save(workers, &measurement) != 0)
                {
                    gateway_worker_fail(
                        workers, "storage worker save failed");
                    return NULL;
                }
            }
            else if (measurement_result == BQ_CLOSED)
            {
                measurement_closed = true;
            }
            else if (measurement_result != BQ_TIMEOUT)
            {
                gateway_worker_fail(
                    workers, "storage worker measurement pop failed");
                return NULL;
            }
        }

        if (!delivery_inflight &&
            !gateway_workers_should_stop(workers))
        {
            int64_t now_ms = gateway_monotonic_ms();
            if (now_ms < 0)
            {
                gateway_worker_fail(workers,
                                    "storage worker clock failed");
                return NULL;
            }

            if (now_ms >= retry_not_before_ms &&
                gateway_storage_schedule_delivery(
                    workers, &delivery_inflight) != 0)
            {
                gateway_worker_fail(
                    workers, "storage worker delivery schedule failed");
                return NULL;
            }
        }
    }

    return NULL;
}

static char *gateway_create_delivery_payload(const outbox_item_t *item)
{
    if (item == NULL || item->payload_json == NULL || item->message_id[0] == '\0')
        return NULL;

    /*
     * message_id 属于 Outbox 元数据，在真正发布前注入 payload。
     * 已含同名字段视为非法，防止调用方数据覆盖系统生成的消息标识。
     */
    cJSON *root = cJSON_Parse(item->payload_json);
    if (root == NULL || !cJSON_IsObject(root) ||
        cJSON_GetObjectItemCaseSensitive(root, "message_id") != NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    if (cJSON_AddStringToObject(root, "message_id", item->message_id) == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static char *gateway_create_command_response(
    const char *request_id,
    const char *status,
    const char *detail_name,
    const char *detail_value)
{
    if (request_id == NULL ||
        request_id[0] == '\0'||
        status == NULL ||
        status[0] == '\0'||
        detail_name == NULL ||
        detail_name[0] == '\0'||
        detail_value == NULL ||
        detail_value[0] == '\0')
    {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
    {
        return NULL;
    }

    if (cJSON_AddStringToObject(
        root, "request_id", request_id) == NULL ||
        cJSON_AddStringToObject(
            root, "status", status) == NULL ||
        cJSON_AddStringToObject(
            root, detail_name, detail_value) == NULL)
    {
        cJSON_Delete(root);
        return NULL;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static mqtt_publisher_result_t gateway_publish_command_response(
    gateway_workers_t *workers,
    const char *payload)
{
    mqtt_publisher_result_t result =
        mqtt_publisher_wait_connected(
            workers->publisher,
            GATEWAY_PUBLISH_TIMEOUT_SECONDS);

    if (result == MQTT_PUBLISHER_OK)
    {
        result = mqtt_publisher_publish_json(
            workers->publisher,
            workers->mqtt_command_response_topic,
            payload,
            GATEWAY_PUBLISH_TIMEOUT_SECONDS);
    }

    return result;
}

static void *gateway_mqtt_command_worker(void *argument)
{
    gateway_workers_t *workers = argument;
    gateway_mqtt_command_request_t request;

    while (1)
    {
        bq_result_t queue_result =
        bq_pop(&workers->mqtt_command_queue,
            &request,
            GATEWAY_QUEUE_POLL_MS);
        if (queue_result == BQ_OK)
        {
            cJSON *root = cJSON_ParseWithLength(request.payload, request.payload_length);
            if (root == NULL)
            {
                fprintf(stderr, "invalid remote command JSON\n");
                continue;
            }

            const cJSON *request_id_item =
                cJSON_GetObjectItemCaseSensitive(root, "request_id");

            if (!cJSON_IsString(request_id_item) ||
                request_id_item->valuestring == NULL ||
                request_id_item->valuestring[0] == '\0' ||
                strnlen(request_id_item->valuestring,
                        GATEWAY_COMMAND_REQUEST_ID_CAPACITY) ==
                    GATEWAY_COMMAND_REQUEST_ID_CAPACITY)
            {
                fprintf(stderr, "invalid remote request_id\n");
                cJSON_Delete(root);
                continue;
            }

            const cJSON *command_item =
                cJSON_GetObjectItemCaseSensitive(root, "command");

            const char *command_error = NULL;

            if (!cJSON_IsString(command_item) ||
                command_item->valuestring == NULL ||
                command_item->valuestring[0] == '\0')
            {
                command_error = "invalid_command";
            }
            else
            {
                bool command_supported =
                strcmp(command_item->valuestring, "status") == 0 ||
                strcmp(command_item->valuestring, "set_interval") == 0 ||
                strncmp(command_item->valuestring,"set_interval ",13) == 0 ||
                strcmp(command_item->valuestring, "get_config") == 0 ||
                strcmp(command_item->valuestring, "pause_collection") == 0 ||
                strcmp(command_item->valuestring, "resume_collection") == 0 ||
                strcmp(command_item->valuestring, "get_storage_stats") == 0 ||
                strcmp(command_item->valuestring, "get_version") == 0;

                if (!command_supported)
                    command_error = "unsupported_command";
            }



            if (command_error != NULL)
            {
                char *error_payload =
                    gateway_create_command_response(
                        request_id_item->valuestring,
                        "error",
                        "error",
                        command_error);

                if (error_payload == NULL)
                {
                    cJSON_Delete(root);
                    gateway_worker_fail(
                        workers,
                        "remote error response creation failed");
                    break;
                }

                mqtt_publisher_result_t publish_result =
                    gateway_publish_command_response(
                        workers,
                        error_payload);

                cJSON_free(error_payload);

                if (publish_result != MQTT_PUBLISHER_OK)
                {
                    fprintf(stderr,
                    "remote error response publish failed: %d\n",
                    (int)publish_result);
                }

                cJSON_Delete(root);
                continue;
            }

            const char *cache_response  = NULL;
            gateway_command_dedup_result_t command_dedup_result =
                gateway_find_cached_command_response(
                     workers,
                     request_id_item->valuestring,
                     command_item->valuestring,
                    &cache_response );

            if (command_dedup_result != GATEWAY_COMMAND_DEDUP_MISS)
            {
                if (command_dedup_result == GATEWAY_COMMAND_DEDUP_MATCH)
                {
                    printf("duplicate remote request_id: %s\n",
                    request_id_item->valuestring);

                    mqtt_publisher_result_t publish_result =
                        gateway_publish_command_response(
                            workers,
                            cache_response);

                    if (publish_result != MQTT_PUBLISHER_OK)
                    {
                        fprintf(stderr,
                                "cached remote response publish failed: %d\n",
                                (int)publish_result);
                    }
                    cJSON_Delete(root);
                    continue;
                }
                else if (command_dedup_result == GATEWAY_COMMAND_DEDUP_CONFLICT)
                {
                    /* 冲突请求不得执行，也不能覆盖最初请求保存的响应。 */
                    char *response_payload =
                        gateway_create_command_response(
                            request_id_item->valuestring,
                            "error",
                            "error",
                            "request_id_conflict");
                    if (response_payload == NULL)
                    {
                        cJSON_Delete(root);
                        gateway_worker_fail(
                            workers,
                            "remote response JSON creation failed");
                        break;
                    }

                    mqtt_publisher_result_t publish_result =
                            gateway_publish_command_response(
                                workers,
                                response_payload);

                    if (publish_result != MQTT_PUBLISHER_OK)
                    {
                        fprintf(stderr,
                                "remote response publish failed: %d\n",
                                (int)publish_result);
                    }
                    cJSON_free(response_payload);

                    cJSON_Delete(root);
                    continue;
                }
                else
                {
                    cJSON_Delete(root);
                    gateway_worker_fail(
                    workers,
                    "gateway find cached command response failed");
                    break;
                }
            }


            printf("remote request_id: %s\n",
                request_id_item->valuestring);
            printf("remote command: %s\n",
                command_item->valuestring);

            char reply[128];

            if (gateway_execute_command(
                    workers,
                    command_item->valuestring,
                    reply,
                    sizeof(reply)) != 0)
            {
                cJSON_Delete(root);
                gateway_worker_fail(
                    workers,
                    "remote command execution failed");
                break;
            }

            const char *response_status = "ok";
            const char *detail_name = "result";
            const char *detail_value = reply;

            if (strncmp(reply, "error=", 6) == 0)
            {
                response_status = "error";
                detail_name = "error";
                detail_value = reply + 6;
            }

            char *response_payload =
                gateway_create_command_response(
                    request_id_item->valuestring,
                    response_status,
                    detail_name,
                    detail_value);
            if (response_payload == NULL)
            {
                cJSON_Delete(root);
                gateway_worker_fail(
                    workers,
                    "remote response JSON creation failed");
                break;
            }

            if (!gateway_cache_command_response(
                    workers,
                    request_id_item->valuestring,
                    command_item->valuestring,
                    response_payload))
            {
                cJSON_free(response_payload);
                cJSON_Delete(root);

                gateway_worker_fail(
                    workers,
                    "remote response caching failed");
                break;
            }

            mqtt_publisher_result_t publish_result =
                    gateway_publish_command_response(
                        workers,
                        response_payload);

            if (publish_result != MQTT_PUBLISHER_OK)
            {
                fprintf(stderr,
                        "remote response publish failed: %d\n",
                        (int)publish_result);
            }
            cJSON_free(response_payload);

            cJSON_Delete(root);
            continue;
        }

         if (queue_result == BQ_TIMEOUT)
            continue;

        if (queue_result == BQ_CLOSED)
            break;

        gateway_worker_fail(
            workers,
            "MQTT command queue pop failed");
        break;
    }
    return NULL;
}

static void *gateway_mqtt_worker(void *argument)
{
    gateway_workers_t *workers = argument;

    for (;;)
    {
        gateway_delivery_job_t job = {0};
        bq_result_t queue_result =
            bq_pop(&workers->delivery_queue, &job, -1);

        if (queue_result == BQ_CLOSED)
            break;
        if (queue_result != BQ_OK)
        {
            gateway_worker_fail(workers,
                                "MQTT worker delivery pop failed");
            break;
        }

        char *delivery_payload = gateway_create_delivery_payload(&job.item);
        if (delivery_payload == NULL)
        {
            outbox_store_free_outbox_item(&job.item);
            gateway_worker_fail(workers,
                                "MQTT worker payload enrichment failed");
            break;
        }

        gateway_delivery_report_t report = {
            .outbox_id = job.item.id,
            .publish_result = mqtt_publisher_publish_json(
                workers->publisher,
                job.item.topic,
                delivery_payload,
                GATEWAY_PUBLISH_TIMEOUT_SECONDS),
            .sent_at_ms = 0
        };
        cJSON_free(delivery_payload);

        if (report.publish_result == MQTT_PUBLISHER_OK)
        {
            /* sent_at 是业务时间戳，因此使用墙上时钟而非单调时钟。 */
            struct timespec now;
            if (clock_gettime(CLOCK_REALTIME, &now) != 0)
                report.publish_result = MQTT_PUBLISHER_LIBRARY_ERROR;
            else
                report.sent_at_ms =
                    (int64_t)now.tv_sec * INT64_C(1000) +
                    now.tv_nsec / 1000000L;
        }

        outbox_store_free_outbox_item(&job.item);

        for (;;)
        {
            /*
             * 正常停机时仍应尽量把发布结果交回 storage worker；只有结果
             * 队列已关闭或系统已进入致命错误状态时才放弃。
             */
            bq_result_t result =
                bq_push(&workers->result_queue,
                        &report,
                        GATEWAY_QUEUE_POLL_MS);

            if (result == BQ_OK)
                break;
            if (result == BQ_CLOSED)
                goto DONE;
            if (result != BQ_TIMEOUT)
            {
                gateway_worker_fail(
                    workers, "MQTT worker result push failed");
                goto DONE;
            }
            if (gateway_workers_failed(workers))
                goto DONE;
        }
    }

DONE:
    /* 通知 storage worker 不会再产生投递回执。 */
    (void)bq_close(&workers->result_queue);
    return NULL;
}

static int gateway_join_thread(pthread_t thread,
                               gateway_workers_t *workers)
{
    if (pthread_join(thread, NULL) != 0)
    {
        gateway_workers_request_stop(workers, true);
        return -1;
    }
    return 0;
}

static void gateway_drain_delivery_queue(gateway_workers_t *workers)
{
    gateway_delivery_job_t job = {0};

    while (bq_pop(&workers->delivery_queue, &job, 0) == BQ_OK)
    {
        /* 未被 MQTT worker 取走的任务仍持有动态分配的 topic/payload。 */
        outbox_store_free_outbox_item(&job.item);
        memset(&job, 0, sizeof(job));
    }
}

int gateway_workers_run(outbox_store_t *store,
                        measurement_source_t *source,
                        mqtt_publisher_t *publisher,
                        const gateway_workers_config_t *config)
{
    if (store == NULL ||
        source == NULL ||
        publisher == NULL ||
        config == NULL ||
        config->mqtt_host == NULL ||
        config->mqtt_host[0] == '\0' ||
        config->mqtt_port < 1 ||
        config->mqtt_port > 65535 ||
        config->mqtt_command_client_id == NULL ||
        config->mqtt_command_client_id[0] == '\0' ||
        config->mqtt_command_request_topic == NULL ||
        config->mqtt_command_request_topic[0] == '\0' ||
        config->mqtt_command_response_topic == NULL ||
        config->mqtt_command_response_topic[0] == '\0')
        return -1;

    if (config->initial_interval_ms < 100 ||
        config->initial_interval_ms > 60000)
        return -1;

    gateway_workers_t workers = {
        .store = store,
        .source = source,
        .publisher = publisher,
        .interval_ms = config->initial_interval_ms,
        .mqtt_command_response_topic = config->mqtt_command_response_topic
    };
    pthread_t source_thread;
    pthread_t storage_thread;
    pthread_t mqtt_thread;
    pthread_t command_thread;
    pthread_t mqtt_command_thread;
    bool status_ready = false;
    bool measurement_ready = false;
    bool delivery_ready = false;
    bool result_ready = false;
    bool source_started = false;
    bool storage_started = false;
    bool mqtt_started = false;
    bool command_started = false;
    bool storage_control_request_ready = false;
    bool storage_control_result_ready = false;
    bool mqtt_command_ready = false;
    bool command_receiver_ready = false;
    bool mqtt_command_started = false;
    bool command_mutex_ready = false;
    int result = -1;

    if (pthread_mutex_init(&workers.status_mutex, NULL) != 0)
        goto CLEANUP;
    status_ready = true;

    if (pthread_mutex_init(&workers.command_mutex, NULL) != 0)
    {
        goto CLEANUP;
    }
    command_mutex_ready = true;


    if (bq_init(&workers.measurement_queue,
                GATEWAY_MEASUREMENT_QUEUE_CAPACITY,
                sizeof(measurement_t)) != BQ_OK)
        goto CLEANUP;
    measurement_ready = true;

    if (bq_init(&workers.delivery_queue,
                GATEWAY_DELIVERY_QUEUE_CAPACITY,
                sizeof(gateway_delivery_job_t)) != BQ_OK)
        goto CLEANUP;
    delivery_ready = true;

    if (bq_init(&workers.result_queue,
                GATEWAY_RESULT_QUEUE_CAPACITY,
                sizeof(gateway_delivery_report_t)) != BQ_OK)
        goto CLEANUP;
    result_ready = true;

    if (bq_init(&workers.storage_control_request_queue,
        1,
        sizeof(gateway_storage_control_request_t)) != BQ_OK)
        goto CLEANUP;
    storage_control_request_ready = true;


    if (bq_init(&workers.storage_control_result_queue,
        1,
        sizeof(gateway_storage_control_result_t)) != BQ_OK)
        goto CLEANUP;
    storage_control_result_ready = true;

    if (bq_init(&workers.mqtt_command_queue,
            GATEWAY_MQTT_COMMAND_QUEUE_CAPACITY,
            sizeof(gateway_mqtt_command_request_t)) != BQ_OK)
    {
        goto CLEANUP;
    }
    mqtt_command_ready = true;

    const mqtt_command_receiver_config_t receiver_config =
    {
        .host = config->mqtt_host,
        .port = config->mqtt_port,
        .client_id = config->mqtt_command_client_id,
        .request_topic = config->mqtt_command_request_topic,
        .keepalive_seconds = 30,
        .message_handler = gateway_mqtt_command_received,
        .message_handler_context = &workers
    };

    workers.command_receiver = mqtt_command_receiver_create(&receiver_config);

    if (workers.command_receiver == NULL)
    {
        goto CLEANUP;
    }
    command_receiver_ready = true;

    /*
     * 先启动下游再启动上游，确保 source 开始产出时整条流水线已可消费。
     */

     if (pthread_create(&mqtt_command_thread,
                   NULL,
                   gateway_mqtt_command_worker,
                   &workers) != 0)
    {
        goto STOP;
    }

    mqtt_command_started = true;

    mqtt_command_receiver_result_t receiver_start_result =
    mqtt_command_receiver_start(
        workers.command_receiver);

    if (receiver_start_result != MQTT_COMMAND_RECEIVER_OK)
    {
        fprintf(stderr,
                "failed to start MQTT command receiver: %d\n",
                (int)receiver_start_result);
        goto STOP;
    }



    if (pthread_create(&mqtt_thread,
                       NULL,
                       gateway_mqtt_worker,
                       &workers) != 0)
        goto STOP;
    mqtt_started = true;

    if (pthread_create(&storage_thread,
                       NULL,
                       gateway_storage_worker,
                       &workers) != 0)
        goto STOP;
    storage_started = true;

    if (pthread_create(&source_thread,
                       NULL,
                       gateway_source_worker,
                       &workers) != 0)
        goto STOP;
    source_started = true;

    if (pthread_create(&command_thread, NULL, gateway_command_worker, &workers) != 0)
    {
        goto STOP;
    }
    command_started = true;

    while (!graceful_shutdown_requested() &&
           !gateway_workers_failed(&workers))
    {
        if (gateway_worker_sleep(&workers, GATEWAY_QUEUE_POLL_MS) != 0)
        {
            gateway_worker_fail(&workers,
                                "worker coordinator sleep failed");
            break;
        }
    }

    if (!gateway_workers_failed(&workers))
        result = 0;

STOP:
    /*
     * 先关闭上游队列唤醒 source/MQTT，再依次 join；MQTT 退出时会关闭
     * result_queue，storage 因而可以处理完已有数据和回执后自然结束。
     */
    gateway_workers_request_stop(&workers, result != 0);

    if (measurement_ready)
        (void)bq_close(&workers.measurement_queue);
    if (delivery_ready)
        (void)bq_close(&workers.delivery_queue);

    if (command_started &&
        gateway_join_thread(command_thread, &workers) != 0)
        result = -1;

    if (storage_control_request_ready)
        (void)bq_close(&workers.storage_control_request_queue);

    if (command_receiver_ready)
    {
        mqtt_command_receiver_destroy(
            workers.command_receiver);
        workers.command_receiver = NULL;
        command_receiver_ready = false;
    }

    if (mqtt_command_ready)
        (void)bq_close(&workers.mqtt_command_queue);

    if (mqtt_command_started &&
    gateway_join_thread(
        mqtt_command_thread,
        &workers) != 0)
    {
        result = -1;
    }

    if (source_started &&
        gateway_join_thread(source_thread, &workers) != 0)
        result = -1;

    if (mqtt_started &&
        gateway_join_thread(mqtt_thread, &workers) != 0)
        result = -1;

    if (result_ready)
        (void)bq_close(&workers.result_queue);

    if (storage_started &&
        gateway_join_thread(storage_thread, &workers) != 0)
        result = -1;

    if (storage_control_result_ready)
        (void)bq_close(&workers.storage_control_result_queue);

    if (gateway_workers_failed(&workers))
        result = -1;

CLEANUP:
    if (command_receiver_ready)
    {
        mqtt_command_receiver_destroy(workers.command_receiver);
    }


    if (mqtt_command_ready &&
        bq_destroy(&workers.mqtt_command_queue) != BQ_OK)
    {
        result = -1;
    }

    if (storage_control_result_ready &&
        bq_destroy(&workers.storage_control_result_queue) != BQ_OK)
        result = -1;


    if (storage_control_request_ready &&
        bq_destroy(&workers.storage_control_request_queue) != BQ_OK)
        result = -1;


    if (delivery_ready)
        gateway_drain_delivery_queue(&workers);

    if (result_ready &&
        bq_destroy(&workers.result_queue) != BQ_OK)
        result = -1;
    if (delivery_ready &&
        bq_destroy(&workers.delivery_queue) != BQ_OK)
        result = -1;
    if (measurement_ready &&
        bq_destroy(&workers.measurement_queue) != BQ_OK)
        result = -1;
    if (command_mutex_ready &&
        pthread_mutex_destroy(&workers.command_mutex) != 0)
        result = -1;
    if (status_ready &&
        pthread_mutex_destroy(&workers.status_mutex) != 0)
        result = -1;

    return result;
}
