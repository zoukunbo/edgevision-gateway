#include "real_serial_source.h"
#include "graceful_shutdown.h"
#include "frame.h"
#include "measurement_json.h"
#ifdef EDGEVISION_ENABLE_MQTT
#include "mqtt_publisher.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 本文件是“单次事务 -> 两条Measurement -> 可选MQTT”的验证入口，
 * 不是默认gateway真实数据源已经切换的声明。分层职责：
 * serial管字节与方向，query管本次请求/响应，map管寄存器语义，publish管出口。
 * 无参数/--serial-mqtt使用PC模拟03；--stm32/--stm32-mqtt使用真实STM32的04。
 * --map-sample系列只用固定模拟值，不访问串口，不能用于真实传感器验收。
 */
#define MODBUS_QUERY_EXERCISE_READY 1

typedef enum {
    QUERY_INCOMPLETE = -4,
    QUERY_IO_ERROR = -3, /* errno保留原因；ECANCELED表示取消。 */
    QUERY_INVALID = -2,
    QUERY_TIMEOUT = 0,
    QUERY_OK = 1,
    QUERY_EXCEPTION = 2
} query_result_t;

static int demo_should_stop(void *context)
{
    (void)context;
    return graceful_shutdown_requested();
}

static void print_bytes(const char *label, const uint8_t *bytes, size_t length)
{
    printf("%s %zu bytes:", label, length);
    for (size_t i = 0; i < length; ++i)
        printf(" %02X", (unsigned int)bytes[i]);
    putchar('\n');
}

/*
 * 站号1、数量2：PC模拟用03/地址0，STM32用04/地址1。
 * source已打开，两个输出指针有效；只有QUERY_OK才写registers。
 * 此函数借用source，不负责关闭，不重试，不生成Measurement。
 * 正常响应共9字节：地址/功能码/字节数 + 4字节数据 + 2字节CRC；
 * 异常响应共5字节：地址/功能码|0x80/异常码 + 2字节CRC。
 * 头部只决定后续长度，不能代替最后的整帧校验。
 */
static query_result_t query_two_registers(
    real_serial_source_t *source,
    int stm32_mode,
    uint16_t registers[2],
    uint8_t *exception_code)
{
    const uint8_t function = stm32_mode ? 0x04 : 0x03;
    const uint16_t start = stm32_mode ? 1u : 0u;
    uint8_t request[8];
    uint8_t *rx = source->buffer;
    int64_t deadline;
    size_t part_received = 0;
    size_t remaining = 0;

    source->received = 0;
    *exception_code = 0;

    if (demo_should_stop(NULL)) {
        errno = ECANCELED;
        return QUERY_IO_ERROR;
    }

    if (modbus_rtu_build_read_registers(
            1, function, start, 2, request, sizeof(request)) != 8)
        return QUERY_INVALID;

    if (real_serial_source_send(source, request, sizeof(request)) < 0)
        return QUERY_IO_ERROR;
    print_bytes("TX", request, sizeof(request));

    /* 发送完成后才开始接收预算；write本身仍没有总超时保证。 */
    if (serial_deadline_after_ms(source->timeout_ms, &deadline) < 0)
        return QUERY_IO_ERROR;

    printf("Waiting for response, shared RX budget=%d ms...\n",
           source->timeout_ms);
    fflush(stdout);
    int rc = serial_read_exact_until_stop(
        source->port.serial_fd, rx, 3, deadline,
        &part_received, demo_should_stop, NULL);

    /* part_received属于这一次read；source->received累计实际帧长，失败也保留诊断字节。 */
    source->received = part_received;
    if (rc < 0) return QUERY_IO_ERROR;
    if (rc == 0) return QUERY_TIMEOUT;

    if (rx[0] == 0x01 && rx[1] == function && rx[2] == 0x04)
    {
        remaining = rx[2] + 2;
    }
    else if (rx[0] == 0x01 && rx[1] == (uint8_t)(function | 0x80u))
    {
        remaining = 2;
    }
    else
    {
        return QUERY_INVALID;
    }

    /* 沿用读头之前生成的deadline；不能为“剩余部分”重新获得一整份接收预算。 */
    rc = serial_read_exact_until_stop(
        source->port.serial_fd, rx + 3, remaining, deadline,
        &part_received, demo_should_stop, NULL);

    source->received += part_received;
    if (rc < 0) return QUERY_IO_ERROR;
    if (rc == 0) return QUERY_TIMEOUT;

    rc = modbus_rtu_check_read_registers(
        rx, source->received, 1, function, 2, exception_code);

    if (rc == MODBUS_RESPONSE_INVALID)
    {
        return QUERY_INVALID;
    }

    if (rc == MODBUS_RESPONSE_EXCEPTION)
    {
        return QUERY_EXCEPTION;
    }

    /* 只有整帧合法且不是异常响应才交付寄存器；高字节在前，不在事务层除以10。 */
    registers[0] = (uint16_t)rx[3] << 8 | (uint16_t)rx[4];

    registers[1] = (uint16_t)rx[5] << 8 | (uint16_t)rx[6];

    return QUERY_OK;
}

