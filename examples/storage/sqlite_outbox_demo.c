/*
 * D39第二阶段：在同一SQLite事务中保存Measurement与pending Outbox。
 * save-fail用约束错误验证两表一起回滚；它不是生产故障注入框架。
 * 本示例只回放历史JSON，不访问UART/GPIO，也不连接MQTT。
 */
#include "measurement_json.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 完成T1-T3后改成1；未完成时不打开/创建数据库。 */
#define TRANSACTION_EXERCISE_READY 1

static int exec_sql(sqlite3 *db, const char *sql)
{
    /* 仅执行源码内固定的DDL/事务语句；业务数据始终通过绑定参数写入。 */
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int initialize_schema(sqlite3 *db)
{
    return exec_sql(db,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS measurements("
        " id INTEGER PRIMARY KEY,"
        " payload_json TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS outbox("
        " id INTEGER PRIMARY KEY,"
        " measurement_id INTEGER NOT NULL UNIQUE"
        "   REFERENCES measurements(id),"
        " topic TEXT NOT NULL CHECK(length(topic)>0),"
        " state TEXT NOT NULL DEFAULT 'pending' CHECK(state='pending')"
        ");");
}

/* 复用上一段的prepare/bind/step/finalize行为，并返回本地数据库行号。
 * measurement_id只用于本库关联，不等于设备sequence或全局事件ID。
 */
static int insert_measurement(
    sqlite3 *db, const char *json, sqlite3_int64 *measurement_id)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO measurements(payload_json) VALUES (?1);",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            *measurement_id = sqlite3_last_insert_rowid(db);
            rc = SQLITE_OK;
        }
    }
    int finish_rc = sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? finish_rc : rc;
}

/* fail_outbox用于可控地触发NOT NULL约束失败，验证前一条INSERT会回滚。
 * 正常topic固定为本次真实设备的现有主题；本轮不实际发布。
 */
static int insert_outbox(
    sqlite3 *db, sqlite3_int64 measurement_id, int fail_outbox)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO outbox(measurement_id, topic) VALUES (?1, ?2);",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, measurement_id);
    if (rc == SQLITE_OK) {
        if (fail_outbox)
            rc = sqlite3_bind_null(stmt, 2);
        else
            rc = sqlite3_bind_text(stmt, 2,
                "edgevision/v1/devices/stm32-dht11-01/measurements",
                -1, SQLITE_STATIC);
    }
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    int finish_rc = sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? finish_rc : rc;
}

/* 输入：一条已通过Measurement校验的JSON。
 * 输出：同一事务中的measurements一行和pending outbox一行。
 * 任一步失败都显式ROLLBACK；成功必须以COMMIT完成才返回SQLITE_OK。
 */
static int save_measurement_and_outbox(
    sqlite3 *db, const char *json, int fail_outbox)
{
    int rc = SQLITE_ERROR;
    int transaction_started = 0;
    sqlite3_int64 measurement_id = 0;

    /* TODO T1：用exec_sql执行BEGIN IMMEDIATE。
     * BEGIN成功才把transaction_started设为1；失败直接返回原错误码。
     */
    rc = exec_sql(db, "BEGIN IMMEDIATE;");
    if (rc != SQLITE_OK) {
        return rc;
    }
    transaction_started = 1;
    /* TODO T2：依次调用insert_measurement和insert_outbox。
     * 任一步不是SQLITE_OK都goto rollback。
     * 两步都成功后执行COMMIT；COMMIT成功时先清除transaction_started再返回SQLITE_OK。
     * COMMIT失败时事务可能仍处于活动状态，也应goto rollback。
     */
    rc = insert_measurement(db, json, &measurement_id);

    if (rc != SQLITE_OK)
    {
        goto rollback;
    }

    rc = insert_outbox(db, measurement_id, fail_outbox);

    if (rc != SQLITE_OK)
    {
        goto rollback;
    }

    rc = exec_sql(db, "COMMIT;");
    if (rc == SQLITE_OK) {
        transaction_started = 0;
        return SQLITE_OK;
    }

rollback:
    /* TODO T3：如果transaction_started为真，显式执行ROLLBACK。
     * 保留最先失败的rc，不允许ROLLBACK成功把业务失败改成SQLITE_OK。
     * 最后返回rc。SQLite文档不保证所有错误都会自动回滚整个事务。
     */
    if (transaction_started)
    {
        (void)exec_sql(db, "ROLLBACK;");
    }
    return rc;
}

static int print_status(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT (SELECT count(*) FROM measurements),"
        "       (SELECT count(*) FROM outbox WHERE state='pending');",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        printf("measurement_count=%d pending_count=%d\n",
            sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1));
        rc = SQLITE_OK;
    }
    int finish_rc = sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? finish_rc : rc;
}

static int read_validated_json(char json[4096])
{
    /* 整条JSON必须能装入固定缓冲区；成功后保留内容，仅裁掉文件行尾。 */
    size_t n = fread(json, 1, 4095, stdin);
    if (ferror(stdin) || n == 0 || !feof(stdin)) return SQLITE_TOOBIG;
    json[n] = '\0';
    measurement_t value;
    if (measurement_from_json(json, n, &value) != MEASUREMENT_JSON_OK)
        return SQLITE_MISMATCH;
    while (n > 0 && (json[n-1] == '\n' || json[n-1] == '\r'))
        json[--n] = '\0';
    return SQLITE_OK;
}

int main(int argc, char **argv)
{
    int save = argc == 3 && strcmp(argv[1], "save") == 0;
    int save_fail = argc == 3 && strcmp(argv[1], "save-fail") == 0;
    int status = argc == 3 && strcmp(argv[1], "status") == 0;
    if (!save && !save_fail && !status) {
        fprintf(stderr, "Usage: %s save|save-fail|status DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!TRANSACTION_EXERCISE_READY) {
        fputs("EXERCISE INCOMPLETE: fill T1-T3, then set TRANSACTION_EXERCISE_READY=1. No database opened.\n", stderr);
        return EXIT_FAILURE;
    }
    if (strcmp(argv[2], ":memory:") == 0) {
        fputs("Use a file path: pending must survive process restart.\n", stderr);
        return EXIT_FAILURE;
    }

    char json[4096] = {0};
    int rc = SQLITE_OK;
    if (save || save_fail) {
        rc = read_validated_json(json);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Invalid Measurement input: rc=%d\n", rc);
            return EXIT_FAILURE;
        }
    }

    sqlite3 *db = NULL;
    int flags = status ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    rc = sqlite3_open_v2(argv[2], &db, flags, NULL);
    if (rc == SQLITE_OK && !status) rc = initialize_schema(db);
    if (rc == SQLITE_OK && (save || save_fail))
        rc = save_measurement_and_outbox(db, json, save_fail);
    if (rc == SQLITE_OK && status) rc = print_status(db);
    if (rc != SQLITE_OK)
        /* 显式ROLLBACK会刷新连接的最新errmsg；按保留的原始rc输出稳定说明。 */
        fprintf(stderr, "SQLite operation failed: rc=%d %s\n",
            rc, sqlite3_errstr(rc));
    int close_rc = sqlite3_close(db);
    if (rc != SQLITE_OK || close_rc != SQLITE_OK) return EXIT_FAILURE;
    if (save) puts("SAVED measurement + pending outbox in one transaction.");
    if (save_fail) {
        fputs("save-fail unexpectedly succeeded.\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
