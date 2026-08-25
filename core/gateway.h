#ifndef EDGEVISION_CORE_GATEWAY_H
#define EDGEVISION_CORE_GATEWAY_H

#include <stddef.h>

/** Gateway 的运行模式。 */
typedef enum
{
    GATEWAY_MODE_SERVICE = 0, /* 常驻运行，等待 SIGINT/SIGTERM。 */
    GATEWAY_MODE_SMOKE = 1    /* 执行一次本地整链自检后退出。 */
} gateway_mode_t;

/** 启动 Gateway 所需的最小配置。 */
typedef struct
{
    gateway_mode_t mode;       /* 常驻模式或 smoke 模式。 */
    const char *log_path;       /* 日志文件路径，不能为空。 */
    size_t log_queue_capacity;  /* 异步日志队列容量，必须大于 0。 */
} gateway_config_t;

/**
 * @brief 按配置运行 Gateway，并负责信号、日志和业务核心的完整生命周期。
 *
 * @param config 调用方提供的运行配置，不能为空。
 * @return 0 表示正常退出或 smoke 通过；-1 表示初始化、运行或清理失败。
 *
 * @note apps/gateway/main.c 只负责解析命令行，然后调用本函数。后续设备管理和消息
 *       路由也应由 core 层协调，不再回填到 main.c。
 */
int gateway_run(const gateway_config_t *config);

#endif
