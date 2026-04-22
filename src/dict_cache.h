#ifndef DICT_CACHE_H
#define DICT_CACHE_H

#include <stdbool.h>

/*
 * Router-level dictionary response cache.
 *
 * Keys are normalized by the router, for example:
 *   english:apple
 *   id:1
 *
 * Values are complete JSON response bodies. dict_cache_get() returns a newly
 * allocated copy so callers can pass it to http_response_t without sharing
 * cache-owned memory across worker threads.
 */

typedef struct dict_cache dict_cache_t;

dict_cache_t *dict_cache_create(int capacity);
void dict_cache_destroy(dict_cache_t *cache);

bool dict_cache_get(dict_cache_t *cache, const char *key, char **out_json);
int dict_cache_put(dict_cache_t *cache, const char *key, const char *json);

void dict_cache_invalidate(dict_cache_t *cache, const char *key);
void dict_cache_invalidate_all(dict_cache_t *cache);

unsigned long dict_cache_hits(const dict_cache_t *cache);
unsigned long dict_cache_misses(const dict_cache_t *cache);

#endif /* DICT_CACHE_H */
