#define _POSIX_C_SOURCE 200809L

/*
 * 消费端幂等示例：
 *
 *   stdin 中每一行代表一次 MQTT 消息投递。程序用 message_id 作为 inbox
 *   主键，并在同一个 SQLite 事务中完成“登记消息”和“写业务表”。因此：
 *
 *   1. 同一消息重复到达时，只有第一次会产生业务记录；
 *   2. 两次写入中途失败时，ROLLBACK 会同时撤销二者，不留下半成功状态。
 *
 * 该示例不连接 Broker，目的是把消费端最关键的事务边界单独展示清楚。
 */
#include "cJSON.h"
#include "measurement_json.h"

#include <sqlite3.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_ID_LENGTH 32u
#define MESSAGE_ID_CAPACITY (MESSAGE_ID_LENGTH + 1u)
#define MAX_PAYLOAD_BYTES (1024u * 1024u)

typedef struct
{
    char message_id[MESSAGE_ID_CAPACITY];
    measurement_t measurement;
} consumer_message_t;

typedef enum
{
    CONSUMER_ERROR = -1,
    CONSUMER_DUPLICATE = 0,
    CONSUMER_PROCESSED = 1
} consumer_result_t;

static int execute_sql(sqlite3 *db, const char *sql)
{
    char *error_text = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error_text);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr,
                "sqlite operation failed: %s\n",
                error_text != NULL ? error_text : sqlite3_errstr(rc));
    }
    sqlite3_free(error_text);
    return rc;
}

static int initialize_schema(sqlite3 *db)
{
    /*
     * consumer_inbox 是“处理过哪些消息”的持久化事实；业务表通过外键引用它。
     * 两张表都以 message_id 为主键，数据库约束是最后一道重复写入防线。
     */
    static const char sql[] =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA journal_mode = WAL;"
        "CREATE TABLE IF NOT EXISTS consumer_inbox ("
        "message_id TEXT PRIMARY KEY CHECK(length(message_id) = 32),"
        "payload_json TEXT NOT NULL CHECK(length(payload_json) > 0),"
        "received_at_ms INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS business_measurements ("
        "message_id TEXT PRIMARY KEY REFERENCES consumer_inbox(message_id),"
        "device_id TEXT NOT NULL,"
        "metric TEXT NOT NULL,"
        "sequence INTEGER NOT NULL,"
        "timestamp_ms INTEGER NOT NULL,"
        "value REAL NOT NULL"
        ");";

    return execute_sql(db, sql);
}

static int message_id_is_valid(const char *message_id)
{
    if (message_id == NULL || strlen(message_id) != MESSAGE_ID_LENGTH)
        return 0;

    /* 协议限定为小写十六进制；拒绝大写可避免同一字节串出现两种文本形式。 */
    for (size_t index = 0; index < MESSAGE_ID_LENGTH; ++index)
    {
        unsigned char ch = (unsigned char)message_id[index];
        if (!isdigit(ch) && (ch < (unsigned char)'a' || ch > (unsigned char)'f'))
            return 0;
    }
    return 1;
}

static int parse_consumer_message(const char *payload,
                                  size_t payload_size,
                                  consumer_message_t *output)
{
    if (payload == NULL || payload_size == 0 || output == NULL)
        return -1;

    memset(output, 0, sizeof(*output));

    /* 复用正式领域解析器，避免示例另写一套 measurement 校验规则。 */
    if (measurement_from_json(payload, payload_size, &output->measurement) !=
        MEASUREMENT_JSON_OK)
    {
        fputs("invalid measurement payload\n", stderr);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_size);
    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        fputs("payload root must be a JSON object\n", stderr);
        return -1;
    }

    const cJSON *message_id =
        cJSON_GetObjectItemCaseSensitive(root, "message_id");
    if (!cJSON_IsString(message_id) ||
        !message_id_is_valid(message_id->valuestring))
    {
        cJSON_Delete(root);
        fputs("message_id must be 32 lowercase hexadecimal characters\n",
              stderr);
        return -1;
    }

    memcpy(output->message_id,
           message_id->valuestring,
           MESSAGE_ID_LENGTH + 1u);
    cJSON_Delete(root);
    return 0;
}

