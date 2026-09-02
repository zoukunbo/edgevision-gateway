/*
 * D40第一阶段：用可控离线发布替身投递最早的一条pending Outbox。
 * 只有替身确认后才更新为sent；未确认或进程中断时记录留待下次重试。
 * 本示例不访问UART/GPIO/Broker，不能作为真实网络或PUBACK证据。
 */
#include "measurement_json.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DELIVERY_EXERCISE_READY 1
#define DELIVERY_NOT_CONFIRMED 1001
#define DELIVERY_NO_PENDING 1002

typedef struct {
    /* 列值会在finalize前复制到此对象，调用方不持有SQLite内部指针。 */
    sqlite3_int64 outbox_id;
    char topic[160];
    char payload[4096];
} outbox_item_t;

static int exec_sql(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int initialize_schema(sqlite3 *db)
{
    /* UNIQUE measurement_id确保本练习的一条Measurement只生成一条Outbox记录。 */
    return exec_sql(db,
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS measurements("
        " id INTEGER PRIMARY KEY, payload_json TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS outbox("
        " id INTEGER PRIMARY KEY,"
        " measurement_id INTEGER NOT NULL UNIQUE REFERENCES measurements(id),"
        " topic TEXT NOT NULL CHECK(length(topic)>0),"
        " state TEXT NOT NULL DEFAULT 'pending'"
        "   CHECK(state IN ('pending','sent')));"
    );
}

/* 样板：用已学过的显式事务创建一条Measurement和pending Outbox。 */
static int seed_pending(sqlite3 *db, const char *json)
{
    /* BEGIN IMMEDIATE让两次INSERT同成同败；失败路径保留最初的SQLite错误码。 */
    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 measurement_id = 0;
    int rc = exec_sql(db, "BEGIN IMMEDIATE;");
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO measurements(payload_json) VALUES (?1);", -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 1, json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK && sqlite3_step(stmt) != SQLITE_DONE) rc = sqlite3_errcode(db);
    if (rc == SQLITE_OK) measurement_id = sqlite3_last_insert_rowid(db);
    {
        int finish_rc = sqlite3_finalize(stmt); stmt = NULL;
        if (rc == SQLITE_OK) rc = finish_rc;
    }
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO outbox(measurement_id,topic) VALUES (?1,?2);",
            -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, measurement_id);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt, 2,
            "edgevision/v1/devices/stm32-dht11-01/measurements",
            -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && sqlite3_step(stmt) != SQLITE_DONE) rc = sqlite3_errcode(db);
    {
        int finish_rc = sqlite3_finalize(stmt);
        if (rc == SQLITE_OK) rc = finish_rc;
    }
    if (rc == SQLITE_OK) {
        rc = exec_sql(db, "COMMIT;");
        if (rc == SQLITE_OK) return SQLITE_OK;
    }
    {
        int original_rc = rc;
        (void)exec_sql(db, "ROLLBACK;");
        return original_rc;
    }
}

/* 每次只取最早的一条pending；复制列值后立即finalize，不跨函数借用SQLite指针。 */
static int load_oldest_pending(sqlite3 *db, outbox_item_t *item)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT o.id,o.topic,m.payload_json FROM outbox o"
        " JOIN measurements m ON m.id=o.measurement_id"
        " WHERE o.state='pending' ORDER BY o.id LIMIT 1;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) rc = DELIVERY_NO_PENDING;
    if (rc == SQLITE_ROW) {
        const unsigned char *topic = sqlite3_column_text(stmt, 1);
        const unsigned char *payload = sqlite3_column_text(stmt, 2);
        if (topic == NULL || payload == NULL ||
            strlen((const char *)topic) >= sizeof(item->topic) ||
            strlen((const char *)payload) >= sizeof(item->payload)) {
            rc = SQLITE_TOOBIG;
        } else {
            item->outbox_id = sqlite3_column_int64(stmt, 0);
            strcpy(item->topic, (const char *)topic);
            strcpy(item->payload, (const char *)payload);
            rc = SQLITE_OK;
        }
    }
    {
        int finish_rc = sqlite3_finalize(stmt);
        if (rc == SQLITE_OK) rc = finish_rc;
    }
    return rc;
}

/* 离线替身只打印输入并返回预设结果；不访问Broker，不生成PUBACK。 */
static int publish_stub(const outbox_item_t *item, int confirmed)
{
    printf("PUBLISH_STUB id=%lld topic=%s payload=%s result=%s\n",
        (long long)item->outbox_id, item->topic, item->payload,
        confirmed ? "confirmed" : "not-confirmed");
    return confirmed;
}

