/* test_router.c -- Router endpoint tests for Trie-backed dictionary APIs. */

#define _POSIX_C_SOURCE 200809L

#include "engine.h"
#include "protocol.h"
#include "router.h"

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
static int setup_dictionary_fixture(void);
static void response_free(http_response_t *resp);
static int response_has(const http_response_t *resp, const char *needle);
static void set_request(http_request_t *req, const char *method,
                        const char *path, const char *query,
                        const char *body);
static http_response_t dispatch_request(const char *method, const char *path,
                                        const char *query, const char *body);
static void test_autocomplete_success(void);
static void test_autocomplete_validation(void);
static void test_admin_insert_success(void);
static void test_admin_insert_validation(void);
static void test_admin_insert_invalidates_dict_cache(void);

int main(void)
{
    printf("=== test_router ===\n");

    test_autocomplete_success();
    test_autocomplete_validation();
    test_admin_insert_success();
    test_admin_insert_validation();
    test_admin_insert_invalidates_dict_cache();

    cleanup_dictionary_fixture();

    printf("\n[ROUTER TESTS] %d passed, %d failed\n", g_passed, g_failed);
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

static int setup_dictionary_fixture(void)
{
    engine_result_t r;
    int ok;

    cleanup_dictionary_fixture();
    if (engine_init("./data") != 0) {
        return -1;
    }

    r = engine_exec_sql(
        "CREATE TABLE dictionary (id INT, english VARCHAR, korean VARCHAR);"
        "INSERT INTO dictionary (id, english, korean) VALUES (1, 'app', 'app-ko');"
        "INSERT INTO dictionary (id, english, korean) VALUES (2, 'apple', 'apple-ko');"
        "INSERT INTO dictionary (id, english, korean) VALUES (3, 'application', 'application-ko');"
        "INSERT INTO dictionary (id, english, korean) VALUES (4, 'banana', 'banana-ko');"
        "INSERT INTO dictionary (id, english, korean) VALUES (5, 'cacheword', 'cache-old');",
        false);
    ok = r.ok;
    engine_result_free(&r);

    if (!ok) {
        engine_shutdown();
        cleanup_dictionary_fixture();
        return -1;
    }

    return 0;
}

static void response_free(http_response_t *resp)
{
    if (resp == NULL) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

static int response_has(const http_response_t *resp, const char *needle)
{
    return resp != NULL && resp->body != NULL && strstr(resp->body, needle) != NULL;
}

static void set_request(http_request_t *req, const char *method,
                        const char *path, const char *query,
                        const char *body)
{
    memset(req, 0, sizeof(*req));
    snprintf(req->method, sizeof(req->method), "%s", method);
    snprintf(req->path, sizeof(req->path), "%s", path);
    snprintf(req->query, sizeof(req->query), "%s", query ? query : "");
    req->body = (char *)body;
    req->content_length = body ? strlen(body) : 0U;
}

static http_response_t dispatch_request(const char *method, const char *path,
                                        const char *query, const char *body)
{
    http_request_t req;
    http_response_t resp;

    set_request(&req, method, path, query, body);
    memset(&resp, 0, sizeof(resp));
    if (router_dispatch(&req, &resp) != 0) {
        response_free(&resp);
        memset(&resp, 0, sizeof(resp));
        resp.status = 500;
    }
    return resp;
}

static void test_autocomplete_success(void)
{
    http_response_t resp;

    printf("[TEST] GET /api/autocomplete success\n");
    CHECK(setup_dictionary_fixture() == 0, "dictionary fixture setup succeeds");

    resp = dispatch_request("GET", "/api/autocomplete", "prefix=app&limit=2", NULL);
    CHECK(resp.status == 200, "autocomplete returns 200");
    CHECK(response_has(&resp, "\"ok\":true"), "autocomplete ok true");
    CHECK(response_has(&resp, "\"prefix\":\"app\""), "autocomplete echoes prefix");
    CHECK(response_has(&resp, "\"suggestions\":[\"app\",\"apple\"]"),
          "autocomplete returns limited suggestions");
    CHECK(response_has(&resp, "\"index_used\":true"),
          "autocomplete engine uses Trie prefix scan");
    CHECK(!response_has(&resp, "banana"), "autocomplete excludes unrelated word");
    response_free(&resp);

    engine_shutdown();
    cleanup_dictionary_fixture();
}

static void test_autocomplete_validation(void)
{
    http_response_t resp;

    printf("[TEST] GET /api/autocomplete validation\n");

    resp = dispatch_request("POST", "/api/autocomplete", "prefix=app", NULL);
    CHECK(resp.status == 405, "autocomplete rejects POST");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/autocomplete", "", NULL);
    CHECK(resp.status == 400, "autocomplete requires prefix");
    CHECK(response_has(&resp, "invalid_prefix"), "missing prefix error");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/autocomplete", "prefix=App", NULL);
    CHECK(resp.status == 400, "autocomplete rejects uppercase prefix");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/autocomplete", "prefix=app&limit=21", NULL);
    CHECK(resp.status == 400, "autocomplete rejects limit above max");
    CHECK(response_has(&resp, "invalid_limit"), "invalid limit error");
    response_free(&resp);
}

static void test_admin_insert_success(void)
{
    http_response_t resp;

    printf("[TEST] POST /api/admin/insert success\n");
    CHECK(setup_dictionary_fixture() == 0, "dictionary fixture setup succeeds");

    resp = dispatch_request("POST", "/api/admin/insert", "",
                            "{\"english\":\"apply\",\"korean\":\"apply-ko\"}");
    CHECK(resp.status == 200, "admin insert returns 200");
    CHECK(response_has(&resp, "\"ok\":true"), "admin insert ok true");
    CHECK(response_has(&resp, "\"english\":\"apply\""), "admin insert echoes english");
    CHECK(!response_has(&resp, "row_id"), "admin insert does not expose guessed row_id");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/autocomplete", "prefix=apply&limit=5", NULL);
    CHECK(resp.status == 200, "autocomplete after insert returns 200");
    CHECK(response_has(&resp, "\"suggestions\":[\"apply\"]"),
          "autocomplete sees inserted word");
    response_free(&resp);

    engine_shutdown();
    cleanup_dictionary_fixture();
}

static void test_admin_insert_validation(void)
{
    http_response_t resp;

    printf("[TEST] POST /api/admin/insert validation\n");

    resp = dispatch_request("GET", "/api/admin/insert", "", NULL);
    CHECK(resp.status == 405, "admin insert rejects GET");
    response_free(&resp);

    resp = dispatch_request("POST", "/api/admin/insert", "", NULL);
    CHECK(resp.status == 400, "admin insert rejects empty body");
    response_free(&resp);

    resp = dispatch_request("POST", "/api/admin/insert", "",
                            "{\"english\":\"Apply\",\"korean\":\"apply-ko\"}");
    CHECK(resp.status == 400, "admin insert rejects uppercase english");
    response_free(&resp);

    resp = dispatch_request("POST", "/api/admin/insert", "",
                            "{\"english\":\"apply\"}");
    CHECK(resp.status == 400, "admin insert requires korean field");
    CHECK(response_has(&resp, "invalid_payload"), "invalid payload error");
    response_free(&resp);
}

static void test_admin_insert_invalidates_dict_cache(void)
{
    http_response_t resp;

    printf("[TEST] POST /api/admin/insert invalidates /api/dict cache\n");
    CHECK(setup_dictionary_fixture() == 0, "dictionary fixture setup succeeds");

    resp = dispatch_request("GET", "/api/dict", "english=cacheword", NULL);
    CHECK(resp.status == 200, "first dict lookup returns 200");
    CHECK(response_has(&resp, "\"cache_hit\":false"), "first dict lookup misses cache");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/dict", "english=cacheword", NULL);
    CHECK(resp.status == 200, "second dict lookup returns 200");
    CHECK(response_has(&resp, "\"cache_hit\":true"), "second dict lookup hits cache");
    response_free(&resp);

    resp = dispatch_request("POST", "/api/admin/insert", "",
                            "{\"english\":\"cacheword\",\"korean\":\"cache-new\"}");
    CHECK(resp.status == 200, "admin insert duplicate word returns 200");
    response_free(&resp);

    resp = dispatch_request("GET", "/api/dict", "english=cacheword", NULL);
    CHECK(resp.status == 200, "dict lookup after insert returns 200");
    CHECK(response_has(&resp, "\"cache_hit\":false"),
          "dict lookup misses cache after insert invalidation");
    response_free(&resp);

    engine_shutdown();
    cleanup_dictionary_fixture();
}
