#define _POSIX_C_SOURCE 200809L
#include "async_logger.h"
#include "graceful_shutdown.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int wait_for_stop(void)
{
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000000L};
    while (!graceful_shutdown_requested())
    {
        struct timespec remaining = interval;
        while (nanosleep(&remaining, &remaining) != 0)
        {
            if (errno == EINTR)
            {
                if (graceful_shutdown_requested())
                    return 0;
                continue;
            }
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *log_path = argc > 1 ? argv[1] : "gateway.log";
    async_logger_t logger;
    int result = EXIT_FAILURE;

    if (graceful_shutdown_install() != 0)
    {
        perror("graceful_shutdown_install");
        return EXIT_FAILURE;
    }
    if (async_logger_init(&logger, log_path, 256) != 0)
    {
        fprintf(stderr, "async_logger_init failed for %s\n", log_path);
        return EXIT_FAILURE;
    }
    if (async_logger_log(&logger, LOG_LEVEL_INFO, "gateway started") != 0)
    {
        fprintf(stderr, "failed to submit startup log\n");
        goto SHUTDOWN;
    }

    printf("gateway running; send SIGINT or SIGTERM to stop\n");
    fflush(stdout);
    if (wait_for_stop() != 0)
    {
        fprintf(stderr, "wait_for_stop failed: %s\n", strerror(errno));
        goto SHUTDOWN;
    }
    if (async_logger_log(&logger, LOG_LEVEL_INFO,
                         "shutdown requested; draining logger") != 0)
    {
        fprintf(stderr, "failed to submit shutdown log\n");
        goto SHUTDOWN;
    }
    result = EXIT_SUCCESS;

SHUTDOWN:
    if (async_logger_shutdown(&logger) != 0)
    {
        fprintf(stderr, "async_logger_shutdown failed\n");
        result = EXIT_FAILURE;
    }
    if (async_logger_destroy(&logger) != 0)
    {
        fprintf(stderr, "async_logger_destroy failed\n");
        result = EXIT_FAILURE;
    }
    if (result == EXIT_SUCCESS)
        printf("gateway stopped cleanly\n");
    return result;
}
