#include "gateway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LOG_QUEUE_CAPACITY 256u

#ifdef EDGEVISION_ENABLE_MQTT

static int parse_port(const char *text, int *output)
{
    char *end = NULL;
    long value;

    if (text == NULL || output == NULL)
    {
        return -1;
    }

    value = strtol(text, &end, 10);

    if (end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535)
    {
        return -1;
    }

    *output = (int)value;
    return 0;
}

#endif
/**
 * @brief 解析网关程序的命令行参数。
 *
 * 支持以下调用方式：
 * - `gateway`：以前台服务模式运行，日志写入 `gateway.log`。
 * - `gateway <日志路径>`：以前台服务模式运行，并使用指定日志文件。
 * - `gateway --smoke`：执行网络回环冒烟测试，日志写入
 *   `gateway-smoke.log`。
 * - `gateway --smoke <日志路径>`：执行网络回环冒烟测试，并使用指定日志文件。
 *
 * @param argc 命令行参数个数。
 * @param argv 命令行参数数组。
 * @param config 输出参数，成功时写入网关运行配置。
 * @return 解析成功返回 0；参数无效或输出指针为空时返回 -1。
 */
static int parse_arguments(int argc, char **argv, gateway_config_t *config)
{
    if (config == NULL) {
        return -1;
    }

    /* 默认配置 */
    config->mode = GATEWAY_MODE_SERVICE;
    config->log_path = "gateway.log";
    config->log_queue_capacity = DEFAULT_LOG_QUEUE_CAPACITY;
    config->mqtt_host = "127.0.0.1";
    config->mqtt_port = 1883;

    if (argc == 1) {
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--smoke") == 0) {
        config->mode = GATEWAY_MODE_SMOKE;
        config->log_path = "gateway-smoke.log";
        return 0;
    }
#ifdef EDGEVISION_ENABLE_MQTT
    if (argc == 2 && strcmp(argv[1], "--mqtt-smoke") == 0)
    {
        config->mode = GATEWAY_MODE_MQTT_SMOKE;
        config->log_path = "gateway-mqtt-smoke.log";
        return 0;
    }
#endif

    if (argc == 2) {
        config->log_path = argv[1];
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0) {
        config->mode = GATEWAY_MODE_SMOKE;
        config->log_path = argv[2];
        return 0;
    }

#ifdef EDGEVISION_ENABLE_MQTT
    if (argc == 4 &&
        strcmp(argv[1], "--mqtt-smoke") == 0)
    {
        if (argv[2][0] == '\0' ||
            parse_port(argv[3], &config->mqtt_port) != 0)
        {
            fprintf(stderr, "invalid MQTT host or port\n");
            return -1;
        }

        config->mode = GATEWAY_MODE_MQTT_SMOKE;
        config->log_path = "gateway-mqtt-smoke.log";
        config->mqtt_host = argv[2];
        return 0;
    }
#endif

    fprintf(
        stderr,
        "usage: %s [--smoke [log_path] | "
        "--mqtt-smoke [host port] | log_path]\n",
        argv[0]);
    return -1;
}

/**
 * @brief 网关程序入口。
 *
 * 入口层只负责解析参数并把配置交给核心编排层，网络、协议、日志和退出流程
 * 均由 gateway_run() 管理。
 *
 * @param argc 命令行参数个数。
 * @param argv 命令行参数数组。
 * @return 启动和运行成功返回 EXIT_SUCCESS，否则返回 EXIT_FAILURE。
 */
int main(int argc, char **argv)
{
    gateway_config_t config;

    if (parse_arguments(argc, argv, &config) != 0) {
        return EXIT_FAILURE;
    }

    return gateway_run(&config) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
