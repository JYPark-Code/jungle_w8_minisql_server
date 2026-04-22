/* test_trie.c -- Round 2 Trie core and engine prefix integration tests. */

#define _POSIX_C_SOURCE 200809L

#include "engine.h"
#include "trie.h"

#include <errno.h>
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

static void remove_file_if_exists(const char *path);
static void cleanup_dictionary_fixture(void);
static int json_has(const engine_result_t *r, const char *needle);
static int json_lacks(const engine_result_t *r, const char *needle);
static void test_trie_create_destroy(void);
static void test_trie_insert_exact_and_prefix(void);
static void test_trie_max_out_and_missing_prefix(void);
static void test_trie_duplicate_and_invalid_input(void);
static void test_engine_prefix_select_uses_trie(void);

int main(void)
{
    printf("=== test_trie ===\n");

    test_trie_create_destroy();
    test_trie_insert_exact_and_prefix();
    test_trie_max_out_and_missing_prefix();
    test_trie_duplicate_and_invalid_input();
    test_engine_prefix_select_uses_trie();

    cleanup_dictionary_fixture();

    printf("\n[TRIE TESTS] %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

static void remove_file_if_exists(const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "failed to remove file: %s\n", path);
    }
}

static void cleanup_dictionary_fixture(void)
{
    remove_file_if_exists("data/tables/dictionary.csv");
    remove_file_if_exists("data/tables/dictionary.csv.tmp");
    remove_file_if_exists("data/tables/dictionary.bin");
    remove_file_if_exists("data/schema/dictionary.schema");
    remove_file_if_exists("data/dictionary.csv");
    remove_file_if_exists("data/dictionary.csv.tmp");
    remove_file_if_exists("data/dictionary.schema");

    (void)TEST_RMDIR("data/tables");
    (void)TEST_RMDIR("data/schema");
    (void)TEST_RMDIR("data");
}

static int json_has(const engine_result_t *r, const char *needle)
{
    return r != NULL && r->json != NULL && strstr(r->json, needle) != NULL;
}

static int json_lacks(const engine_result_t *r, const char *needle)
{
    return r != NULL && r->json != NULL && strstr(r->json, needle) == NULL;
}

static void test_trie_create_destroy(void)
{
    trie_t *t;

    printf("[TEST] trie create/destroy\n");
    t = trie_create();
    CHECK(t != NULL, "trie_create returns non-NULL");
    trie_destroy(t);
    trie_destroy(NULL);
    CHECK(1, "trie_destroy is NULL-safe");
}

static void test_trie_insert_exact_and_prefix(void)
{
    trie_t *t = trie_create();
    int out[8];
    int n;

    printf("[TEST] insert, exact search, prefix search\n");
    CHECK(t != NULL, "trie_create succeeds");
    CHECK(trie_insert(t, "app", 10) == 0, "insert app");
    CHECK(trie_insert(t, "apple", 11) == 0, "insert apple");
    CHECK(trie_insert(t, "application", 12) == 0, "insert application");
    CHECK(trie_insert(t, "banana", 20) == 0, "insert banana");

    CHECK(trie_search_exact(t, "app") == 10, "exact app -> row_index 10");
    CHECK(trie_search_exact(t, "apple") == 11, "exact apple -> row_index 11");
    CHECK(trie_search_exact(t, "apply") == -1, "missing exact -> -1");

    n = trie_search_prefix(t, "app", out, 8);
    CHECK(n == 3, "prefix app returns 3 rows");
    CHECK(out[0] == 10 && out[1] == 11 && out[2] == 12,
          "prefix rows are collected in lexical order");

    trie_destroy(t);
}

static void test_trie_max_out_and_missing_prefix(void)
{
    trie_t *t = trie_create();
    int out[2];
    int n;

    printf("[TEST] prefix max_out and missing prefix\n");
    CHECK(t != NULL, "trie_create succeeds");
    CHECK(trie_insert(t, "app", 1) == 0, "insert app");
    CHECK(trie_insert(t, "apple", 2) == 0, "insert apple");
    CHECK(trie_insert(t, "application", 3) == 0, "insert application");

    n = trie_search_prefix(t, "app", out, 2);
    CHECK(n == 2, "max_out limits prefix result count");
    CHECK(out[0] == 1 && out[1] == 2, "max_out keeps first two rows");
    CHECK(trie_search_prefix(t, "zzz", out, 2) == 0, "missing prefix returns 0");
    CHECK(trie_search_prefix(t, "app", out, 0) == 0, "max_out=0 returns 0");

    trie_destroy(t);
}

