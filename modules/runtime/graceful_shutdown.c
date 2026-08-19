#define _POSIX_C_SOURCE 200809L
#include "graceful_shutdown.h"
#include <signal.h>
#include <stddef.h>

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

int graceful_shutdown_install(void)
{
    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    if (sigemptyset(&action.sa_mask) != 0)
        return -1;
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) != 0)
        return -1;
    if (sigaction(SIGTERM, &action, NULL) != 0)
        return -1;
    return 0;
}

int graceful_shutdown_requested(void)
{
    return stop_requested != 0;
}
