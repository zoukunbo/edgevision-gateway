#include "measurement_source.h"
#include "simulated_source.h"

#include <stdio.h>

int main(void)
{
    simulated_source_t simulated;
    measurement_source_t source;
    measurement_t first;
    measurement_t second;

    simulated_source_init(&simulated);
    source = simulated_source_as_measurement_source(&simulated);

    /* 核心验证1：连续取得两条数据。 */
    if (measurement_source_next(&source, &first) !=
            MEASUREMENT_SOURCE_OK ||
        measurement_source_next(&source, &second) !=
            MEASUREMENT_SOURCE_OK)
    {
        fprintf(stderr, "failed to read simulated measurements\n");
        return 1;
    }

    if (first.sequence != 1 || second.sequence != 2 ||
        first.timestamp_ms + 1 != second.timestamp_ms ||
        first.value != 20 || second.value != 20.125)
    {
        return 1;
    }
    /* 最重要异常：空函数指针必须被统一接口拒绝。 */
    const measurement_source_t invalid_source = {0};

    if (measurement_source_next(
            (measurement_source_t *)&invalid_source,
            &first) != MEASUREMENT_SOURCE_ERROR)
    {
        fprintf(stderr, "invalid source was not rejected\n");
        return 1;
    }

    printf("simulated source tests passed\n");
    return 0;
}