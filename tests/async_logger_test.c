#include "async_logger.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 8
#define LOGS_PER_THREAD 2000

typedef struct {
    async_logger_t *logger;
    int worker_id;
    int failed;
} worker_arg_t;

static void *worker_main(void *opaque)
{
    worker_arg_t *worker = opaque;
    for (int i = 0; i < LOGS_PER_THREAD; ++i)
    {
        char text[128];
        snprintf(text, sizeof(text), "worker=%d message=%d", worker->worker_id, i);
        if (async_logger_log(worker->logger, LOG_LEVEL_INFO, text) != 0)
        {
            worker->failed = 1;
            break;
        }
    }
    return NULL;
}

int main(void)
{
    async_logger_t logger;
    pthread_t threads[THREAD_COUNT];
    worker_arg_t workers[THREAD_COUNT];
    const uint64_t expected = (uint64_t)THREAD_COUNT * LOGS_PER_THREAD;

    remove("async_logger_test.log");
    if (async_logger_init(&logger, "async_logger_test.log", 1024) != 0)
        return EXIT_FAILURE;

    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        workers[i] = (worker_arg_t){.logger = &logger, .worker_id = i, .failed = 0};
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0)
            return EXIT_FAILURE;
    }
    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        if (pthread_join(threads[i], NULL) != 0 || workers[i].failed)
            return EXIT_FAILURE;
    }

    if (async_logger_shutdown(&logger) != 0 ||
        logger.accepted != expected || logger.written != expected ||
        logger.rejected != 0 || logger.write_error != 0)
    {
        fprintf(stderr,
                "accepted=%" PRIu64 " written=%" PRIu64
                " rejected=%" PRIu64 " write_error=%d\n",
                logger.accepted, logger.written, logger.rejected, logger.write_error);
        return EXIT_FAILURE;
    }
    if (async_logger_log(&logger, LOG_LEVEL_INFO, "after shutdown") == 0 ||
        logger.rejected != 1)
        return EXIT_FAILURE;
    if (async_logger_destroy(&logger) != 0)
        return EXIT_FAILURE;
    remove("async_logger_test.log");
    return EXIT_SUCCESS;
}