static int insert_inbox(sqlite3 *db,
                        const consumer_message_t *message,
                        const char *payload,
                        int *out_inserted)
{
    static const char sql[] =
        "INSERT INTO consumer_inbox(message_id, payload_json, received_at_ms) "
        "VALUES(?1, ?2, CAST(strftime('%s','now') AS INTEGER) * 1000) "
        "ON CONFLICT(message_id) DO NOTHING;";
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);

    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(
            statement, 1, message->message_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(statement, 2, payload, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK && sqlite3_step(statement) != SQLITE_DONE)
        rc = sqlite3_errcode(db);

    if (rc == SQLITE_OK)
    {
        /* changes==1 表示首次登记；changes==0 表示唯一键识别出重复消息。 */
        *out_inserted = sqlite3_changes(db) == 1;
    }

    int finalize_rc = sqlite3_finalize(statement);
    return rc == SQLITE_OK ? finalize_rc : rc;
}

static int insert_business_measurement(sqlite3 *db,
                                       const consumer_message_t *message)
{
    static const char sql[] =
        "INSERT INTO business_measurements("
        "message_id, device_id, metric, sequence, timestamp_ms, value"
        ") VALUES(?1, ?2, ?3, ?4, ?5, ?6);";
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);

    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(
            statement, 1, message->message_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(statement,
                               2,
                               message->measurement.device_id,
                               -1,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(statement,
                               3,
                               message->measurement.metric,
                               -1,
                               SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(
            statement, 4, (sqlite3_int64)message->measurement.sequence);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(
            statement, 5, (sqlite3_int64)message->measurement.timestamp_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_double(statement, 6, message->measurement.value);
    if (rc == SQLITE_OK && sqlite3_step(statement) != SQLITE_DONE)
        rc = sqlite3_errcode(db);

    int finalize_rc = sqlite3_finalize(statement);
    return rc == SQLITE_OK ? finalize_rc : rc;
}

static int duplicate_payload_matches(sqlite3 *db,
                                     const consumer_message_t *message,
                                     const char *payload)
{
    static const char sql[] =
        "SELECT payload_json FROM consumer_inbox WHERE message_id = ?1;";
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);

    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(
            statement, 1, message->message_id, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(statement);

    if (rc == SQLITE_ROW)
    {
        const unsigned char *stored = sqlite3_column_text(statement, 0);
        if (stored == NULL || strcmp((const char *)stored, payload) != 0)
        {
            /*
             * 同一个 ID 指向不同正文不是正常重投。把它升级为错误，避免
             * ON CONFLICT DO NOTHING 静默掩盖生产端 ID 复用或数据损坏。
             */
            fputs("message_id conflict: stored payload differs\n", stderr);
            rc = SQLITE_CONSTRAINT;
        }
        else
        {
            rc = SQLITE_OK;
        }
    }
    else if (rc == SQLITE_DONE)
    {
        rc = SQLITE_NOTFOUND;
    }

    int finalize_rc = sqlite3_finalize(statement);
    return rc == SQLITE_OK ? finalize_rc : rc;
}

static consumer_result_t process_payload(sqlite3 *db,
                                         const char *payload,
                                         size_t payload_size)
{
    consumer_message_t message;
    if (parse_consumer_message(payload, payload_size, &message) != 0)
        return CONSUMER_ERROR;

    int rc = execute_sql(db, "BEGIN IMMEDIATE;");
    if (rc != SQLITE_OK)
        return CONSUMER_ERROR;

    int inserted = 0;
    rc = insert_inbox(db, &message, payload, &inserted);

    if (rc == SQLITE_OK && !inserted)
        rc = duplicate_payload_matches(db, &message, payload);

    /*
     * 只有本事务刚插入 inbox 时才执行业务写入。重复消息直接提交空事务，
     * 因而不会再次触发计费、告警、控制指令等不可重复的业务副作用。
     */
    if (rc == SQLITE_OK && inserted)
        rc = insert_business_measurement(db, &message);

    if (rc == SQLITE_OK)
        rc = execute_sql(db, "COMMIT;");

    if (rc != SQLITE_OK)
    {
        /* 回滚同时撤销 inbox 与业务写入，下一次重投仍有机会完整处理。 */
        (void)execute_sql(db, "ROLLBACK;");
        return CONSUMER_ERROR;
    }

    printf("%s message_id=%s\n",
           inserted ? "processed" : "duplicate",
           message.message_id);
    return inserted ? CONSUMER_PROCESSED : CONSUMER_DUPLICATE;
}

static int read_counts(sqlite3 *db, int *inbox_count, int *business_count)
{
    static const char sql[] =
        "SELECT (SELECT COUNT(*) FROM consumer_inbox),"
        "       (SELECT COUNT(*) FROM business_measurements);";
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);

    if (rc == SQLITE_OK)
        rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW)
    {
        *inbox_count = sqlite3_column_int(statement, 0);
        *business_count = sqlite3_column_int(statement, 1);
        rc = SQLITE_OK;
    }

    int finalize_rc = sqlite3_finalize(statement);
    return rc == SQLITE_OK ? finalize_rc : rc;
}

static int run_self_test(void)
{
    static const char payload[] =
        "{\"device_id\":\"self-test-device\","
        "\"metric\":\"temperature\",\"unit\":\"celsius\","
        "\"schema_version\":1,\"sequence\":1,"
        "\"timestamp_ms\":1788754000000,\"value\":21.5,"
        "\"quality\":\"good\","
        "\"message_id\":\"0123456789abcdef0123456789abcdef\"}";
    static const char conflicting_payload[] =
        "{\"device_id\":\"self-test-device\","
        "\"metric\":\"temperature\",\"unit\":\"celsius\","
        "\"schema_version\":1,\"sequence\":2,"
        "\"timestamp_ms\":1788754000001,\"value\":22.0,"
        "\"quality\":\"good\","
        "\"message_id\":\"0123456789abcdef0123456789abcdef\"}";
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);

    if (rc == SQLITE_OK)
        rc = initialize_schema(db);
    if (rc == SQLITE_OK &&
        process_payload(db, payload, sizeof(payload) - 1u) !=
            CONSUMER_PROCESSED)
        rc = SQLITE_ERROR;
    if (rc == SQLITE_OK &&
        process_payload(db, payload, sizeof(payload) - 1u) !=
            CONSUMER_DUPLICATE)
        rc = SQLITE_ERROR;

    /* 同 ID 不同正文必须被拒绝，且不能改变已经提交的两张表。 */
    if (rc == SQLITE_OK &&
        process_payload(db, conflicting_payload, sizeof(conflicting_payload) - 1u) !=
            CONSUMER_ERROR)
        rc = SQLITE_ERROR;

    int inbox_count = 0;
    int business_count = 0;
    if (rc == SQLITE_OK)
        rc = read_counts(db, &inbox_count, &business_count);
    if (rc == SQLITE_OK && (inbox_count != 1 || business_count != 1))
        rc = SQLITE_ERROR;

    int close_rc = sqlite3_close(db);
    if (rc == SQLITE_OK)
        rc = close_rc;
    if (rc != SQLITE_OK)
    {
        fputs("self-test failed\n", stderr);
        return EXIT_FAILURE;
    }

    puts("self-test passed: deliveries=2 inbox=1 business=1");
    return EXIT_SUCCESS;
}

