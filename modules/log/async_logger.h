#ifndef ASYNC_LOGGER_H
#define ASYNC_LOGGER_H

#include "../blocking_queue/blocking_queue.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define LOG_TEXT_MAX 256
#define ASYNC_LOGGER_PATH_MAX 4096

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

typedef enum {
    /* Return after the message enters the bounded queue. */
    ASYNC_LOGGER_BUFFERED = 0,
    /* Return only after the writer has flushed and fsynced this message. */
    ASYNC_LOGGER_FSYNC_EACH
} async_logger_durability_t;

typedef struct {
    const char *path;                    /* Active log file. */
    size_t capacity;                     /* Number of queued messages. */
    size_t rotation_bytes;               /* 0 disables size rotation. */
    unsigned int rotation_files;         /* Retained .N or .N.gz files. */
    int compress_rotated;                /* Non-zero enables gzip. */
    async_logger_durability_t durability; /* Throughput/durability choice. */
} async_logger_config_t;

typedef struct {
    uint64_t sequence;
    struct timespec timestamp;
    unsigned long thread_id;
    log_level_t level;
    char text[LOG_TEXT_MAX];
} log_message_t;

typedef struct {
    bounded_queue_t queue;
    pthread_t writer_thread;
    FILE *file;

    pthread_mutex_t stats_mutex;
    pthread_mutex_t submit_mutex;
    pthread_cond_t durable_cond;

    uint64_t next_sequence;
    uint64_t accepted;
    uint64_t written;
    uint64_t rejected;
    uint64_t durable_sequence;
    int write_error;

    char path[ASYNC_LOGGER_PATH_MAX];
    size_t rotation_bytes;
    size_t current_bytes;
    unsigned int rotation_files;
    int compress_rotated;
    async_logger_durability_t durability;

    int initialized;
    int shutdown_complete;
} async_logger_t;

int async_logger_init(async_logger_t *logger,
                      const char *path,
                      size_t capacity);

/* Extended initializer for rotation, compression, and durability settings. */
int async_logger_init_ex(async_logger_t *logger,
                         const async_logger_config_t *config);

/*
 * Submit one already-formatted message. Returns 0 on acceptance (BUFFERED) or
 * durable persistence (FSYNC_EACH), and -1 on invalid input/closed queue/I/O
 * failure. Text longer than LOG_TEXT_MAX - 1 is truncated.
 */
int async_logger_log(async_logger_t *logger,
                     log_level_t level,
                     const char *text);

/* Close the queue, drain accepted messages, sync the file, and join writer. */
int async_logger_shutdown(async_logger_t *logger);
/* Release resources. Call only after async_logger_shutdown() succeeds. */
int async_logger_destroy(async_logger_t *logger);

#endif
