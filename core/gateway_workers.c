#define _POSIX_C_SOURCE 200809L

#include "gateway_workers.h"

#include "blocking_queue.h"
#include "cJSON.h"
#include "graceful_shutdown.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GATEWAY_MEASUREMENT_QUEUE_CAPACITY 32u
#define GATEWAY_DELIVERY_QUEUE_CAPACITY 1u
#define GATEWAY_RESULT_QUEUE_CAPACITY 1u
#define GATEWAY_QUEUE_POLL_MS 100
#define GATEWAY_SOURCE_INTERVAL_MS 1000
#define GATEWAY_DELIVERY_RETRY_MS 1000
#define GATEWAY_PUBLISH_TIMEOUT_SECONDS 3

typedef struct
{
    outbox_item_t item;
} gateway_delivery_job_t;

typedef struct
{
    int64_t outbox_id;
    mqtt_publisher_result_t publish_result;
    int64_t sent_at_ms;
} gateway_delivery_report_t;

typedef struct
{
    outbox_store_t *store;
    measurement_source_t *source;
    mqtt_publisher_t *publisher;

    bounded_queue_t measurement_queue;
    bounded_queue_t delivery_queue;
    bounded_queue_t result_queue;

    pthread_mutex_t status_mutex;
    int stop_requested;
    int fatal_error;
} gateway_workers_t;

static void gateway_workers_request_stop(gateway_workers_t *workers,
                                         bool fatal)
{
    pthread_mutex_lock(&workers->status_mutex);
    workers->stop_requested = 1;
    if (fatal)
        workers->fatal_error = 1;
    pthread_mutex_unlock(&workers->status_mutex);
}

static bool gateway_workers_should_stop(gateway_workers_t *workers)
{
    int requested;

    pthread_mutex_lock(&workers->status_mutex);
    requested = workers->stop_requested;
    pthread_mutex_unlock(&workers->status_mutex);

    return requested != 0 || graceful_shutdown_requested();
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

static int gateway_worker_sleep(gateway_workers_t *workers, int total_ms)
{
    const struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = GATEWAY_QUEUE_POLL_MS * 1000000L
    };

    for (int elapsed = 0; elapsed < total_ms;
         elapsed += GATEWAY_QUEUE_POLL_MS)
    {
        if (gateway_workers_should_stop(workers))
            return 0;

        struct timespec remaining = interval;
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

static void *gateway_source_worker(void *argument)
{
    gateway_workers_t *workers = argument;

    while (!gateway_workers_should_stop(workers))
    {
        measurement_t measurement = {0};
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

        for (;;)
        {
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

        if (gateway_worker_sleep(workers, GATEWAY_SOURCE_INTERVAL_MS) != 0)
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

    while (!measurement_closed || !result_closed)
    {
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
        outbox_store_free_outbox_item(&job.item);
        memset(&job, 0, sizeof(job));
    }
}

int gateway_workers_run(outbox_store_t *store,
                        measurement_source_t *source,
                        mqtt_publisher_t *publisher)
{
    gateway_workers_t workers = {
        .store = store,
        .source = source,
        .publisher = publisher
    };
    pthread_t source_thread;
    pthread_t storage_thread;
    pthread_t mqtt_thread;
    bool status_ready = false;
    bool measurement_ready = false;
    bool delivery_ready = false;
    bool result_ready = false;
    bool source_started = false;
    bool storage_started = false;
    bool mqtt_started = false;
    int result = -1;

    if (store == NULL || source == NULL || publisher == NULL)
        return -1;

    if (pthread_mutex_init(&workers.status_mutex, NULL) != 0)
        goto CLEANUP;
    status_ready = true;

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
    gateway_workers_request_stop(&workers, result != 0);

    if (measurement_ready)
        (void)bq_close(&workers.measurement_queue);
    if (delivery_ready)
        (void)bq_close(&workers.delivery_queue);

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

    if (gateway_workers_failed(&workers))
        result = -1;

CLEANUP:
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
    if (status_ready &&
        pthread_mutex_destroy(&workers.status_mutex) != 0)
        result = -1;

    return result;
}
