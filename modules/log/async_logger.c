#define _POSIX_C_SOURCE 200809L

#include "async_logger.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* 格式化后单行日志所使用的临时缓冲区大小。 */
#define LOG_LINE_MAX 768
/* gzip 压缩时每次读取、写入的数据块大小。 */
#define COPY_BUFFER_SIZE 16384

/* 将日志级别转换为写入文件的固定文本。 */
static const char *level_to_string(log_level_t level)
{
    switch (level)
    {
    case LOG_LEVEL_DEBUG:
        return "DEBUG";
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARN:
        return "WARN";
    case LOG_LEVEL_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

/*
 * 记录后台写线程错误并唤醒所有等待持久化确认的生产线程。
 * 关闭队列还会阻止后续提交，并让可能阻塞的队列操作及时退出。
 */
static void mark_writer_error(async_logger_t *logger)
{
    pthread_mutex_lock(&logger->stats_mutex);
    logger->write_error = 1;
    pthread_cond_broadcast(&logger->durable_cond);
    pthread_mutex_unlock(&logger->stats_mutex);
    bq_close(&logger->queue);
}

/* 根据轮转序号和压缩选项生成“原路径.N[.gz]”形式的文件名。 */
static int make_rotated_path(const async_logger_t *logger,
                             unsigned int index,
                             int compressed,
                             char *out,
                             size_t out_size)
{
    int n = snprintf(out,
                     out_size,
                     compressed ? "%s.%u.gz" : "%s.%u",
                     logger->path,
                     index);

    if (n < 0 || (size_t)n >= out_size)
    {
        return -1;
    }
    return 0;
}

/*
 * 将 source 流式压缩到 destination。
 * 只有压缩文件完整写入并关闭后才删除源文件；失败时清理不完整的目标文件。
 */
static int gzip_file(const char *source, const char *destination)
{
    unsigned char buffer[COPY_BUFFER_SIZE];
    FILE *input = fopen(source, "rb");
    gzFile output;
    int result = -1;

    if (input == NULL)
    {
        return -1;
    }
    output = gzopen(destination, "wb");
    if (output == NULL)
    {
        fclose(input);
        return -1;
    }

    for (;;)
    {
        size_t count = fread(buffer, 1, sizeof(buffer), input);

        if (count > 0 &&
            gzwrite(output, buffer, (unsigned int)count) != (int)count)
        {
            goto DONE;
        }
        if (count < sizeof(buffer))
        {
            if (ferror(input))
            {
                goto DONE;
            }
            break;
        }
    }

    if (gzclose(output) != Z_OK)
    {
        output = NULL;
        goto DONE;
    }
    output = NULL;

    if (fclose(input) != 0)
    {
        input = NULL;
        goto DONE;
    }
    input = NULL;

    if (unlink(source) != 0)
    {
        goto DONE;
    }
    result = 0;

DONE:
    if (output != NULL)
    {
        gzclose(output);
    }
    if (input != NULL)
    {
        fclose(input);
    }
    if (result != 0)
    {
        unlink(destination);
    }
    return result;
}

/* 将 stdio 缓冲区刷新到内核，并要求内核把文件数据同步到存储设备。 */
static int sync_stream(FILE *file)
{
    if (fflush(file) != 0)
    {
        return -1;
    }
    if (fsync(fileno(file)) != 0)
    {
        return -1;
    }
    return 0;
}

/*
 * 执行一次日志轮转。
 *
 * 历史文件按 .1 -> .2 的方向移动，当前活动文件变为 .1；启用压缩时
 * 历史文件使用 .N.gz 命名。最后重新打开一个空的活动日志文件。
 */
static int rotate_file(async_logger_t *logger)
{
    char older[ASYNC_LOGGER_PATH_MAX];
    char newer[ASYNC_LOGGER_PATH_MAX];
    char first_plain[ASYNC_LOGGER_PATH_MAX];
    char first_compressed[ASYNC_LOGGER_PATH_MAX];

    if (sync_stream(logger->file) != 0 || fclose(logger->file) != 0)
    {
        logger->file = NULL;
        return -1;
    }
    logger->file = NULL;

    /* 从最大序号向后移动，避免较新的历史文件覆盖尚未搬移的旧文件。 */
    for (unsigned int index = logger->rotation_files; index > 1; --index)
    {
        if (make_rotated_path(logger,
                              index - 1,
                              logger->compress_rotated,
                              older,
                              sizeof(older)) != 0 ||
            make_rotated_path(logger,
                              index,
                              logger->compress_rotated,
                              newer,
                              sizeof(newer)) != 0)
        {
            return -1;
        }
        if (rename(older, newer) != 0 && errno != ENOENT)
        {
            return -1;
        }
    }

    if (make_rotated_path(logger, 1, 0, first_plain, sizeof(first_plain)) != 0)
    {
        return -1;
    }

    if (logger->compress_rotated)
    {
        if (make_rotated_path(logger,
                              1,
                              1,
                              first_compressed,
                              sizeof(first_compressed)) != 0)
        {
            return -1;
        }
        unlink(first_compressed);
    }
    else
    {
        unlink(first_plain);
    }

    if (rename(logger->path, first_plain) != 0)
    {
        return -1;
    }
    if (logger->compress_rotated &&
        gzip_file(first_plain, first_compressed) != 0)
    {
        return -1;
    }

    logger->file = fopen(logger->path, "a");
    if (logger->file == NULL)
    {
        return -1;
    }
    logger->current_bytes = 0;
    return 0;
}

/* 将队列消息格式化为最终写入文件的一行日志。 */
static int format_line(const log_message_t *message,
                       char *line,
                       size_t line_size)
{
    struct tm tm_info;
    char time_buffer[64];
    int length;

    if (localtime_r(&message->timestamp.tv_sec, &tm_info) == NULL)
    {
        return -1;
    }
    if (strftime(time_buffer,
                 sizeof(time_buffer),
                 "%Y-%m-%d %H:%M:%S",
                 &tm_info) == 0)
    {
        return -1;
    }

    length = snprintf(line,
                      line_size,
                      "[%" PRIu64 "] [%s.%09ld] [%-5s] [thread=%lu] %s\n",
                      message->sequence,
                      time_buffer,
                      message->timestamp.tv_nsec,
                      level_to_string(message->level),
                      message->thread_id,
                      message->text);

    if (length < 0 || (size_t)length >= line_size)
    {
        return -1;
    }
    return length;
}

/*
 * 写入单条消息，并在写入前按大小阈值执行轮转。
 * FSYNC_EACH 模式还会在每条消息后立即刷新并同步文件。
 */
static int write_one(async_logger_t *logger, const log_message_t *message)
{
    char line[LOG_LINE_MAX];
    int length = format_line(message, line, sizeof(line));

    if (length < 0)
    {
        return -1;
    }
    if (logger->rotation_bytes > 0 &&
        logger->rotation_files > 0 &&
        logger->current_bytes > 0 &&
        logger->current_bytes + (size_t)length > logger->rotation_bytes)
    {
        if (rotate_file(logger) != 0)
        {
            return -1;
        }
    }
    if (fwrite(line, 1, (size_t)length, logger->file) != (size_t)length)
    {
        return -1;
    }
    logger->current_bytes += (size_t)length;

    if (logger->durability == ASYNC_LOGGER_FSYNC_EACH &&
        sync_stream(logger->file) != 0)
    {
        return -1;
    }
    return 0;
}

/* 后台写线程入口：持续消费队列，统一负责写文件、轮转和压缩。 */
static void *writer_main(void *arg)
{
    async_logger_t *logger = arg;
    log_message_t message;

    /* FILE、轮转和 gzip 只由本线程访问，避免额外的文件操作锁。 */
    for (;;)
    {
        bq_result_t rc = bq_pop(&logger->queue, &message, -1);

        if (rc == BQ_CLOSED)
        {
            break;
        }
        if (rc != BQ_OK)
        {
            mark_writer_error(logger);
            return NULL;
        }
        if (write_one(logger, &message) != 0)
        {
            mark_writer_error(logger);
            return NULL;
        }

        pthread_mutex_lock(&logger->stats_mutex);
        logger->written++;
        if (logger->durability == ASYNC_LOGGER_FSYNC_EACH)
        {
            /* 当前序号已确认落盘，唤醒等待持久化结果的生产线程。 */
            logger->durable_sequence = message.sequence;
            pthread_cond_broadcast(&logger->durable_cond);
        }
        pthread_mutex_unlock(&logger->stats_mutex);
    }

    /* 队列关闭且已排空，说明此前接收的消息均已被消费，退出前统一同步。 */
    if (sync_stream(logger->file) != 0)
    {
        mark_writer_error(logger);
    }
    return NULL;
}

/*
 * 初始化过程中启动写线程之前发生失败时，按已成功创建的资源逆序清理。
 * 各 ready 参数用于避免销毁尚未初始化的资源。
 */
static void cleanup_before_writer(async_logger_t *logger,
                                  int file_ready,
                                  int cond_ready,
                                  int submit_ready,
                                  int stats_ready,
                                  int queue_ready)
{
    if (file_ready)
    {
        fclose(logger->file);
        logger->file = NULL;
    }
    if (cond_ready)
    {
        pthread_cond_destroy(&logger->durable_cond);
    }
    if (submit_ready)
    {
        pthread_mutex_destroy(&logger->submit_mutex);
    }
    if (stats_ready)
    {
        pthread_mutex_destroy(&logger->stats_mutex);
    }
    if (queue_ready)
    {
        bq_destroy(&logger->queue);
    }
}

/* 使用扩展配置创建队列、同步原语、日志文件和后台写线程。 */
int async_logger_init_ex(async_logger_t *logger,
                         const async_logger_config_t *config)
{
    struct stat file_stat;
    int queue_ready = 0;
    int stats_ready = 0;
    int submit_ready = 0;
    int cond_ready = 0;
    int file_ready = 0;
    int n;

    if (logger == NULL || config == NULL || config->path == NULL ||
        config->capacity == 0 ||
        (config->durability != ASYNC_LOGGER_BUFFERED &&
         config->durability != ASYNC_LOGGER_FSYNC_EACH) ||
        (config->rotation_bytes > 0 && config->rotation_files == 0))
    {
        return -1;
    }

    memset(logger, 0, sizeof(*logger));
    n = snprintf(logger->path, sizeof(logger->path), "%s", config->path);
    if (n < 0 || (size_t)n >= sizeof(logger->path))
    {
        return -1;
    }

    logger->next_sequence = 1;
    logger->rotation_bytes = config->rotation_bytes;
    logger->rotation_files = config->rotation_files;
    logger->compress_rotated = config->compress_rotated != 0;
    logger->durability = config->durability;

    if (bq_init(&logger->queue,
                config->capacity,
                sizeof(log_message_t)) != BQ_OK)
    {
        goto FAIL;
    }
    queue_ready = 1;
    if (pthread_mutex_init(&logger->stats_mutex, NULL) != 0)
    {
        goto FAIL;
    }
    stats_ready = 1;
    if (pthread_mutex_init(&logger->submit_mutex, NULL) != 0)
    {
        goto FAIL;
    }
    submit_ready = 1;
    if (pthread_cond_init(&logger->durable_cond, NULL) != 0)
    {
        goto FAIL;
    }
    cond_ready = 1;

    logger->file = fopen(logger->path, "a");
    if (logger->file == NULL)
    {
        goto FAIL;
    }
    file_ready = 1;
    if (fstat(fileno(logger->file), &file_stat) != 0)
    {
        goto FAIL;
    }
    logger->current_bytes = (size_t)file_stat.st_size;

    /* 所有依赖资源准备完成后再启动写线程，避免线程观察到半初始化状态。 */
    if (pthread_create(&logger->writer_thread,
                       NULL,
                       writer_main,
                       logger) != 0)
    {
        goto FAIL;
    }

    logger->initialized = 1;
    return 0;

FAIL:
    cleanup_before_writer(logger,
                          file_ready,
                          cond_ready,
                          submit_ready,
                          stats_ready,
                          queue_ready);
    return -1;
}

/* 使用无轮转、BUFFERED 持久化策略的简化初始化接口。 */
int async_logger_init(async_logger_t *logger,
                      const char *path,
                      size_t capacity)
{
    const async_logger_config_t config = {
        .path = path,
        .capacity = capacity,
        .rotation_bytes = 0,
        .rotation_files = 0,
        .compress_rotated = 0,
        .durability = ASYNC_LOGGER_BUFFERED,
    };

    return async_logger_init_ex(logger, &config);
}

/* 构造消息并将其提交到有界队列；必要时等待写线程确认消息已落盘。 */
int async_logger_log(async_logger_t *logger,
                     log_level_t level,
                     const char *text)
{
    log_message_t message;
    bq_result_t push_result;
    int durable_mode;

    if (logger == NULL || !logger->initialized || text == NULL ||
        level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_ERROR)
    {
        return -1;
    }

    durable_mode = logger->durability == ASYNC_LOGGER_FSYNC_EACH;
    if (durable_mode)
    {
        /*
         * 串行化持久模式的提交，使消息序号、入队顺序和每次调用等待的
         * 持久化确认始终对应同一条记录。
         */
        pthread_mutex_lock(&logger->submit_mutex);
    }

    pthread_mutex_lock(&logger->stats_mutex);
    message.sequence = logger->next_sequence++;
    pthread_mutex_unlock(&logger->stats_mutex);

    if (clock_gettime(CLOCK_REALTIME, &message.timestamp) != 0)
    {
        if (durable_mode)
        {
            pthread_mutex_unlock(&logger->submit_mutex);
        }
        return -1;
    }
    message.thread_id = (unsigned long)pthread_self();
    message.level = level;
    snprintf(message.text, sizeof(message.text), "%s", text);

    push_result = bq_push(&logger->queue, &message, -1);
    if (push_result != BQ_OK)
    {
        pthread_mutex_lock(&logger->stats_mutex);
        logger->rejected++;
        pthread_mutex_unlock(&logger->stats_mutex);
        if (durable_mode)
        {
            pthread_mutex_unlock(&logger->submit_mutex);
        }
        return -1;
    }

    pthread_mutex_lock(&logger->stats_mutex);
    logger->accepted++;
    pthread_mutex_unlock(&logger->stats_mutex);

    if (durable_mode)
    {
        int durable;

        pthread_mutex_unlock(&logger->submit_mutex);
        pthread_mutex_lock(&logger->stats_mutex);
        /* 使用条件谓词循环处理虚假唤醒，并在写线程失败时及时退出。 */
        while (logger->durable_sequence < message.sequence &&
               !logger->write_error)
        {
            pthread_cond_wait(&logger->durable_cond, &logger->stats_mutex);
        }
        durable = logger->durable_sequence >= message.sequence &&
                  !logger->write_error;
        pthread_mutex_unlock(&logger->stats_mutex);
        return durable ? 0 : -1;
    }

    return 0;
}

/* 停止接收新消息，排空队列并等待后台写线程退出。 */
int async_logger_shutdown(async_logger_t *logger)
{
    int write_error;

    if (logger == NULL || !logger->initialized || logger->shutdown_complete)
    {
        return -1;
    }
    /* 关闭操作会唤醒队列等待者；写线程会在退出前消费完已有消息。 */
    if (bq_close(&logger->queue) != BQ_OK)
    {
        return -1;
    }
    if (pthread_join(logger->writer_thread, NULL) != 0)
    {
        return -1;
    }

    logger->shutdown_complete = 1;
    pthread_mutex_lock(&logger->stats_mutex);
    write_error = logger->write_error;
    pthread_mutex_unlock(&logger->stats_mutex);
    return write_error ? -1 : 0;
}

/* 在 shutdown 完成后释放日志器持有的全部系统资源。 */
int async_logger_destroy(async_logger_t *logger)
{
    int result = 0;

    if (logger == NULL || !logger->initialized || !logger->shutdown_complete)
    {
        return -1;
    }

    if (fclose(logger->file) != 0)
    {
        result = -1;
    }
    logger->file = NULL;
    if (bq_destroy(&logger->queue) != BQ_OK)
    {
        result = -1;
    }
    if (pthread_cond_destroy(&logger->durable_cond) != 0)
    {
        result = -1;
    }
    if (pthread_mutex_destroy(&logger->submit_mutex) != 0)
    {
        result = -1;
    }
    if (pthread_mutex_destroy(&logger->stats_mutex) != 0)
    {
        result = -1;
    }

    logger->initialized = 0;
    return result;
}
