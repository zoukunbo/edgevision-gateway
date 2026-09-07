#define _POSIX_C_SOURCE 200809L

#include "stm32_modbus_source.h"

#include "frame.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#define STM32_SLAVE_ADDRESS 1u
#define STM32_FUNCTION_INPUT_REGISTERS 0x04u
#define STM32_START_REGISTER 1u
#define STM32_REGISTER_COUNT 2u
#define STM32_NORMAL_RESPONSE_LENGTH 9u
#define STM32_RAW_VALUE_LIMIT 1000u

typedef enum
{
    STM32_QUERY_ERROR = -1,
    STM32_QUERY_NO_DATA = 0,
    STM32_QUERY_OK = 1
} stm32_query_result_t;

static int source_stop_requested(stm32_modbus_source_t *source)
{
    return source->should_stop != NULL &&
           source->should_stop(source->stop_context) != 0;
}

/*
 * 完成一次有共同接收截止时间的 Modbus 04 事务。
 *
 * 先读 3 字节头只是为了确定正常帧或异常帧的剩余长度；头部不能替代完整
 * CRC/站号/功能码校验。第二段读取继续使用原 deadline，不能重新获得一整份
 * timeout，否则分段读取会悄悄突破调用方配置的总接收预算。
 */
static stm32_query_result_t query_registers(
    stm32_modbus_source_t *source,
    uint16_t registers[STM32_REGISTER_COUNT])
{
    uint8_t request[8];
    uint8_t *response = source->serial.buffer;
    size_t part_received = 0;
    size_t remaining = 0;
    int64_t deadline_ns = 0;

    source->serial.received = 0;
    if (source_stop_requested(source))
        return STM32_QUERY_NO_DATA;

    if (modbus_rtu_build_read_registers(
            STM32_SLAVE_ADDRESS,
            STM32_FUNCTION_INPUT_REGISTERS,
            STM32_START_REGISTER,
            STM32_REGISTER_COUNT,
            request,
            sizeof(request)) != (int)sizeof(request))
    {
        errno = EPROTO;
        return STM32_QUERY_ERROR;
    }

    if (real_serial_source_send(&source->serial, request, sizeof(request)) < 0)
        return STM32_QUERY_ERROR;

    if (serial_deadline_after_ms(source->serial.timeout_ms, &deadline_ns) < 0)
        return STM32_QUERY_ERROR;

    int rc = serial_read_exact_until_stop(
        source->serial.port.serial_fd,
        response,
        3u,
        deadline_ns,
        &part_received,
        source->should_stop,
        source->stop_context);
    source->serial.received = part_received;

    if (rc < 0)
        return errno == ECANCELED ? STM32_QUERY_NO_DATA : STM32_QUERY_ERROR;
    if (rc == 0)
        return STM32_QUERY_NO_DATA;

    if (response[0] == STM32_SLAVE_ADDRESS &&
        response[1] == STM32_FUNCTION_INPUT_REGISTERS &&
        response[2] == STM32_REGISTER_COUNT * 2u)
    {
        remaining = response[2] + 2u;
    }
    else if (response[0] == STM32_SLAVE_ADDRESS &&
             response[1] ==
                 (uint8_t)(STM32_FUNCTION_INPUT_REGISTERS | 0x80u))
    {
        remaining = 2u;
    }
    else
    {
        return STM32_QUERY_NO_DATA;
    }

    part_received = 0;
    rc = serial_read_exact_until_stop(
        source->serial.port.serial_fd,
        response + 3u,
        remaining,
        deadline_ns,
        &part_received,
        source->should_stop,
        source->stop_context);
    source->serial.received += part_received;

    if (rc < 0)
        return errno == ECANCELED ? STM32_QUERY_NO_DATA : STM32_QUERY_ERROR;
    if (rc == 0)
        return STM32_QUERY_NO_DATA;

    uint8_t exception_code = 0;
    modbus_response_result_t checked = modbus_rtu_check_read_registers(
        response,
        source->serial.received,
        STM32_SLAVE_ADDRESS,
        STM32_FUNCTION_INPUT_REGISTERS,
        STM32_REGISTER_COUNT,
        &exception_code);
    (void)exception_code;
    if (checked != MODBUS_RESPONSE_NORMAL ||
        source->serial.received != STM32_NORMAL_RESPONSE_LENGTH)
    {
        return STM32_QUERY_NO_DATA;
    }

    registers[0] =
        (uint16_t)((uint16_t)response[3] << 8u) | response[4];
    registers[1] =
        (uint16_t)((uint16_t)response[5] << 8u) | response[6];
    return STM32_QUERY_OK;
}

