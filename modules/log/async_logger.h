#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H

#include "../blocking_queue/blocking_queue.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* 单条日志正文（不含时间、级别等格式化字段）的最大存储长度。 */
#define LOG_TEXT_MAX 256
/* 日志文件路径缓冲区大小，包含字符串结尾的 '\0'。 */
#define ASYNC_LOGGER_PATH_MAX 4096

/** 日志级别，数值越大表示严重程度越高。 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

/**
 * 日志持久化策略。
 *
 * BUFFERED 模式优先吞吐量，消息进入内存队列后即可返回；
 * FSYNC_EACH 模式优先持久性，每条消息写入并同步到存储设备后才返回。
 */
typedef enum {
    /* 消息成功进入有界队列后返回。 */
    ASYNC_LOGGER_BUFFERED = 0,
    /* 写线程完成 fflush 和 fsync 后返回。 */
    ASYNC_LOGGER_FSYNC_EACH
} async_logger_durability_t;

/** 异步日志器的扩展配置。 */
typedef struct {
    const char *path;                     /* 当前活动日志文件路径。 */
    size_t capacity;                      /* 队列最多容纳的消息数。 */
    size_t rotation_bytes;                /* 文件轮转阈值（字节），0 表示禁用轮转。 */
    unsigned int rotation_files;          /* 保留的 .N 或 .N.gz 历史文件数量。 */
    int compress_rotated;                 /* 非 0 时使用 gzip 压缩轮转文件。 */
    async_logger_durability_t durability; /* 吞吐量与持久性的取舍策略。 */
} async_logger_config_t;

/**
 * 队列中的日志消息。
 *
 * 生产线程在提交时填充该结构，写线程随后将其格式化为一行文本。
 */
typedef struct {
    uint64_t sequence;          /* 全局递增序号，用于标识写入顺序。 */
    struct timespec timestamp;  /* 提交时的实时时钟时间，精确到纳秒。 */
    unsigned long thread_id;    /* 提交日志的线程标识。 */
    log_level_t level;          /* 日志级别。 */
    char text[LOG_TEXT_MAX];    /* 以 '\0' 结尾的日志正文。 */
} log_message_t;

/**
 * 异步日志器运行时状态。
 *
 * 该结构由初始化函数建立，由 shutdown 和 destroy 按顺序关闭、释放。
 * 调用方不应在日志器运行期间直接修改其中字段。
 */
typedef struct {
    bounded_queue_t queue;       /* 连接生产线程与写线程的有界队列。 */
    pthread_t writer_thread;     /* 唯一执行文件写入、轮转和压缩的线程。 */
    FILE *file;                  /* 当前活动日志文件。 */

    pthread_mutex_t stats_mutex; /* 保护计数器、持久序号和写错误状态。 */
    pthread_mutex_t submit_mutex;/* 串行化 FSYNC_EACH 模式下的提交。 */
    pthread_cond_t durable_cond; /* 通知等待中的生产线程消息已持久化。 */

    uint64_t next_sequence;      /* 下一条消息使用的序号。 */
    uint64_t accepted;           /* 已成功进入队列的消息数。 */
    uint64_t written;            /* 已由写线程写入文件的消息数。 */
    uint64_t rejected;           /* 因队列关闭等原因被拒绝的消息数。 */
    uint64_t durable_sequence;   /* 已确认同步到存储设备的最大连续序号。 */
    int write_error;             /* 写入、同步、轮转或压缩发生错误的标志。 */

    char path[ASYNC_LOGGER_PATH_MAX];     /* 当前活动日志文件路径副本。 */
    size_t rotation_bytes;                /* 文件轮转阈值。 */
    size_t current_bytes;                 /* 当前活动文件的已知字节数。 */
    unsigned int rotation_files;          /* 历史轮转文件保留数量。 */
    int compress_rotated;                 /* 是否压缩轮转文件。 */
    async_logger_durability_t durability; /* 当前持久化策略。 */

    int initialized;          /* 初始化成功标志。 */
    int shutdown_complete;    /* 写线程已退出且队列已排空的标志。 */
} async_logger_t;

/**
 * 使用默认策略初始化日志器。
 *
 * 默认不轮转、不压缩，并使用 ASYNC_LOGGER_BUFFERED 模式。
 *
 * @return 成功返回 0，参数无效或资源初始化失败返回 -1。
 */
int async_logger_init(async_logger_t *logger,
                      const char *path,
                      size_t capacity);

/**
 * 使用完整配置初始化日志器，并启动后台写线程。
 *
 * 当 rotation_bytes 大于 0 时，rotation_files 必须大于 0。
 *
 * @return 成功返回 0，失败返回 -1。
 */
int async_logger_init_ex(async_logger_t *logger,
                         const async_logger_config_t *config);

/**
 * 提交一条已经组织好的日志正文。
 *
 * BUFFERED 模式下，消息进入队列即返回 0；FSYNC_EACH 模式下，消息完成
 * fflush 和 fsync 后返回 0。参数无效、队列关闭或发生 I/O 错误时返回 -1。
 * 超过 LOG_TEXT_MAX - 1 字节的正文会被截断。
 */
int async_logger_log(async_logger_t *logger,
                     log_level_t level,
                     const char *text);

/**
 * 关闭队列，等待已接收消息全部写出并同步，然后回收写线程。
 *
 * @return 正常关闭返回 0；状态无效、线程操作或写入失败返回 -1。
 */
int async_logger_shutdown(async_logger_t *logger);

/**
 * 释放文件、队列和同步原语等资源。
 *
 * 仅可在 async_logger_shutdown() 成功后调用。
 *
 * @return 全部资源成功释放返回 0，否则返回 -1。
 */
int async_logger_destroy(async_logger_t *logger);

#endif
