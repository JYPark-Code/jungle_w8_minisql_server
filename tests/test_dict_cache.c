#include "dict_cache.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) do { \
    if (cond) { \
        g_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        g_failed++; \
        printf("  [FAIL] %s\n", msg); \
    } \
} while (0)

enum {
    THREADS = 8,
    OPS_PER_THREAD = 200
};

typedef struct {
    dict_cache_t *cache;
    int thread_id;
    int failures;
} worker_arg_t;

static int g_passed;
static int g_failed;

static void test_create_and_invalid_args(void);
static void test_put_get_and_copy(void);
static void test_lru_eviction(void);
static void test_invalidate(void);
static void test_concurrent_access(void);
static void *cache_worker(void *arg);

int main(void)
{
    printf("=== test_dict_cache ===\n");

    test_create_and_invalid_args();
    test_put_get_and_copy();
    test_lru_eviction();
    test_invalidate();
    test_concurrent_access();

    printf("\n[DICT CACHE TESTS] %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

static void test_create_and_invalid_args(void)
{
    dict_cache_t *cache;
    char *json = NULL;

    printf("[TEST] create and invalid arguments\n");
    cache = dict_cache_create(0);
    CHECK(cache != NULL, "capacity <= 0 uses default capacity");
    CHECK(!dict_cache_get(cache, NULL, &json), "NULL key get returns miss");
    CHECK(json == NULL, "NULL key get leaves out_json NULL");
    CHECK(dict_cache_put(cache, NULL, "{}") == -1, "NULL key put fails");
    CHECK(dict_cache_put(cache, "english:apple", NULL) == -1, "NULL json put fails");
    CHECK(dict_cache_hits(NULL) == 0, "NULL cache hits returns 0");
    CHECK(dict_cache_misses(NULL) == 0, "NULL cache misses returns 0");
    dict_cache_destroy(cache);
}

static void test_put_get_and_copy(void)
{
    dict_cache_t *cache;
    char *first = NULL;
    char *second = NULL;

    printf("[TEST] put/get returns owned JSON copy\n");
    cache = dict_cache_create(4);
    CHECK(cache != NULL, "cache created");
    CHECK(dict_cache_put(cache, "english:apple", "{\"korean\":\"sagwa\"}") == 0,
          "put apple succeeds");
    CHECK(dict_cache_get(cache, "english:apple", &first), "get apple hits");
    CHECK(first != NULL && strcmp(first, "{\"korean\":\"sagwa\"}") == 0,
          "get apple returns stored json");
    CHECK(dict_cache_get(cache, "english:apple", &second), "second get apple hits");
    CHECK(first != second, "each get returns a separate allocation");
    CHECK(dict_cache_hits(cache) == 2, "hit counter increments");
    CHECK(dict_cache_misses(cache) == 0, "miss counter remains zero");

    free(first);
    free(second);
    dict_cache_destroy(cache);
}

static void test_lru_eviction(void)
{
    dict_cache_t *cache;
    char *json = NULL;

    printf("[TEST] LRU eviction\n");
    cache = dict_cache_create(2);
    CHECK(cache != NULL, "small cache created");
    CHECK(dict_cache_put(cache, "english:apple", "apple-json") == 0, "put apple");
    CHECK(dict_cache_put(cache, "english:banana", "banana-json") == 0, "put banana");
    CHECK(dict_cache_get(cache, "english:apple", &json), "touch apple before eviction");
    free(json);
    json = NULL;

    CHECK(dict_cache_put(cache, "english:cat", "cat-json") == 0, "put cat evicts LRU");
    CHECK(!dict_cache_get(cache, "english:banana", &json), "banana was evicted");
    CHECK(json == NULL, "evicted get leaves out_json NULL");
    CHECK(dict_cache_get(cache, "english:apple", &json), "apple remains cached");
    free(json);
    json = NULL;
    CHECK(dict_cache_get(cache, "english:cat", &json), "cat is cached");
    free(json);
    dict_cache_destroy(cache);
}

static void test_invalidate(void)
{
    dict_cache_t *cache;
    char *json = NULL;

    printf("[TEST] invalidate one/all\n");
    cache = dict_cache_create(4);
    CHECK(cache != NULL, "cache created");
    CHECK(dict_cache_put(cache, "english:apple", "apple-json") == 0, "put apple");
    CHECK(dict_cache_put(cache, "id:1", "id-json") == 0, "put id");

    dict_cache_invalidate(cache, "english:apple");
    CHECK(!dict_cache_get(cache, "english:apple", &json), "invalidated key misses");
    CHECK(dict_cache_get(cache, "id:1", &json), "other key still hits");
    free(json);
    json = NULL;

    dict_cache_invalidate_all(cache);
    CHECK(!dict_cache_get(cache, "id:1", &json), "invalidate_all clears remaining key");
    CHECK(json == NULL, "invalidate_all miss leaves out_json NULL");
    dict_cache_destroy(cache);
}

static void test_concurrent_access(void)
{
    dict_cache_t *cache;
    pthread_t threads[THREADS];
    worker_arg_t args[THREADS];
    char *missing = NULL;
    int failures = 0;
    int i;

    printf("[TEST] concurrent get/put smoke\n");
    cache = dict_cache_create(32);
    CHECK(cache != NULL, "cache created");
    CHECK(!dict_cache_get(cache, "english:not-found", &missing), "initial miss recorded");
    CHECK(missing == NULL, "initial miss leaves out_json NULL");

    for (i = 0; i < THREADS; ++i) {
        args[i].cache = cache;
        args[i].thread_id = i;
        args[i].failures = 0;
        CHECK(pthread_create(&threads[i], NULL, cache_worker, &args[i]) == 0,
              "worker created");
    }

    for (i = 0; i < THREADS; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0, "worker joined");
        failures += args[i].failures;
    }

    CHECK(failures == 0, "all workers completed cache operations");
    CHECK(dict_cache_hits(cache) > 0, "concurrent run recorded hits");
    CHECK(dict_cache_misses(cache) > 0, "concurrent run recorded misses");
    dict_cache_destroy(cache);
}

static void *cache_worker(void *arg)
{
    worker_arg_t *worker = (worker_arg_t *)arg;
    int i;

    for (i = 0; i < OPS_PER_THREAD; ++i) {
        char key[64];
        char json[96];
        char *cached = NULL;

        snprintf(key, sizeof(key), "english:word-%d", i % 16);
        snprintf(json, sizeof(json), "{\"thread\":%d,\"i\":%d}",
                 worker->thread_id, i);

        if (dict_cache_put(worker->cache, key, json) != 0) {
            worker->failures++;
            continue;
        }

        if (!dict_cache_get(worker->cache, key, &cached)) {
            worker->failures++;
            continue;
        }

        free(cached);

        if ((i % 37) == 0) {
            dict_cache_invalidate(worker->cache, key);
        }
    }

    return NULL;
}
