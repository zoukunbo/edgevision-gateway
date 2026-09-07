#ifndef EDGEVISION_CORE_GATEWAY_WORKERS_H
#define EDGEVISION_CORE_GATEWAY_WORKERS_H

#include "measurement_source.h"
#include "mqtt_publisher.h"
#include "outbox_store.h"

/*
 * 运行三 worker 服务：
 * - source worker 独占 MeasurementSource；
 * - storage worker 独占 Store；
 * - MQTT worker 独占 Publisher 的发布接口。
 *
 * 返回前会关闭队列并回收三个应用线程；Publisher 和 Store 的最终销毁仍由
 * gateway_run() 按依赖逆序完成。
 */
int gateway_workers_run(outbox_store_t *store,
                        measurement_source_t *source,
                        mqtt_publisher_t *publisher);

#endif
