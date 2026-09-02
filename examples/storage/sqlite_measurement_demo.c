/*
 * D39第一阶段：把一条通过领域校验的Measurement JSON持久化到SQLite。
 * put与read刻意由两个进程执行，以验证进程退出后的文件持久性；这不是断电测试。
 * 本示例仅做历史数据离线回放，不访问UART/GPIO/MQTT，也不创建Outbox。
 */
#include "measurement_json.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 完成三个TODO后改成1；未完成时不打开/创建数据库。 */
#define INSERT_EXERCISE_READY 1

/* 输入：已校验且以NUL结尾的JSON；db由当前单线程独占，无外层事务。
 * 成功返回SQLITE_OK；失败返回SQLite错误码；所有路径释放stmt。
 * 本轮单条INSERT使用SQLite隐式事务，下一段再练两表显式事务。
 */
static int insert_json(sqlite3 *db, const char *json)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO measurements(payload_json) VALUES (?1);";
    int rc = SQLITE_ERROR;

    /* 将SQL语句编译成字节码，得到一个sqlite3_stmt *句柄 */
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto cleanup;
    /* sqlite3_bind_xxx 把应用程序数据绑定到sql,语句中的占位符 （？或 NNN）上*/
    rc = sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) goto cleanup;
    /**  sqlite3_step 执行准备好的语句
    *对于INSERT/UPDATE/DELETET通常一步就完成返回SQLITE_DONE;
    * 对于SELECT每次返回SQLITE_ROW
    */
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        rc = SQLITE_OK;
    }
    // 如果 rc 是 SQLITE_ROW 或其他错误码，则保持不变


    goto cleanup;
cleanup:
    {
        /* 销毁语句句柄，释放资源 */
        int finish_rc = sqlite3_finalize(stmt);
        if (rc == SQLITE_OK && finish_rc != SQLITE_OK) rc = finish_rc;
    }
    return rc;
}

static int read_rows(sqlite3 *db)
{
    /* column_text返回的指针只在当前行/stmt有效，因此在下一次step前立即输出。 */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT payload_json FROM measurements ORDER BY id;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text == NULL) { rc = SQLITE_ERROR; break; }
        if (puts((const char *)text) == EOF) { rc = SQLITE_IOERR; break; }
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int finish_rc = sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? finish_rc : rc;
}

int main(int argc, char **argv)
{
    /* read只读打开已有库；put才允许创建文件和表，避免查询拼错路径时生成空库。 */
    if (argc != 3 || (strcmp(argv[1], "put") != 0 && strcmp(argv[1], "read") != 0)) {
        fprintf(stderr, "Usage: %s put|read DATABASE\nput reads one Measurement JSON from stdin.\n", argv[0]);
        return EXIT_FAILURE;
    }
    int writing = strcmp(argv[1], "put") == 0;
    char json[4096];
    if (writing) {
        if (!INSERT_EXERCISE_READY) {
            fputs("EXERCISE INCOMPLETE: fill S1-S3, then set INSERT_EXERCISE_READY=1. No database opened.\n", stderr);
            return EXIT_FAILURE;
        }
        size_t n = fread(json, 1, sizeof(json)-1, stdin);
        if (ferror(stdin) || n == 0 || !feof(stdin)) {
            fputs("Input empty, too large or unreadable.\n", stderr);
            return EXIT_FAILURE;
        }
        json[n] = '\0';
        measurement_t value;
        if (measurement_from_json(json, n, &value) != MEASUREMENT_JSON_OK) {
            fputs("Invalid Measurement: no database opened.\n", stderr);
            return EXIT_FAILURE;
        }
        /* 保留原JSON字段/时间/来源；只去掉文件末尾换行，便于逐字节比较。 */
        while (n > 0 && (json[n-1] == '\n' || json[n-1] == '\r')) json[--n] = '\0';
    }
    if (strcmp(argv[2], ":memory:") == 0) {
        fputs("Use a file path: this exercise checks persistence across processes.\n", stderr);
        return EXIT_FAILURE;
    }
    sqlite3 *db = NULL;
    int flags = writing ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
    int rc = sqlite3_open_v2(argv[2], &db, flags, NULL);
    if (rc == SQLITE_OK && writing) {
        rc = sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS measurements("
            "id INTEGER PRIMARY KEY, payload_json TEXT NOT NULL);", NULL, NULL, NULL);
        if (rc == SQLITE_OK) rc = insert_json(db, json);
    } else if (rc == SQLITE_OK) {
        rc = read_rows(db);
    }
    if (rc != SQLITE_OK)
        fprintf(stderr, "SQLite operation failed: rc=%d %s\n", rc,
                db != NULL ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int close_rc = sqlite3_close(db);
    if (rc != SQLITE_OK || close_rc != SQLITE_OK) return EXIT_FAILURE;
    if (writing) puts("SAVED one Measurement; database closed.");
    return EXIT_SUCCESS;
}
