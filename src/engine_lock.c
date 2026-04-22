/* engine_lock.c — concurrency layer 구현 (지용)
 * ============================================================================
 *
 * 구현 요점:
 *   - 테이블 락 레지스트리: 고정 용량 배열 (최대 MAX_TABLES 개)
 *     · 신규 테이블은 engine_lock_table_{read,write} 시점에 lazy 등록
 *     · 레지스트리 자체는 meta mutex 로 보호 (등록/조회 race 방지)
 *     · 등록 후 RW lock 은 meta mutex 밖에서 획득 → 레지스트리 경합 최소화
 *
 *   - 카탈로그 락: 단일 pthread_rwlock_t
 *   - single mode: 단일 pthread_mutex_t (enter/exit 쌍)
 *
 *   - 모든 획득 직전 clock_gettime(CLOCK_MONOTONIC) 을 찍고,
 *     획득 후 diff 를 atomic fetch_add 로 누적 → EXPLAIN 및 /api/stats 용
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "engine_lock.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define MAX_TABLES 64

typedef struct {
    char              name[64];
    pthread_rwlock_t  lock;
    int               used;   /* 0 = slot 비어있음, 1 = 등록됨 */
} table_lock_t;

static table_lock_t    s_tables[MAX_TABLES];
static pthread_mutex_t s_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_rwlock_t s_catalog_lock;
static pthread_mutex_t  s_single_mutex;
static int              s_inited = 0;

static atomic_uint_fast64_t s_wait_ns_total;

/* ── 내부 유틸 ────────────────────────────────────────────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void record_wait(uint64_t start_ns) {
    uint64_t elapsed = now_ns() - start_ns;
    atomic_fetch_add_explicit(&s_wait_ns_total, elapsed, memory_order_relaxed);
}

/* 테이블 이름으로 slot 조회. 없으면 새 slot 등록. 실패 시 NULL.
 * 호출 전 s_registry_mutex 를 hold 해야 한다. */
static table_lock_t *registry_get_or_create_locked(const char *table) {
    for (int i = 0; i < MAX_TABLES; i++) {
        if (s_tables[i].used && strncmp(s_tables[i].name, table, sizeof(s_tables[i].name)) == 0) {
            return &s_tables[i];
        }
    }
    for (int i = 0; i < MAX_TABLES; i++) {
        if (!s_tables[i].used) {
            if (pthread_rwlock_init(&s_tables[i].lock, NULL) != 0) return NULL;
            strncpy(s_tables[i].name, table, sizeof(s_tables[i].name) - 1);
            s_tables[i].name[sizeof(s_tables[i].name) - 1] = '\0';
            s_tables[i].used = 1;
            return &s_tables[i];
        }
    }
    return NULL; /* MAX_TABLES 초과 */
}

static table_lock_t *registry_find_locked(const char *table) {
    for (int i = 0; i < MAX_TABLES; i++) {
        if (s_tables[i].used && strncmp(s_tables[i].name, table, sizeof(s_tables[i].name)) == 0) {
            return &s_tables[i];
        }
    }
    return NULL;
}

/* ── 공개 API ─────────────────────────────────────────────────────── */

int engine_lock_init(void) {
    if (s_inited) return 0;

    memset(s_tables, 0, sizeof(s_tables));
    atomic_store_explicit(&s_wait_ns_total, 0, memory_order_relaxed);

    if (pthread_rwlock_init(&s_catalog_lock, NULL) != 0) return -1;
    if (pthread_mutex_init(&s_single_mutex, NULL) != 0) {
        pthread_rwlock_destroy(&s_catalog_lock);
        return -1;
    }

    s_inited = 1;
    return 0;
}

void engine_lock_shutdown(void) {
    if (!s_inited) return;

    pthread_mutex_lock(&s_registry_mutex);
    for (int i = 0; i < MAX_TABLES; i++) {
        if (s_tables[i].used) {
            pthread_rwlock_destroy(&s_tables[i].lock);
            s_tables[i].used = 0;
        }
    }
    pthread_mutex_unlock(&s_registry_mutex);

    pthread_rwlock_destroy(&s_catalog_lock);
    pthread_mutex_destroy(&s_single_mutex);
    s_inited = 0;
}

void engine_lock_table_read(const char *table) {
    uint64_t t0 = now_ns();

    pthread_mutex_lock(&s_registry_mutex);
    table_lock_t *slot = registry_get_or_create_locked(table);
    pthread_mutex_unlock(&s_registry_mutex);

    if (slot) pthread_rwlock_rdlock(&slot->lock);
    record_wait(t0);
}

void engine_lock_table_write(const char *table) {
    uint64_t t0 = now_ns();

    pthread_mutex_lock(&s_registry_mutex);
    table_lock_t *slot = registry_get_or_create_locked(table);
    pthread_mutex_unlock(&s_registry_mutex);

    if (slot) pthread_rwlock_wrlock(&slot->lock);
    record_wait(t0);
}

void engine_lock_table_release(const char *table) {
    pthread_mutex_lock(&s_registry_mutex);
    table_lock_t *slot = registry_find_locked(table);
    pthread_mutex_unlock(&s_registry_mutex);

    if (slot) pthread_rwlock_unlock(&slot->lock);
}

void engine_lock_catalog_read(void) {
    uint64_t t0 = now_ns();
    pthread_rwlock_rdlock(&s_catalog_lock);
    record_wait(t0);
}

void engine_lock_catalog_write(void) {
    uint64_t t0 = now_ns();
    pthread_rwlock_wrlock(&s_catalog_lock);
    record_wait(t0);
}

void engine_lock_catalog_release(void) {
    pthread_rwlock_unlock(&s_catalog_lock);
}

void engine_lock_single_enter(void) {
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&s_single_mutex);
    record_wait(t0);
}

void engine_lock_single_exit(void) {
    pthread_mutex_unlock(&s_single_mutex);
}

uint64_t engine_lock_wait_ns_total(void) {
    return atomic_load_explicit(&s_wait_ns_total, memory_order_relaxed);
}
