#include "mqtt_publisher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * 该测试只验证不依赖真实 Broker 的确定性行为：参数校验、初始状态、
 * 多实例全局资源管理，以及启动/停止的基本生命周期。
 */

/* 所有测试从同一份合法配置复制，再单独破坏待测字段。 */
static const mqtt_publisher_config_t valid_config = {
    .host = "127.0.0.1",
    .port = 1,
    .client_id = "edgevision-mqtt-test",
    .topic_prefix = "edgevision/v1/devices",
    .keepalive_seconds = 30,
    .reconnect_delay_seconds = 1,
    .reconnect_delay_max_seconds = 8
};

/* publish 测试需要先通过领域层校验，因此准备一条完整合法的 Measurement。 */
static const measurement_t valid_measurement = {
    .schema_version = MEASUREMENT_SCHEMA_VERSION,
    .device_id = "test-device-01",
    .sequence = 1,
    .timestamp_ms = INT64_C(1787623200000),
    .metric = "temperature",
    .value = 26.5,
    .unit = "celsius",
    .quality = MEASUREMENT_QUALITY_GOOD
};

/* 统一比较返回码，让失败输出包含用例名、实际值和期望值。 */
static int expect_result(const char *case_name,
                         mqtt_publisher_result_t actual,
                         mqtt_publisher_result_t expected)
{
    if (actual == expected)
        return 0;

    fprintf(stderr,
            "%s failed: actual=%d expected=%d\n",
            case_name,
            actual,
            expected);
    return -1;
}

static int test_invalid_arguments(void)
{
    mqtt_publisher_config_t config = valid_config;

    /* create 必须拒绝空配置。 */
    if (mqtt_publisher_create(NULL) != NULL)
        return -1;

    /* host 必须为非空字符串。 */
    config.host = "";
    if (mqtt_publisher_create(&config) != NULL)
        return -1;

    /* MQTT 端口必须位于 1～65535。 */
    config = valid_config;
    config.port = 0;
    if (mqtt_publisher_create(&config) != NULL)
        return -1;

    /* libmosquitto 要求 Keep Alive 至少为 5 秒。 */
    config = valid_config;
    config.keepalive_seconds = 4;
    if (mqtt_publisher_create(&config) != NULL)
        return -1;

    /* 指数退避的初始值不能大于最大值。 */
    config = valid_config;
    config.reconnect_delay_seconds = 9;
    if (mqtt_publisher_create(&config) != NULL)
        return -1;

    /* 公开接口应统一处理 NULL，不能发生解引用或崩溃。 */
    if (expect_result("start NULL",
                      mqtt_publisher_start(NULL),
                      MQTT_PUBLISHER_INVALID_ARGUMENT) != 0 ||
        expect_result("wait NULL",
                      mqtt_publisher_wait_connected(NULL, 1),
                      MQTT_PUBLISHER_INVALID_ARGUMENT) != 0 ||
        expect_result("publish NULL",
                      mqtt_publisher_publish(NULL, &valid_measurement, 1),
                      MQTT_PUBLISHER_INVALID_ARGUMENT) != 0 ||
        mqtt_publisher_is_connected(NULL))
    {
        return -1;
    }

    /* void 清理接口对 NULL 保持无操作语义。 */
    mqtt_publisher_stop(NULL);
    mqtt_publisher_destroy(NULL);
    return 0;
}

static int test_stopped_publisher(void)
{
    mqtt_publisher_t *publisher = mqtt_publisher_create(&valid_config);
    int failed = 0;

    if (publisher == NULL)
    {
        fprintf(stderr, "create stopped publisher failed\n");
        return -1;
    }

    /* create 只创建资源，不会隐式连接；发布也必须被拒绝。 */
    if (mqtt_publisher_is_connected(publisher) ||
        expect_result("wait while stopped",
                      mqtt_publisher_wait_connected(publisher, 1),
                      MQTT_PUBLISHER_NOT_CONNECTED) != 0 ||
        expect_result("publish while stopped",
                      mqtt_publisher_publish(publisher,
                                             &valid_measurement,
                                             1),
                      MQTT_PUBLISHER_NOT_CONNECTED) != 0)
    {
        failed = -1;
    }

    /* STOPPED 状态下重复停止必须安全且无副作用。 */
    mqtt_publisher_stop(publisher);
    mqtt_publisher_stop(publisher);
    mqtt_publisher_destroy(publisher);
    return failed;
}

static int test_multiple_instances(void)
{
    mqtt_publisher_config_t second_config = valid_config;
    mqtt_publisher_t *first;
    mqtt_publisher_t *second;

    /* MQTT 客户端 ID 必须唯一，避免 Broker 用新连接踢掉旧连接。 */
    second_config.client_id = "edgevision-mqtt-test-2";
    first = mqtt_publisher_create(&valid_config);
    second = mqtt_publisher_create(&second_config);

    if (first == NULL || second == NULL)
    {
        mqtt_publisher_destroy(first);
        mqtt_publisher_destroy(second);
        fprintf(stderr, "multiple publisher creation failed\n");
        return -1;
    }

    /* 销毁一个实例不能提前清理另一个实例仍依赖的 libmosquitto。 */
    mqtt_publisher_destroy(first);
    if (mqtt_publisher_is_connected(second))
    {
        mqtt_publisher_destroy(second);
        return -1;
    }

    mqtt_publisher_destroy(second);
    return 0;
}

static int test_start_and_stop(void)
{
    mqtt_publisher_t *publisher = mqtt_publisher_create(&valid_config);
    mqtt_publisher_result_t start_result;

    if (publisher == NULL)
        return -1;

    /*
     * 端口 1 不要求存在 Broker。普通环境会成功启动网络线程；禁止网络系统
     * 调用的沙箱也可能让 connect_async 立即返回库错误，两者都是合法结果。
     */
    start_result = mqtt_publisher_start(publisher);
    if (start_result != MQTT_PUBLISHER_OK &&
        start_result != MQTT_PUBLISHER_LIBRARY_ERROR)
    {
        mqtt_publisher_destroy(publisher);
        return -1;
    }

    if (start_result == MQTT_PUBLISHER_OK &&
        expect_result("duplicate start",
                      mqtt_publisher_start(publisher),
                      MQTT_PUBLISHER_INVALID_ARGUMENT) != 0)
    {
        mqtt_publisher_destroy(publisher);
        return -1;
    }

    /* 同时覆盖 CONNECTING/DISCONNECTED 到 STOPPED 的清理路径。 */
    mqtt_publisher_stop(publisher);
    mqtt_publisher_stop(publisher);
    /* stop 返回后实例必须处于非连接状态，之后仍可安全 destroy。 */
    if (mqtt_publisher_is_connected(publisher))
    {
        mqtt_publisher_destroy(publisher);
        return -1;
    }

    mqtt_publisher_destroy(publisher);
    return 0;
}

int main(void)
{
    /* 任一子测试失败都让进程返回非零，CTest 会据此判定失败。 */
    if (test_invalid_arguments() != 0 ||
        test_stopped_publisher() != 0 ||
        test_multiple_instances() != 0 ||
        test_start_and_stop() != 0)
    {
        fprintf(stderr, "mqtt publisher tests failed\n");
        return EXIT_FAILURE;
    }

    puts("mqtt publisher tests passed");
    return EXIT_SUCCESS;
}
