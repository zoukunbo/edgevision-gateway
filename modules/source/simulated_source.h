#ifndef EDGEVISION_MODULES_SOURCE_SIMULATED_SOURCE_H
#define EDGEVISION_MODULES_SOURCE_SIMULATED_SOURCE_H

#include "measurement_source.h"

#include <stdint.h>

typedef struct
{
    uint32_t next_sequence;
    int64_t next_timestamp_ms;
} simulated_source_t;

/* 初始化为可重复验证的确定性数据。 */
void simulated_source_init(simulated_source_t *source);

/* 把具体模拟源转换成 Gateway Core 使用的统一接口。 */
measurement_source_t simulated_source_as_measurement_source(
    simulated_source_t *source);

#endif