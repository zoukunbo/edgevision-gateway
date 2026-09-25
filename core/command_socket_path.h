#ifndef EDGEVISION_COMMAND_SOCKET_PATH_H
#define EDGEVISION_COMMAND_SOCKET_PATH_H

#include <stdlib.h>

#define EDGEVISION_COMMAND_SOCKET_ENV "EDGEVISION_COMMAND_SOCKET"
#define EDGEVISION_COMMAND_SOCKET_DEFAULT "/tmp/edgevision-study.sock"

static inline const char *edgevision_command_socket_path(void)
{
    const char *path = getenv(EDGEVISION_COMMAND_SOCKET_ENV);

    if (path == NULL || path[0] == '\0')
        return EDGEVISION_COMMAND_SOCKET_DEFAULT;

    return path;
}

#endif