static int consume_standard_input(sqlite3 *db)
{
    char *line = NULL;
    size_t capacity = 0;
    unsigned long line_number = 0;
    int exit_code = EXIT_SUCCESS;

    for (;;)
    {
        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0)
            break;
        ++line_number;

        if ((size_t)length > MAX_PAYLOAD_BYTES)
        {
            fprintf(stderr, "line %lu exceeds payload limit\n", line_number);
            exit_code = EXIT_FAILURE;
            break;
        }

        if (process_payload(db, line, (size_t)length) == CONSUMER_ERROR)
        {
            fprintf(stderr, "line %lu was not processed\n", line_number);
            exit_code = EXIT_FAILURE;
            break;
        }
    }

    if (ferror(stdin))
    {
        fputs("failed to read standard input\n", stderr);
        exit_code = EXIT_FAILURE;
    }
    free(line);
    return exit_code;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return run_self_test();

    if (argc != 2)
    {
        fprintf(stderr,
                "Usage: %s DATABASE < newline-delimited-messages.jsonl\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    sqlite3 *db = NULL;
    int rc = sqlite3_open(argv[1], &db);
    if (rc == SQLITE_OK)
        rc = initialize_schema(db);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "failed to open consumer database: %s\n",
                db != NULL ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        sqlite3_close(db);
        return EXIT_FAILURE;
    }

    int exit_code = consume_standard_input(db);
    if (sqlite3_close(db) != SQLITE_OK)
    {
        fputs("failed to close consumer database\n", stderr);
        exit_code = EXIT_FAILURE;
    }
    return exit_code;
}
