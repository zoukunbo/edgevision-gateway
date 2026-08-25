#ifndef EDGEVISION_DOMAIN_MEASUREMENT_H
#define EDGEVISION_DOMAIN_MEASUREMENT_H

#include <stdint.h>

#define MEASUREMENT_SCHEMA_VERSION 1u

/*
 * 容量包含字符串末尾的 '\0'：
 * device_id 最多63字符，所以数组容量是64。
 */
#define MEASUREMENT_DEVICE_ID_CAPACITY 64u
#define MEASUREMENT_METRIC_CAPACITY 32u
#define MEASUREMENT_UNIT_CAPACITY 16u

typedef enum
{
    MEASUREMENT_QUALITY_GOOD = 0,
    MEASUREMENT_QUALITY_UNCERTAIN,
    MEASUREMENT_QUALITY_BAD
} measurement_quality_t;

typedef struct
{
    // Measurement 契约版本
    uint32_t schema_version;
    // 数据来源设备 非空，最多63字符
    char device_id[MEASUREMENT_DEVICE_ID_CAPACITY];
    // 设备内递增序号 大于0
    uint32_t sequence;
    // UTC采集时间，Unix毫秒
    int64_t timestamp_ms;
    // 指标名，如temperature 非空，最多31字符
    char metric[MEASUREMENT_METRIC_CAPACITY];
    // 测量值
    double value;
    // 单位，如celsius 最多15字符
    char unit[MEASUREMENT_UNIT_CAPACITY];
    // 数据质量
    measurement_quality_t quality;
} measurement_t;

typedef enum
{
    MEASUREMENT_VALID = 0,
    MEASUREMENT_INVALID_ARGUMENT = -1,
    MEASUREMENT_UNSUPPORTED_SCHEMA = -2,
    MEASUREMENT_INVALID_DEVICE_ID = -3,
    MEASUREMENT_INVALID_SEQUENCE = -4,
    MEASUREMENT_INVALID_TIMESTAMP = -5,
    MEASUREMENT_INVALID_METRIC = -6,
    MEASUREMENT_INVALID_VALUE = -7,
    MEASUREMENT_INVALID_UNIT = -8,
    MEASUREMENT_INVALID_QUALITY = -9
} measurement_validation_result_t;

measurement_validation_result_t measurement_validate(
    const measurement_t *measurement);

#endif