static int64_t realtime_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * INT64_C(1000) +
           (int64_t)now.tv_nsec / INT64_C(1000000);
}

/*
 * 寄存器已经通过完整 Modbus 校验后才进入本函数。两条 Measurement 先在局部
 * 数组中完整构造并验证，再一次性写入输出，失败时不留下半成品。
 */
static int map_measurements(
    stm32_modbus_source_t *source,
    const uint16_t registers[STM32_REGISTER_COUNT],
    measurement_t output[STM32_REGISTER_COUNT])
{
    if (registers[0] > STM32_RAW_VALUE_LIMIT ||
        registers[1] > STM32_RAW_VALUE_LIMIT ||
        source->next_sequence == 0u ||
        source->next_sequence == UINT32_MAX)
    {
        return -1;
    }

    int64_t timestamp_ms = realtime_milliseconds();
    if (timestamp_ms <= 0)
        return -1;

    measurement_t pending[STM32_REGISTER_COUNT] = {
        {
            .schema_version = MEASUREMENT_SCHEMA_VERSION,
            .device_id = "stm32-dht11-01",
            .sequence = source->next_sequence,
            .timestamp_ms = timestamp_ms,
            .metric = "temperature",
            .value = (double)registers[0] / 10.0,
            .unit = "celsius",
            .quality = MEASUREMENT_QUALITY_UNCERTAIN
        },
        {
            .schema_version = MEASUREMENT_SCHEMA_VERSION,
            .device_id = "stm32-dht11-01",
            .sequence = source->next_sequence + 1u,
            .timestamp_ms = timestamp_ms,
            .metric = "humidity",
            .value = (double)registers[1] / 10.0,
            .unit = "percent",
            .quality = MEASUREMENT_QUALITY_UNCERTAIN
        }
    };

    if (measurement_validate(&pending[0]) != MEASUREMENT_VALID ||
        measurement_validate(&pending[1]) != MEASUREMENT_VALID)
    {
        return -1;
    }

    memcpy(output, pending, sizeof(pending));
    return 0;
}

static measurement_source_result_t stm32_next(
    void *context,
    measurement_t *output)
{
    stm32_modbus_source_t *source = context;
    if (source == NULL || output == NULL || source->serial.port.serial_fd < 0)
        return MEASUREMENT_SOURCE_ERROR;

    if (source->pending_valid)
    {
        *output = source->pending;
        memset(&source->pending, 0, sizeof(source->pending));
        source->pending_valid = false;
        return MEASUREMENT_SOURCE_OK;
    }

    uint16_t registers[STM32_REGISTER_COUNT] = {0};
    stm32_query_result_t query = query_registers(source, registers);
    if (query == STM32_QUERY_NO_DATA)
        return MEASUREMENT_SOURCE_NO_DATA;
    if (query != STM32_QUERY_OK)
        return MEASUREMENT_SOURCE_ERROR;

    measurement_t measurements[STM32_REGISTER_COUNT];
    if (map_measurements(source, registers, measurements) != 0)
        return MEASUREMENT_SOURCE_NO_DATA;

    *output = measurements[0];
    source->pending = measurements[1];
    source->pending_valid = true;
    source->next_sequence += STM32_REGISTER_COUNT;
    return MEASUREMENT_SOURCE_OK;
}

int stm32_modbus_source_open(
    stm32_modbus_source_t *source,
    const stm32_modbus_source_config_t *config)
{
    if (source == NULL || config == NULL || config->serial_path == NULL ||
        config->gpiochip_path == NULL || config->timeout_ms <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    const real_serial_source_config_t serial_config = {
        .serial_path = config->serial_path,
        .gpiochip_path = config->gpiochip_path,
        .line_offset = config->line_offset,
        .receive_length = STM32_NORMAL_RESPONSE_LENGTH,
        .timeout_ms = config->timeout_ms
    };
    if (real_serial_source_open(&source->serial, &serial_config) != 0)
        return -1;

    source->next_sequence = 1u;
    memset(&source->pending, 0, sizeof(source->pending));
    source->pending_valid = false;
    source->should_stop = config->should_stop;
    source->stop_context = config->stop_context;
    return 0;
}

void stm32_modbus_source_close(stm32_modbus_source_t *source)
{
    if (source == NULL)
        return;
    real_serial_source_close(&source->serial);
    source->next_sequence = 1u;
    memset(&source->pending, 0, sizeof(source->pending));
    source->pending_valid = false;
    source->should_stop = NULL;
    source->stop_context = NULL;
}

measurement_source_t stm32_modbus_source_as_measurement_source(
    stm32_modbus_source_t *source)
{
    const measurement_source_t interface = {
        .context = source,
        .next = stm32_next
    };
    return interface;
}
