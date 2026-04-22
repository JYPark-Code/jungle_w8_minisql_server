/* test_engine_concurrent.c -- W8 engine wrapper smoke + concurrency tests. */

#define _POSIX_C_SOURCE 200809L

#include "engine.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <unistd.h>
#define TEST_RMDIR(path) rmdir(path)
#endif

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do {                                             \
    if (cond) {                                                           \
        ++g_passed;                                                       \
        printf("  [PASS] %s\n", msg);                                     \
    } else {                                                              \
        ++g_failed;                                                       \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__);                 \
    }                                                                     \
} while (0)

typedef struct {
    const char *sql;
    int loops;
    int failures;
} select_worker_arg_t;

typedef struct {
    int thread_id;
    int loops;
    int failures;
} insert_worker_arg_t;

static void remove_file_if_exists(const char *path);
static void cleanup_engine_fixtures(void);
static int json_has(const engine_result_t *r, const char *needle);
static void test_exec_sql_basic_json(void);
static void test_single_mode_executes(void);
static void test_explain_index_plan(void);
static void test_concurrent_selects(void);
static void test_concurrent_inserts(void);
static void *select_worker(void *arg);
static void *insert_worker(void *arg);

int main(void)
{
    printf("=== test_engine_concurrent ===\n");

    test_exec_sql_basic_json();
    test_single_mode_executes();
    test_explain_index_plan();
    test_concurrent_selects();
    test_concurrent_inserts();

    cleanup_engine_fixtures();

    printf("\n[ENGINE TESTS] %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

static void remove_file_if_exists(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "failed to remove file: %s\n", path);
    }
}

static void cleanup_engine_fixtures(void)
{
    const char *tables[] = {
        "engine_users",
        "single_users",
        "select_users",
        "insert_users",
    };
    size_t i;

    for (i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "data/tables/%s.csv", tables[i]);
        remove_file_if_exists(path);
        snprintf(path, sizeof(path), "data/tables/%s.csv.tmp", tables[i]);
        remove_file_if_exists(path);
        snprintf(path, sizeof(path), "data/tables/%s.bin", tables[i]);
        remove_file_if_exists(path);
        snprintf(path, sizeof(path), "data/schema/%s.schema", tables[i]);
        remove_file_if_exists(path);
    }

    (void)TEST_RMDIR("data/tables");
    (void)TEST_RMDIR("data/schema");
    (void)TEST_RMDIR("data");
}

static int json_has(const engine_result_t *r, const char *needle)
{
    return r != NULL && r->json != NULL && strstr(r->json, needle) != NULL;
}

static void test_exec_sql_basic_json(void)
{
    engine_result_t r;

    printf("[TEST] engine_exec_sql basic CREATE/INSERT/SELECT JSON\n");
    cleanup_engine_fixtures();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_exec_sql(
        "CREATE TABLE engine_users (id INT, name VARCHAR, age INT);"
        "INSERT INTO engine_users (name, age) VALUES ('alice', 25);"
        "SELECT * FROM engine_users WHERE id = 1;",
        false);

    CHECK(r.ok, "multi-statement execution succeeds");
    CHECK(json_has(&r, "\"statement_count\":3"), "statement_count is 3");
    CHECK(json_has(&r, "alice"), "SELECT JSON contains inserted value");
    CHECK(json_has(&r, "\"row_count\":1"), "SELECT JSON contains one row");
    CHECK(r.index_used, "id lookup uses B+Tree path");

    engine_result_free(&r);
    engine_shutdown();
}

static void test_single_mode_executes(void)
{
    engine_result_t r;

    printf("[TEST] mode=single executes through global serialization path\n");
    cleanup_engine_fixtures();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_exec_sql(
        "CREATE TABLE single_users (id INT, name VARCHAR);"
        "INSERT INTO single_users (name) VALUES ('bob');"
        "SELECT * FROM single_users WHERE id = 1;",
        true);

    CHECK(r.ok, "single mode execution succeeds");
    CHECK(json_has(&r, "bob"), "single mode JSON contains inserted value");
    CHECK(r.lock_wait_ms >= 0.0, "single mode reports lock wait");

    engine_result_free(&r);
    engine_shutdown();
}

