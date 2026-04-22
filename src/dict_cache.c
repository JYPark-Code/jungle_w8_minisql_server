/* pthread_rwlock_* 는 POSIX feature test macro 가 노출되어야 선언됨.
 * CI 가 -D_POSIX_C_SOURCE 없이 Makefile CFLAGS 를 덮어쓰는 경우 대비해
 * 파일 단위에서 선언 (engine_lock.c / threadpool.c / main.c 와 동일 패턴). */
#define _POSIX_C_SOURCE 200809L

#include "dict_cache.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define DICT_CACHE_DEFAULT_CAPACITY 128

/* Router-level dictionary cache.
 *
 * 동시성 모델 (P3 fix, 2026-04-22):
 *   - pthread_rwlock_t 로 get (rdlock) vs put/invalidate (wrlock) 분리.
 *   - LRU 타임스탬프 (last_used) 와 hit/miss 카운터는 atomic 필드로 둬서
 *     rdlock 하에서도 갱신 가능 → hit 다수 시 병렬 처리.
 *   - 기존 단일 mutex 구조에서는 100 병렬 동일 단어 hit 가 nocache 보다
 *     5× 느린 역전 현상이 있었음 (Issue #24). rwlock + atomic LRU 로 해결.
 */

typedef struct {
    char *key;
    char *json;
    atomic_ulong last_used;
} dict_cache_entry_t;

struct dict_cache {
    pthread_rwlock_t lock;
    dict_cache_entry_t *entries;
    int capacity;
    atomic_ulong clock;
    atomic_ulong hits;
    atomic_ulong misses;
};

static char *dict_cache_strdup(const char *text)
{
    size_t len;
    char *copy;

    if (text == NULL) return NULL;
    len = strlen(text);
    copy = malloc(len + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, text, len + 1U);
    return copy;
}

static void dict_cache_entry_free(dict_cache_entry_t *entry)
{
    if (entry == NULL) return;
    free(entry->key);
    free(entry->json);
    entry->key = NULL;
    entry->json = NULL;
    atomic_store_explicit(&entry->last_used, 0UL, memory_order_relaxed);
}

