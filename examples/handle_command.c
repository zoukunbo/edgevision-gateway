#include <stdio.h>
#include <string.h>

typedef struct
{
    int interval_ms;
    int collected_count;
} gateway_status_t;

/**
 * command：调用者想做什么；strcmp(...) == 0 表示字符串内容相同。
 * interval_ms：本次查询使用的当前状态，不能在函数里写死成 1000。
 * reply、reply_size：回答写在哪里、最多能写多少字节。数组由调用者持有，函数只是借用。
 */
int handle_command(const char *command, const gateway_status_t *status,
                   char *reply, size_t reply_size)
{
    int n;

    if (strcmp(command, "status") == 0) 
    {
        n = snprintf(reply, reply_size,
                     "interval_ms=%d collected_count=%d",
                      status->interval_ms,
                      status->collected_count);
    } 
    else 
    {
        n = snprintf(reply, reply_size, "error=unknown_command");
    }

    if (n < 0 || (size_t)n >= reply_size)
        return -1;

    return 0;
}

int main(void)
{
    gateway_status_t status = {
        .interval_ms = 1000,
        .collected_count = 12
    };
    char reply[128];

    if (handle_command("status", &status,
                       reply, sizeof(reply)) != 0) {
        fprintf(stderr, "生成回答失败\n");
        return 1;
    }

    puts(reply);
    status.collected_count++;

    if (handle_command("status", &status,
                       reply, sizeof(reply)) != 0) {
        fprintf(stderr, "生成回答失败\n");
        return 1;
    }

    puts(reply);
    return 0;
}