/* UPDATE同时检查旧状态，避免把已经处理的记录再次算作本次成功。 */
static int mark_sent(sqlite3 *db, sqlite3_int64 outbox_id)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "UPDATE outbox SET state='sent' WHERE id=?1 AND state='pending';",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt, 1, outbox_id);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
    }
    if (rc == SQLITE_OK && sqlite3_changes(db) != 1) rc = SQLITE_NOTFOUND;
    {
        int finish_rc = sqlite3_finalize(stmt);
        if (rc == SQLITE_OK) rc = finish_rc;
    }
    return rc;
}

/* 本轮关键行为：只有发布替身确认成功，才允许pending -> sent。 */
static int deliver_one(sqlite3 *db, int confirmed)
{
    outbox_item_t item = {0};
    int rc = SQLITE_ERROR;

    /* 取出数据库中最旧的1条pendin待发记录 */
    rc = load_oldest_pending(db, &item);
    if (rc == DELIVERY_NO_PENDING)
    {
        printf("NO_PENDING \n");
        return DELIVERY_NO_PENDING;
    }

    if (rc != SQLITE_OK)
    {
        return rc;
    }
    /* 调用publish_stub离线模拟网络发送
        发送失败 数据库状态不变，依旧pending,下次重启还能充实发送
        发送成功，这条记录state从pending 到sent
    */
    int publish_result  = publish_stub(&item, confirmed);
    if (publish_result == 0)
    {
        printf("NOT_CONFIRMED \n");
        return DELIVERY_NOT_CONFIRMED;
    }

    rc = mark_sent(db, item.outbox_id);

    if (rc == SQLITE_OK)
    {
        printf("MARKED_SENT id=%lld\n", (long long)item.outbox_id);
    }


    return rc;
}

static int print_status(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT (SELECT count(*) FROM measurements),"
        " (SELECT count(*) FROM outbox WHERE state='pending'),"
        " (SELECT count(*) FROM outbox WHERE state='sent');",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        printf("measurement_count=%d pending_count=%d sent_count=%d\n",
            sqlite3_column_int(stmt,0), sqlite3_column_int(stmt,1),
            sqlite3_column_int(stmt,2));
        rc = SQLITE_OK;
    }
    {
        int finish_rc = sqlite3_finalize(stmt);
        if (rc == SQLITE_OK) rc = finish_rc;
    }
    return rc;
}

static int read_validated_json(char json[4096])
{
    size_t n=fread(json,1,4095,stdin);
    if (ferror(stdin) || n==0 || !feof(stdin)) return SQLITE_TOOBIG;
    json[n]='\0';
    measurement_t value;
    if (measurement_from_json(json,n,&value)!=MEASUREMENT_JSON_OK) return SQLITE_MISMATCH;
    while (n>0 && (json[n-1]=='\n' || json[n-1]=='\r')) json[--n]='\0';
    return SQLITE_OK;
}

int main(int argc, char **argv)
{
    int seed=argc==3 && strcmp(argv[1],"seed")==0;
    int fail=argc==3 && strcmp(argv[1],"deliver-fail")==0;
    int ok=argc==3 && strcmp(argv[1],"deliver-ok")==0;
    int status=argc==3 && strcmp(argv[1],"status")==0;
    if (!seed && !fail && !ok && !status) {
        fprintf(stderr,"Usage: %s seed|deliver-fail|deliver-ok|status DATABASE\n",argv[0]);
        return EXIT_FAILURE;
    }
    if (!DELIVERY_EXERCISE_READY) {
        fputs("EXERCISE INCOMPLETE: fill D1-D3, then set DELIVERY_EXERCISE_READY=1. No database opened.\n",stderr);
        return EXIT_FAILURE;
    }
    char json[4096]={0};
    int rc=seed ? read_validated_json(json) : SQLITE_OK;
    sqlite3 *db=NULL;
    if (rc==SQLITE_OK) rc=sqlite3_open_v2(argv[2],&db,
        status ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,NULL);
    if (rc==SQLITE_OK && !status) rc=initialize_schema(db);
    if (rc==SQLITE_OK && seed) rc=seed_pending(db,json);
    if (rc==SQLITE_OK && (fail || ok)) rc=deliver_one(db,ok);
    if (rc==SQLITE_OK && status) rc=print_status(db);
    if (rc!=SQLITE_OK && rc!=DELIVERY_NOT_CONFIRMED && rc!=DELIVERY_NO_PENDING)
        fprintf(stderr,"operation failed: rc=%d %s\n",rc,sqlite3_errstr(rc));
    int close_rc=sqlite3_close(db);
    if (seed && rc==SQLITE_OK) puts("SEEDED one pending Outbox item.");
    if (rc!=SQLITE_OK || close_rc!=SQLITE_OK) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
