#ifndef EDGEVISION_CORE_GATEWAY_WORKERS_H
#define EDGEVISION_CORE_GATEWAY_WORKERS_H

#include "measurement_source.h"
#include "mqtt_publisher.h"
#include "outbox_store.h"

typedef struct
{
    int initial_interval_ms;

    const char *mqtt_host;
    int mqtt_port;
    const char *mqtt_command_client_id;
    const char *mqtt_command_request_topic;
    const char *mqtt_command_response_topic;
} gateway_workers_config_t;
/*
 * 运行 Gateway worker 服务：
 * - source worker 独占 MeasurementSource；
 * - storage worker 独占 Store；
 * - MQTT worker 独占 Publisher 的发布接口。
 * - command worker 通过本地 Unix socket 提供运行状态查询。
 *
 * 返回前会关闭队列并回收四个应用线程；Publisher 和 Store 的最终销毁仍由
 * gateway_run() 按依赖逆序完成。
 */
int gateway_workers_run(outbox_store_t *store,
                        measurement_source_t *source,
                        mqtt_publisher_t *publisher,
                        const gateway_workers_config_t *config);

#endif
