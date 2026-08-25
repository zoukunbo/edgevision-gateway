#include "measurement.h"

#include <stddef.h>
#include <string.h>
#include <math.h>


static int measurement_string_is_valid(
    const char *text,
    size_t capacity)
{
    /*
     void *memchr(const void *s, int c, size_t n);
     在一块内存缓冲区里，从起始位置s开始，在最多n个字节范围内，查找字节值c第一次出现的位置
     找到：返回指向该字节的指针 遍历完 n 字节都没找到：返回NULL
     */
    if (text == NULL || capacity == 0 || text[0] == '\0' || 
        memchr(text, '\0', capacity)== NULL)
    {
        return 0;
    }
    
    return 1;
}

measurement_validation_result_t measurement_validate(
    const measurement_t *measurement)
{
    if (measurement == NULL)
    {
        return MEASUREMENT_INVALID_ARGUMENT;
    }

    if (measurement->schema_version != MEASUREMENT_SCHEMA_VERSION)
    {
        return MEASUREMENT_UNSUPPORTED_SCHEMA;
    }
    

    if (measurement_string_is_valid(measurement->device_id, sizeof(measurement->device_id)) == 0) {
        return MEASUREMENT_INVALID_DEVICE_ID;
    }

    if (measurement_string_is_valid(measurement->metric, sizeof(measurement->metric)) == 0) {
        return MEASUREMENT_INVALID_METRIC;
    }

    if (measurement_string_is_valid(measurement->unit, sizeof(measurement->unit)) == 0) {
        return MEASUREMENT_INVALID_UNIT;
    }
    
    if (measurement->sequence == 0)
    {
       return MEASUREMENT_INVALID_SEQUENCE;
    }
    
    if (measurement->timestamp_ms <= 0)
    {
        return MEASUREMENT_INVALID_TIMESTAMP;
    }
    
    if (measurement->quality < MEASUREMENT_QUALITY_GOOD || measurement->quality > MEASUREMENT_QUALITY_BAD)
    {
        return MEASUREMENT_INVALID_QUALITY;
    }
    /*isfinite() 对正常有限数字返回非零，所以当前代码会把 12.5 判为非法*/
    if (isfinite(measurement->value) == 0) {
        return MEASUREMENT_INVALID_VALUE;
    }

    return MEASUREMENT_VALID;
}