static void test_explain_index_plan(void)
{
    engine_result_t r;

    printf("[TEST] engine_explain reports index plan\n");
    cleanup_engine_fixtures();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_explain("SELECT * FROM engine_users WHERE id = 1;");

    CHECK(r.ok, "explain succeeds");
    CHECK(r.index_used, "explain marks id lookup as index_used");
    CHECK(json_has(&r, "BPTREE_POINT_LOOKUP"), "explain JSON names point lookup plan");

    engine_result_free(&r);
    engine_shutdown();
}

static void test_concurrent_selects(void)
{
    enum { THREADS = 8, LOOPS = 50 };
    pthread_t threads[THREADS];
    select_worker_arg_t args[THREADS];
    engine_result_t r;
    int i;
    int failures = 0;

    printf("[TEST] concurrent SELECT calls through engine_exec_sql\n");
    cleanup_engine_fixtures();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_exec_sql(
        "CREATE TABLE select_users (id INT, name VARCHAR);"
        "INSERT INTO select_users (id, name) VALUES (1, 'warm');"
        "SELECT * FROM select_users WHERE id = 1;",
        false);
    CHECK(r.ok, "fixture setup succeeds");
    engine_result_free(&r);

    for (i = 0; i < THREADS; i++) {
        args[i].sql = "SELECT * FROM select_users WHERE id = 1;";
        args[i].loops = LOOPS;
        args[i].failures = 0;
        CHECK(pthread_create(&threads[i], NULL, select_worker, &args[i]) == 0,
              "SELECT worker created");
    }

    for (i = 0; i < THREADS; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0, "SELECT worker joined");
        failures += args[i].failures;
    }

    CHECK(failures == 0, "all concurrent SELECT calls returned expected JSON");
    engine_shutdown();
}

static void test_concurrent_inserts(void)
{
    enum { THREADS = 4, LOOPS = 20, EXPECTED = THREADS * LOOPS };
    pthread_t threads[THREADS];
    insert_worker_arg_t args[THREADS];
    engine_result_t r;
    char expected_row_count[64];
    int i;
    int failures = 0;

    printf("[TEST] concurrent INSERT calls through engine_exec_sql\n");
    cleanup_engine_fixtures();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_exec_sql("CREATE TABLE insert_users (id INT, name VARCHAR);", false);
    CHECK(r.ok, "fixture table created");
    engine_result_free(&r);

    for (i = 0; i < THREADS; i++) {
        args[i].thread_id = i;
        args[i].loops = LOOPS;
        args[i].failures = 0;
        CHECK(pthread_create(&threads[i], NULL, insert_worker, &args[i]) == 0,
              "INSERT worker created");
    }

    for (i = 0; i < THREADS; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0, "INSERT worker joined");
        failures += args[i].failures;
    }

    r = engine_exec_sql("SELECT * FROM insert_users;", false);
    snprintf(expected_row_count, sizeof(expected_row_count),
             "\"row_count\":%d", EXPECTED);

    CHECK(failures == 0, "all concurrent INSERT calls succeeded");
    CHECK(r.ok, "final SELECT succeeds");
    CHECK(json_has(&r, expected_row_count), "final row_count matches inserted rows");

    engine_result_free(&r);
    engine_shutdown();
}

static void *select_worker(void *arg)
{
    select_worker_arg_t *a = (select_worker_arg_t *)arg;
    int i;

    for (i = 0; i < a->loops; i++) {
        engine_result_t r = engine_exec_sql(a->sql, false);
        if (!r.ok || !r.index_used || !json_has(&r, "warm")) {
            a->failures++;
        }
        engine_result_free(&r);
    }

    return NULL;
}

static void *insert_worker(void *arg)
{
    insert_worker_arg_t *a = (insert_worker_arg_t *)arg;
    int i;

    for (i = 0; i < a->loops; i++) {
        char sql[256];
        int id = a->thread_id * 1000 + i + 1;

        snprintf(sql, sizeof(sql),
                 "INSERT INTO insert_users (id, name) VALUES (%d, 'worker%d');",
                 id, a->thread_id);

        engine_result_t r = engine_exec_sql(sql, false);
        if (!r.ok) {
            a->failures++;
        }
        engine_result_free(&r);
    }

    return NULL;
}
