#include "dict_cache.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DICT_CACHE_DEFAULT_CAPACITY 128

typedef struct {
    char *key;
    char *json;
    unsigned long last_used;
} dict_cache_entry_t;

struct dict_cache {
    pthread_mutex_t mutex;
    dict_cache_entry_t *entries;
    int capacity;
    unsigned long clock;
    unsigned long hits;
    unsigned long misses;
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
    entry->last_used = 0;
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

    for (i = 0; i < cache->capacity; ++i) {
        if (cache->entries[i].key == NULL) return i;
        if (cache->entries[i].last_used < cache->entries[oldest].last_used) {
            oldest = i;
        }
    }
    return oldest;
}

dict_cache_t *dict_cache_create(int capacity)
{
    dict_cache_t *cache;

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

    if (pthread_mutex_init(&cache->mutex, NULL) != 0) {
        free(cache->entries);
        free(cache);
        return NULL;
    }

    cache->capacity = capacity;
    return cache;
}

void dict_cache_destroy(dict_cache_t *cache)
{
    int i;

    if (cache == NULL) return;

    pthread_mutex_lock(&cache->mutex);
    for (i = 0; i < cache->capacity; ++i) {
        dict_cache_entry_free(&cache->entries[i]);
    }
    pthread_mutex_unlock(&cache->mutex);

    pthread_mutex_destroy(&cache->mutex);
    free(cache->entries);
    free(cache);
}

bool dict_cache_get(dict_cache_t *cache, const char *key, char **out_json)
{
    int index;

    if (out_json == NULL) return false;
    *out_json = NULL;
    if (cache == NULL || key == NULL || key[0] == '\0') return false;

    pthread_mutex_lock(&cache->mutex);
    index = dict_cache_find(cache, key);
    if (index < 0) {
        cache->misses++;
        pthread_mutex_unlock(&cache->mutex);
        return false;
    }

    *out_json = dict_cache_strdup(cache->entries[index].json);
    if (*out_json == NULL) {
        pthread_mutex_unlock(&cache->mutex);
        return false;
    }

    cache->entries[index].last_used = ++cache->clock;
    cache->hits++;
    pthread_mutex_unlock(&cache->mutex);
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

    pthread_mutex_lock(&cache->mutex);
    index = dict_cache_find(cache, key);
    if (index < 0) {
        index = dict_cache_slot_for_put(cache);
    }

    dict_cache_entry_free(&cache->entries[index]);
    cache->entries[index].key = key_copy;
    cache->entries[index].json = json_copy;
    cache->entries[index].last_used = ++cache->clock;
    pthread_mutex_unlock(&cache->mutex);
    return 0;
}

void dict_cache_invalidate(dict_cache_t *cache, const char *key)
{
    int index;

    if (cache == NULL || key == NULL || key[0] == '\0') return;

    pthread_mutex_lock(&cache->mutex);
    index = dict_cache_find(cache, key);
    if (index >= 0) {
        dict_cache_entry_free(&cache->entries[index]);
    }
    pthread_mutex_unlock(&cache->mutex);
}

void dict_cache_invalidate_all(dict_cache_t *cache)
{
    int i;

    if (cache == NULL) return;

    pthread_mutex_lock(&cache->mutex);
    for (i = 0; i < cache->capacity; ++i) {
        dict_cache_entry_free(&cache->entries[i]);
    }
    cache->clock = 0;
    pthread_mutex_unlock(&cache->mutex);
}

unsigned long dict_cache_hits(const dict_cache_t *cache)
{
    unsigned long hits;
    dict_cache_t *mutable_cache = (dict_cache_t *)cache;

    if (cache == NULL) return 0;

    pthread_mutex_lock(&mutable_cache->mutex);
    hits = mutable_cache->hits;
    pthread_mutex_unlock(&mutable_cache->mutex);
    return hits;
}

unsigned long dict_cache_misses(const dict_cache_t *cache)
{
    unsigned long misses;
    dict_cache_t *mutable_cache = (dict_cache_t *)cache;

    if (cache == NULL) return 0;

    pthread_mutex_lock(&mutable_cache->mutex);
    misses = mutable_cache->misses;
    pthread_mutex_unlock(&mutable_cache->mutex);
    return misses;
}