static void test_trie_duplicate_and_invalid_input(void)
{
    trie_t *t = trie_create();
    int out[4];

    printf("[TEST] duplicate overwrite and invalid input\n");
    CHECK(t != NULL, "trie_create succeeds");
    CHECK(trie_insert(t, "apple", 1) == 0, "insert apple");
    CHECK(trie_insert(t, "apple", 7) == 0, "duplicate overwrites row_index");
    CHECK(trie_search_exact(t, "apple") == 7, "exact search sees latest row_index");

    CHECK(trie_insert(t, "Apple", 2) != 0, "uppercase word rejected");
    CHECK(trie_insert(t, "app1", 2) != 0, "digit word rejected");
    CHECK(trie_insert(t, "", 2) != 0, "empty word rejected");
    CHECK(trie_insert(t, "apply", -1) != 0, "negative row_index rejected");
    CHECK(trie_search_exact(t, "Apple") == -1, "uppercase exact rejected");
    CHECK(trie_search_prefix(t, "ap_", out, 4) == 0, "invalid prefix returns 0");

    trie_destroy(t);
}

static void test_engine_prefix_select_uses_trie(void)
{
    engine_result_t r;

    printf("[TEST] engine dictionary LIKE prefix uses Trie fast path\n");
    cleanup_dictionary_fixture();
    CHECK(engine_init("./data") == 0, "engine_init succeeds");

    r = engine_exec_sql(
        "CREATE TABLE dictionary (id INT, english VARCHAR, korean VARCHAR);"
        "INSERT INTO dictionary (id, english, korean) VALUES (100, 'banana', 'banana-ko');"
        "INSERT INTO dictionary (id, english, korean) VALUES (20, 'apple', 'sagwa');"
        "INSERT INTO dictionary (id, english, korean) VALUES (300, 'application', 'eungyong');"
        "INSERT INTO dictionary (id, english, korean) VALUES (40, 'app', 'app-ko');",
        false);
    CHECK(r.ok, "dictionary fixture setup succeeds");
    engine_result_free(&r);

    r = engine_exec_sql(
        "SELECT english,korean FROM dictionary WHERE english LIKE 'app%' LIMIT 5;",
        false);
    CHECK(r.ok, "prefix SELECT succeeds");
    CHECK(r.index_used, "prefix SELECT marks index_used");
    CHECK(json_has(&r, "\"row_count\":3"), "prefix SELECT returns 3 rows");
    CHECK(json_has(&r, "app"), "prefix SELECT contains app");
    CHECK(json_has(&r, "apple"), "prefix SELECT contains apple");
    CHECK(json_has(&r, "application"), "prefix SELECT contains application");
    CHECK(json_lacks(&r, "banana"), "prefix SELECT excludes banana");
    engine_result_free(&r);

    r = engine_explain("SELECT english FROM dictionary WHERE english LIKE 'app%';");
    CHECK(r.ok, "prefix EXPLAIN succeeds");
    CHECK(r.index_used, "prefix EXPLAIN marks index_used");
    CHECK(json_has(&r, "TRIE_PREFIX_SCAN"), "prefix EXPLAIN names trie plan");
    engine_result_free(&r);

    r = engine_exec_sql("SELECT english FROM dictionary WHERE korean = 'sagwa';", false);
    CHECK(r.ok, "korean reverse lookup fallback succeeds");
    CHECK(!r.index_used, "korean reverse lookup does not use Trie");
    CHECK(json_has(&r, "apple"), "korean reverse lookup returns apple");
    engine_result_free(&r);

    r = engine_exec_sql("SELECT english FROM dictionary WHERE english LIKE 'a_p%';", false);
    CHECK(r.ok, "wildcard LIKE fallback succeeds");
    CHECK(!r.index_used, "wildcard LIKE does not use Trie");
    CHECK(json_has(&r, "apple"), "wildcard LIKE still uses storage fallback");
    engine_result_free(&r);

    r = engine_exec_sql(
        "INSERT INTO dictionary (id, english, korean) VALUES (400, 'apply', 'jeogyong');",
        false);
    CHECK(r.ok, "dictionary INSERT after initial build succeeds");
    engine_result_free(&r);

    r = engine_exec_sql("SELECT english FROM dictionary WHERE english LIKE 'app%';", false);
    CHECK(r.ok, "prefix SELECT after INSERT succeeds");
    CHECK(r.index_used, "prefix SELECT after INSERT uses Trie");
    CHECK(json_has(&r, "\"row_count\":4"), "prefix SELECT after INSERT returns 4 rows");
    CHECK(json_has(&r, "apply"), "prefix SELECT after INSERT contains new word");
    engine_result_free(&r);

    engine_shutdown();
    cleanup_dictionary_fixture();
}
