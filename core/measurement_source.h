#ifndef EDGEVISION_CORE_MEASUREMENT_SOURCE_H
#define EDGEVISION_CORE_MEASUREMENT_SOURCE_H

#include "measurement.h"

typedef enum
{
    /* 成功产生一条合法 Measurement。 */
    MEASUREMENT_SOURCE_OK = 0,

    /* 当前暂时没有数据，例如串口本轮超时；可以稍后继续。 */
    MEASUREMENT_SOURCE_NO_DATA = 1,

    /* 数据源发生不可继续的错误。 */
    MEASUREMENT_SOURCE_ERROR = -1
} measurement_source_result_t;

/*
 * 具体数据源的取数函数。
 *
 * context：
 *     具体实现自己的状态，例如模拟序号或Modbus串口对象。
 *
 * output：
 *     成功时写入完整的Measurement。
 */
typedef measurement_source_result_t (*measurement_source_next_fn)(
    void *context,
    measurement_t *output);

typedef struct
{
    void *context;
    measurement_source_next_fn next;
} measurement_source_t;

/*
 * 统一调用入口。
 *
 * Gateway Core 只调用这个函数，不直接调用 simulated_source_next()
 * 或未来的 modbus_source_next()。
 */
measurement_source_result_t measurement_source_next(
    measurement_source_t *source,
    measurement_t *output);

#endif