/*
 * 两个寄存器的语义由来源决定，不能把PC模拟的比例直接用于STM32：
 * PC：无符号温度/湿度，1 count = 1单位，原始值0..100，pc-modbus-sim-01。
 * STM32：温度/湿度均为0.1单位，stm32-dht11-01；当前映射只接受非负0..1000。
 * 一条Measurement只描述一个指标，因此一次响应生成温度、湿度两条记录。
 * PC_MAP_*名称保留自早期练习，不表示STM32也被标记为模拟来源。
 */
typedef enum {
    PC_MAP_INVALID = -1,
    PC_MAP_OK = 0,
    PC_MAP_INCOMPLETE = 1
} pc_map_result_t;

/*
 * 只映射已通过对应03/04响应校验的寄存器；不读取串口、不计算CRC。
 * timestamp_ms由调用方提供：网关接受此次数据时的UTC Unix毫秒，
 * 不是设备内部测量时刻。两条记录共用时间，序号依次递增。
 * 失败/未完成时不得改写output；成功时一次性交付两条合法记录。
 */
static pc_map_result_t map_registers(
    const uint16_t registers[2],
    int64_t timestamp_ms,
    uint32_t first_sequence,
    measurement_t output[2],
    int stm32_mode)
{
    if (registers == NULL || output == NULL || timestamp_ms <= 0 ||
        first_sequence == 0 || first_sequence == UINT32_MAX)
        return PC_MAP_INVALID;

    measurement_t pending[2] = {
        {
            .schema_version = MEASUREMENT_SCHEMA_VERSION,
            .device_id = "pc-modbus-sim-01",
            .sequence = first_sequence,
            .timestamp_ms = timestamp_ms,
            .metric = "temperature",
            .unit = "celsius",
            .quality = MEASUREMENT_QUALITY_GOOD
        },
        {
            .schema_version = MEASUREMENT_SCHEMA_VERSION,
            .device_id = "pc-modbus-sim-01",
            .sequence = first_sequence + 1u,
            .timestamp_ms = timestamp_ms,
            .metric = "humidity",
            .unit = "percent",
            .quality = MEASUREMENT_QUALITY_GOOD
        }
    };

    /*
     * STM32截图确认：04/地址1温度、地址2湿度，原始值均除以10。
     * 暂仅接受非负0..100 celsius / 0..100 percent，这是应用保护范围，
     * 不是传感器量程结论。随后已核对STM32源码：温度为int16补码；
     * 无有效数据、最近采样失败或数据年龄达到7s时，04返回异常码04。
     * 本轮只收尾通信时序，不扩展负温度解码或改变网关quality策略。
     */
    const uint16_t raw_limit = stm32_mode ? 1000u : 100u;
    const double scale = stm32_mode ? 10.0 : 1.0;
    if (registers[0] > raw_limit || registers[1] > raw_limit)
    {
        return PC_MAP_INVALID;
    }

    pending[0].value = (double)registers[0] / scale;
    pending[1].value = (double)registers[1] / scale;

    if (stm32_mode) {
        for (size_t i = 0; i < 2; ++i) {
            memcpy(pending[i].device_id, "stm32-dht11-01", sizeof("stm32-dht11-01"));
            /*
             * quality暂沿用保守UNCERTAIN；传输CRC本身不证明传感器精度/新鲜度。
             * 固件状态规则虽已读到，仍不在方向控制修正中顺手改动质量策略。
             */
            pending[i].quality = MEASUREMENT_QUALITY_UNCERTAIN;
        }
    }

    if (measurement_validate(&pending[0]) != MEASUREMENT_VALID ||
        measurement_validate(&pending[1]) != MEASUREMENT_VALID)
    {
        return PC_MAP_INVALID;
    }

    memcpy(output, pending, sizeof(pending));

    return PC_MAP_OK;
}

/* 复用现有MQTT示例的UTC毫秒换算方式；接收截止时间仍用MONOTONIC。 */
static int64_t current_timestamp_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * INT64_C(1000) +
           (int64_t)now.tv_nsec / INT64_C(1000000);
}