static int dict_cache_find(const dict_cache_t *cache, const char *key)
{
    int i;

    for (i = 0; i < cache->capacity; ++i) {
        if (cache->entries[i].key != NULL &&
            strcmp(cache->entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int dict_cache_slot_for_put(const dict_cache_t *cache)
{
    int i;
    int oldest = 0;
    unsigned long oldest_used;

    oldest_used = atomic_load_explicit(&cache->entries[0].last_used,
                                       memory_order_relaxed);
    for (i = 0; i < cache->capacity; ++i) {
        unsigned long used;
        if (cache->entries[i].key == NULL) return i;
        used = atomic_load_explicit(&cache->entries[i].last_used,
                                    memory_order_relaxed);
        if (used < oldest_used) {
            oldest = i;
            oldest_used = used;
        }
    }
    return oldest;
}

dict_cache_t *dict_cache_create(int capacity)
{
    dict_cache_t *cache;
    int i;

    if (capacity <= 0) {
        capacity = DICT_CACHE_DEFAULT_CAPACITY;
    }

    cache = calloc(1, sizeof(*cache));
    if (cache == NULL) return NULL;

    cache->entries = calloc((size_t)capacity, sizeof(*cache->entries));
    if (cache->entries == NULL) {
        free(cache);
        return NULL;
    }

    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        free(cache->entries);
        free(cache);
        return NULL;
    }

    for (i = 0; i < capacity; ++i) {
        atomic_init(&cache->entries[i].last_used, 0UL);
    }
    atomic_init(&cache->clock, 0UL);
    atomic_init(&cache->hits, 0UL);
    atomic_init(&cache->misses, 0UL);

    cache->capacity = capacity;
    return cache;
}

void dict_cache_destroy(dict_cache_t *cache)
{
    int i;

    if (cache == NULL) return;

    pthread_rwlock_wrlock(&cache->lock);
    for (i = 0; i < cache->capacity; ++i) {
        dict_cache_entry_free(&cache->entries[i]);
    }
    pthread_rwlock_unlock(&cache->lock);

    pthread_rwlock_destroy(&cache->lock);
    free(cache->entries);
    free(cache);
}

bool dict_cache_get(dict_cache_t *cache, const char *key, char **out_json)
{
    int index;

    if (out_json == NULL) return false;
    *out_json = NULL;
    if (cache == NULL || key == NULL || key[0] == '\0') return false;

    pthread_rwlock_rdlock(&cache->lock);
    index = dict_cache_find(cache, key);
    if (index < 0) {
        atomic_fetch_add_explicit(&cache->misses, 1UL, memory_order_relaxed);
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }

    /* strdup 은 rdlock 안에서 — writer 가 entries[index].json 을 free 하지
     * 못하도록 보장. rwlock 이 writer 를 막아주므로 안전. */
    *out_json = dict_cache_strdup(cache->entries[index].json);
    if (*out_json == NULL) {
        pthread_rwlock_unlock(&cache->lock);
        return false;
    }

    /* LRU 타임스탬프 갱신 — atomic store 로 rdlock 하에서도 안전. */
    atomic_store_explicit(&cache->entries[index].last_used,
        atomic_fetch_add_explicit(&cache->clock, 1UL, memory_order_relaxed) + 1UL,
        memory_order_relaxed);
    atomic_fetch_add_explicit(&cache->hits, 1UL, memory_order_relaxed);
    pthread_rwlock_unlock(&cache->lock);
    return true;
}

int dict_cache_put(dict_cache_t *cache, const char *key, const char *json)
{
    int index;
    char *key_copy;
    char *json_copy;

    if (cache == NULL || key == NULL || key[0] == '\0' || json == NULL) {
        return -1;
    }

    key_copy = dict_cache_strdup(key);
    if (key_copy == NULL) return -1;

    json_copy = dict_cache_strdup(json);
    if (json_copy == NULL) {
        free(key_copy);
        return -1;
    }

    pthread_rwlock_wrlock(&cache->lock);
    index = dict_cache_find(cache, key);
    if (index < 0) {
        index = dict_cache_slot_for_put(cache);
    }

    dict_cache_entry_free(&cache->entries[index]);
    cache->entries[index].key = key_copy;
    cache->entries[index].json = json_copy;
    atomic_store_explicit(&cache->entries[index].last_used,
        atomic_fetch_add_explicit(&cache->clock, 1UL, memory_order_relaxed) + 1UL,
        memory_order_relaxed);
    pthread_rwlock_unlock(&cache->lock);
    return 0;
}

void dict_cache_invalidate(dict_cache_t *cache, const char *key)
{
    int index;

    if (cache == NULL || key == NULL || key[0] == '\0') return;

    pthread_rwlock_wrlock(&cache->lock);
    index = dict_cache_find(cache, key);
    if (index >= 0) {
        dict_cache_entry_free(&cache->entries[index]);
    }
    pthread_rwlock_unlock(&cache->lock);
}

void dict_cache_invalidate_all(dict_cache_t *cache)
{
    int i;

    if (cache == NULL) return;

    pthread_rwlock_wrlock(&cache->lock);
    for (i = 0; i < cache->capacity; ++i) {
        dict_cache_entry_free(&cache->entries[i]);
    }
    atomic_store_explicit(&cache->clock, 0UL, memory_order_relaxed);
    pthread_rwlock_unlock(&cache->lock);
}

unsigned long dict_cache_hits(const dict_cache_t *cache)
{
    if (cache == NULL) return 0;
    return atomic_load_explicit(&cache->hits, memory_order_relaxed);
}

unsigned long dict_cache_misses(const dict_cache_t *cache)
{
    if (cache == NULL) return 0;
    return atomic_load_explicit(&cache->misses, memory_order_relaxed);
}
