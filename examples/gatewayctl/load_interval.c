#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <limits.h>

/* 在这里放入已有的 parse_interval_ms()。 */
static int parse_interval_ms(const char *text, int *value)
{
    if (text == NULL || value == NULL || text[0] == '\0')
        return 1;

    /* 第一版只接受数字，不接受正负号、空格或其他字符。 */
    for (const char *p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return 1;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);

    if (errno == ERANGE || *end != '\0' || parsed > INT_MAX)
        return 1;

    *value = (int)parsed;
    return 0;
}



/* 你实现：成功返回 0；格式或范围非法返回 -1。
 * 失败时不得修改 *out_ms。
 */
static int parse_interval_line(const char *line, int *out_ms)
{
    if (line == NULL || line[0] == '\0' || out_ms == NULL)
    {
        return -1;
    }

    if (strncmp(line, "interval_ms=", 12) != 0)
    {
        return -1;
    }
    int value;
    int rc = parse_interval_ms(line+12, &value);
    if (rc != 0)
    {
        return -1;
    }
    
    if (value < 100 || value > 60000)
    {
        return -1;
    }
    
    *out_ms = value;
    
    return 0;    
}

/* 返回 0：读取成功；1：文件不存在；-1：其他错误。
 * 返回非 0 时，保留调用者原来的值。
 * 第一版只接受一行配置，可有一个末尾换行，不接受额外行。
 */
static int load_interval_file(const char *path, int *out_ms)
{
    if (path == NULL || out_ms == NULL)
        return -1;

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return errno == ENOENT ? 1 : -1;

    char line[64];
    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return -1;
    }

    /* 检查是否还有未读内容，同时拒绝过长行和额外行。 */
    int next = fgetc(fp);
    int read_failed = ferror(fp);
    int close_result = fclose(fp);

    if (next != EOF || read_failed || close_result != 0)
        return -1;

    /* 去掉行尾的 LF，兼容 Windows 的 CRLF。 */
    size_t length = strlen(line);
    if (length > 0 && line[length - 1] == '\n')
        line[--length] = '\0';
    if (length > 0 && line[length - 1] == '\r')
        line[--length] = '\0';

    return parse_interval_line(line, out_ms);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "用法：%s <配置文件路径>\n", argv[0]);
        return 1;
    }

    int interval_ms = 1000;
    int rc = load_interval_file(argv[1], &interval_ms);

    /* 你完成：根据 rc 决定报错退出，还是继续使用配置。 */
    if (rc == -1)
    {
        fprintf(stderr, "配置读取失败 \n");
        return 1;
    }
    else if (rc == 1)
    {
        puts("配置文件不存在，使用默认值");
    }
    

    printf("准备启动：interval_ms=%d\n", interval_ms);
    return 0;
}