#ifdef EDGEVISION_ENABLE_MQTT
/*
 * 复用Gateway使用的发布器；每条成功必须等到对应的QoS 1 PUBACK。
 * PUBACK只表示Broker确认，现场验收还要由独立订阅端核对两条JSON。
 * host=127.0.0.1指运行此程序的机器：板端使用时需已有Broker或临时SSH反向转发。
 * PC模拟上报已有证据，STM32真实上报在当前交接时尚未现场验证。
 */
static int publish_measurements(const measurement_t measurements[2])
{
    const mqtt_publisher_config_t config = {
        .host = "127.0.0.1",
        .port = 1883,
        .client_id = "edgevision-modbus-demo",
        .topic_prefix = "edgevision/v1/devices",
        .keepalive_seconds = 30,
        .reconnect_delay_seconds = 1u,
        .reconnect_delay_max_seconds = 8u
    };
    mqtt_publisher_t *publisher = mqtt_publisher_create(&config);
    if (publisher == NULL) {
        fputs("MQTT publisher creation failed.\n", stderr);
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    mqtt_publisher_result_t status = mqtt_publisher_start(publisher);
    if (status != MQTT_PUBLISHER_OK) {
        fprintf(stderr, "MQTT start failed: status=%d\n", (int)status);
        goto cleanup;
    }
    status = mqtt_publisher_wait_connected(publisher, 5);
    if (status != MQTT_PUBLISHER_OK) {
        fprintf(stderr, "MQTT connection unconfirmed: status=%d\n", (int)status);
        goto cleanup;
    }

    for (size_t i = 0; i < 2; ++i) {
        status = mqtt_publisher_publish(publisher, &measurements[i], 5);
        if (status != MQTT_PUBLISHER_OK) {
            fprintf(stderr, "MQTT publish unconfirmed: sequence=%u status=%d\n",
                    (unsigned int)measurements[i].sequence, (int)status);
            goto cleanup;
        }
        printf("PUBLISH_CONFIRMED device_id=%s sequence=%u metric=%s\n",
               measurements[i].device_id,
               (unsigned int)measurements[i].sequence, measurements[i].metric);
    }
    /* 两条独立发布，不是原子批次；这里不实现重试或持久化。 */
    exit_code = EXIT_SUCCESS;

cleanup:
    mqtt_publisher_destroy(publisher);
    return exit_code;
}
#endif

static int show_measurements(
    const uint16_t registers[2], int publish_mqtt, int stm32_mode)
{
#ifndef EDGEVISION_ENABLE_MQTT
    if (publish_mqtt) {
        fputs("MQTT disabled in this build; use an MQTT-enabled build.\n",
              stderr);
        return EXIT_FAILURE;
    }
#endif
    measurement_t measurements[2];
    char *json[2] = {NULL, NULL};
    size_t json_size[2] = {0, 0};
    int result = EXIT_FAILURE;

    /* 单次演示从1开始，不宣称具有跨进程/重启后的连续序号。 */
    pc_map_result_t mapped = map_registers(
        registers, current_timestamp_ms(), 1u, measurements, stm32_mode);
    if (mapped == PC_MAP_INCOMPLETE) {
        fputs("MAPPING INCOMPLETE: fill TODO M1/M2; no Measurement emitted.\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (mapped != PC_MAP_OK) {
        fputs("MAPPING INVALID: no Measurement emitted.\n", stderr);
        return EXIT_FAILURE;
    }

    /* 两条都序列化成功后才输出；无论何处失败都释放已分配JSON。 */
    for (size_t i = 0; i < 2; ++i) {
        if (measurement_to_json(&measurements[i], &json[i], &json_size[i]) !=
            MEASUREMENT_JSON_OK) {
            fputs("Measurement JSON encoding failed.\n", stderr);
            goto cleanup;
        }
    }
    for (size_t i = 0; i < 2; ++i)
        puts(json[i]);
    result = EXIT_SUCCESS;
#ifdef EDGEVISION_ENABLE_MQTT
    if (publish_mqtt)
        result = publish_measurements(measurements);
#endif

cleanup:
    for (size_t i = 0; i < 2; ++i) {
        if (json[i] != NULL)
            measurement_json_free(json[i]);
    }
    return result;
}

/*
 * 运行模式分成两类：map-sample只验证固定寄存器到Measurement的映射；
 * 其余模式才打开UART5/GPIO22并完成一次Modbus事务。带-mqtt的模式在
 * 串口/映射成功后追加同步QoS 1发布，但不把发布失败回写到设备或持久化重试。
 */
int main(int argc, char **argv)
{
    if (argc == 2 && (strcmp(argv[1], "--map-sample") == 0 ||
                      strcmp(argv[1], "--map-sample-mqtt") == 0)) {
        const uint16_t sample_registers[2] = {25, 50};
        int publish_mqtt = strcmp(argv[1], "--map-sample-mqtt") == 0;
        puts("PC_SIMULATED mapping sample; no serial or GPIO opened.");
        return show_measurements(sample_registers, publish_mqtt, 0);
    }
    int stm32_mode = argc == 2 && (strcmp(argv[1], "--stm32") == 0 ||
                                  strcmp(argv[1], "--stm32-mqtt") == 0);
    int publish_mqtt = argc == 2 && (strcmp(argv[1], "--serial-mqtt") == 0 ||
                                    strcmp(argv[1], "--stm32-mqtt") == 0);
    if (argc != 1 && !publish_mqtt && !stm32_mode) {
        fprintf(stderr, "Usage: %s [--map-sample|--map-sample-mqtt|--serial-mqtt|--stm32|--stm32-mqtt]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
#ifndef EDGEVISION_ENABLE_MQTT
    if (publish_mqtt) {
        fputs("MQTT disabled in this build; no serial or GPIO opened.\n", stderr);
        return EXIT_FAILURE;
    }
#endif
    if (!MODBUS_QUERY_EXERCISE_READY) {
        fputs("Exercise incomplete: fill TODO 1-3 and review, then set "
              "MODBUS_QUERY_EXERCISE_READY=1. No hardware opened.\n", stderr);
        return EXIT_FAILURE;
    }

    real_serial_source_t source = REAL_SERIAL_SOURCE_INITIALIZER;
    const real_serial_source_config_t config = {
        .serial_path = "/dev/ttyS5",
        .gpiochip_path = "/dev/gpiochip0",
        .line_offset = 22U,
        /* open需要有效配置；事务自行分段收帧，不调用定长source_read。 */
        .receive_length = 9,
        .timeout_ms = 1000,
    };
    uint16_t registers[2] = {0};
    uint8_t exception_code = 0;
    int exit_code = EXIT_FAILURE;

    if (graceful_shutdown_install() < 0) {
        perror("graceful_shutdown_install");
        return EXIT_FAILURE;
    }
    if (demo_should_stop(NULL))
        return EXIT_SUCCESS;

    if (real_serial_source_open(&source, &config) < 0) {
        perror("real_serial_source_open");
        return EXIT_FAILURE;
    }

    if (stm32_mode) {
        puts("STM32 DHT11: slave=1 function=04 start=1 quantity=2; values / 10.");
        /* 以下是早期接入提示的原文；固件规则后来已核对，但保守quality尚未调整。
         * 本次仅补注释，保留输出字符串以免把文档整理混成运行行为修改。 */
        puts("quality=uncertain: sensor failure/freshness encoding not yet confirmed.");
    } else {
        puts("PC-simulated Modbus peer only; raw registers, not sensor measurements.");
    }
    query_result_t result =
        query_two_registers(&source, stm32_mode, registers, &exception_code);
    int saved_errno = errno;

    print_bytes("RX", source.buffer, source.received);
    switch (result) {
    case QUERY_OK:
        printf("QUERY OK: source=%s reg[0]=%u reg[1]=%u\n",
               stm32_mode ? "STM32_DHT11" : "PC_SIMULATED",
               (unsigned int)registers[0], (unsigned int)registers[1]);
        exit_code = show_measurements(registers, publish_mqtt, stm32_mode);
        break;
    case QUERY_TIMEOUT:
        printf("QUERY TIMEOUT: received=%zu, RX budget=%d ms\n",
               source.received, source.timeout_ms);
        break;
    case QUERY_EXCEPTION:
        printf("QUERY EXCEPTION: code=0x%02X\n", (unsigned int)exception_code);
        break;
    case QUERY_INVALID:
        puts("QUERY INVALID: response does not pass request/length/CRC checks");
        break;
    case QUERY_IO_ERROR:
        if (saved_errno == ECANCELED) {
            printf("QUERY CANCELED: received=%zu\n", source.received);
            exit_code = EXIT_SUCCESS;
        } else {
            errno = saved_errno;
            perror("QUERY IO ERROR");
        }
        break;
    case QUERY_INCOMPLETE:
        puts("QUERY INCOMPLETE: TODO logic has not been completed");
        break;
    }
    real_serial_source_close(&source);
    puts("RS485 closed");
    return exit_code;
}
