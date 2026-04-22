/* test_engine_lock.c — engine_lock concurrency 단위 테스트 (지용)
 * ============================================================================
 *
 * 검증 범위:
 *   T1. init / shutdown 정상 동작 (재진입 안전성 포함)
 *   T2. 단일 스레드에서 테이블 RW lock 획득 / 해제
 *   T3. 서로 다른 테이블 락은 간섭 없음 (독립 lockspace)
 *   T4. 동일 테이블 rdlock 다중 진입 가능 (N 리더 동시 통과)
 *   T5. wrlock 은 rdlock 과 상호 배타 (deadlock 없이 직렬화)
 *   T6. catalog 전역 RW lock 은 테이블 lock 과 독립
 *   T7. single-mode mutex 는 enter / exit 쌍으로 동작
 *   T8. engine_lock_wait_ns_total 누적 단조 증가
 *
 * 실행:
 *   make test_engine_lock      (Makefile 타겟이 빌드 + 실행)
 *
 * 성공 시 stdout 마지막 줄: "All engine_lock tests passed."
 * 실패 시 exit code 1.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "engine_lock.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PASS(name) printf("[PASS] %s\n", (name))
#define FAIL(name) do { printf("[FAIL] %s\n", (name)); exit(1); } while (0)

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ── T1 ─────────────────────────────────────────────────────────── */

static void t1_init_shutdown(void) {
    if (engine_lock_init() != 0) FAIL("T1 init");
    engine_lock_shutdown();

    /* 재진입: shutdown 후 다시 init 가능해야 함 (프로세스 재시작 대신 사용) */
    if (engine_lock_init() != 0) FAIL("T1 re-init");
    engine_lock_shutdown();
    PASS("T1 init/shutdown");
}

/* ── T2 ─────────────────────────────────────────────────────────── */

static void t2_single_thread_rw(void) {
    if (engine_lock_init() != 0) FAIL("T2 init");

    engine_lock_table_read("users");
    engine_lock_table_release("users");

    engine_lock_table_write("users");
    engine_lock_table_release("users");

    engine_lock_shutdown();
    PASS("T2 single-thread table RW");
}

/* ── T3 ─────────────────────────────────────────────────────────── */

static void t3_independent_tables(void) {
    if (engine_lock_init() != 0) FAIL("T3 init");

    /* 서로 다른 테이블에 write lock 을 동시에 들고 있어도 문제 없어야 함 */
    engine_lock_table_write("orders");
    engine_lock_table_write("products");   /* 같은 스레드에서도 다른 테이블이면 OK */
    engine_lock_table_release("orders");
    engine_lock_table_release("products");

    engine_lock_shutdown();
    PASS("T3 independent table locks");
}

/* ── T4 ─────────────────────────────────────────────────────────── */

#define T4_READERS 8

typedef struct {
    atomic_int     entered;
    atomic_int     leave;
    atomic_int     max_concurrent;
    pthread_barrier_t barrier;
} t4_ctx;

static void *t4_reader(void *arg) {
    t4_ctx *c = arg;
    pthread_barrier_wait(&c->barrier);

    engine_lock_table_read("payments");
    int cur = atomic_fetch_add(&c->entered, 1) + 1;

    /* max_concurrent 갱신 (lock-free "max" update) */
    int prev;
    do {
        prev = atomic_load(&c->max_concurrent);
        if (cur <= prev) break;
    } while (!atomic_compare_exchange_weak(&c->max_concurrent, &prev, cur));

    msleep(30);   /* 리더들이 동시에 머물러 있도록 잠시 대기 */
    atomic_fetch_sub(&c->entered, 1);
    atomic_fetch_add(&c->leave, 1);
    engine_lock_table_release("payments");
    return NULL;
}

static void t4_concurrent_readers(void) {
    if (engine_lock_init() != 0) FAIL("T4 init");

    t4_ctx c;
    atomic_store(&c.entered, 0);
    atomic_store(&c.leave, 0);
    atomic_store(&c.max_concurrent, 0);
    pthread_barrier_init(&c.barrier, NULL, T4_READERS);

    pthread_t ths[T4_READERS];
    for (int i = 0; i < T4_READERS; i++) {
        if (pthread_create(&ths[i], NULL, t4_reader, &c) != 0) FAIL("T4 pthread_create");
    }
    for (int i = 0; i < T4_READERS; i++) pthread_join(ths[i], NULL);

    pthread_barrier_destroy(&c.barrier);

    /* 여러 리더가 동시에 진입했어야 함 (2 이상이면 공유 rdlock 증명) */
    if (atomic_load(&c.max_concurrent) < 2) FAIL("T4 readers did not share rdlock");
    if (atomic_load(&c.leave) != T4_READERS) FAIL("T4 some readers missing");

    engine_lock_shutdown();
    PASS("T4 concurrent readers share rdlock");
}

/* ── T5 ─────────────────────────────────────────────────────────── */

typedef struct {
    atomic_int       writers_inside;
    atomic_int       observed_conflict;
    atomic_int       total_runs;
    pthread_barrier_t barrier;
} t5_ctx;

static void *t5_writer(void *arg) {
    t5_ctx *c = arg;
    pthread_barrier_wait(&c->barrier);

    for (int i = 0; i < 100; i++) {
        engine_lock_table_write("t5");
        int inside = atomic_fetch_add(&c->writers_inside, 1) + 1;
        if (inside > 1) atomic_store(&c->observed_conflict, 1);
        /* critical section */
        atomic_fetch_sub(&c->writers_inside, 1);
        engine_lock_table_release("t5");
        atomic_fetch_add(&c->total_runs, 1);
    }
    return NULL;
}

static void t5_writer_mutual_exclusion(void) {
    if (engine_lock_init() != 0) FAIL("T5 init");

    t5_ctx c;
    atomic_store(&c.writers_inside, 0);
    atomic_store(&c.observed_conflict, 0);
    atomic_store(&c.total_runs, 0);
    pthread_barrier_init(&c.barrier, NULL, 4);

    pthread_t ths[4];
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&ths[i], NULL, t5_writer, &c) != 0) FAIL("T5 pthread_create");
    }
    for (int i = 0; i < 4; i++) pthread_join(ths[i], NULL);

    pthread_barrier_destroy(&c.barrier);

    if (atomic_load(&c.observed_conflict) != 0) FAIL("T5 writers overlapped");
    if (atomic_load(&c.total_runs) != 4 * 100)  FAIL("T5 writer count mismatch");

    engine_lock_shutdown();
    PASS("T5 writer mutual exclusion");
}

/* ── T6 ─────────────────────────────────────────────────────────── */

static void t6_catalog_lock(void) {
    if (engine_lock_init() != 0) FAIL("T6 init");

    engine_lock_catalog_read();
    engine_lock_catalog_release();

    engine_lock_catalog_write();
    engine_lock_catalog_release();

    /* catalog 과 테이블 락은 독립적. 서로 홀드해도 문제 없어야 함. */
    engine_lock_table_write("t6");
    engine_lock_catalog_read();
    engine_lock_catalog_release();
    engine_lock_table_release("t6");

    engine_lock_shutdown();
    PASS("T6 catalog lock independent of table lock");
}

/* ── T7 ─────────────────────────────────────────────────────────── */

typedef struct {
    atomic_int       inside;
    atomic_int       conflict;
    pthread_barrier_t barrier;
} t7_ctx;

static void *t7_worker(void *arg) {
    t7_ctx *c = arg;
    pthread_barrier_wait(&c->barrier);

    for (int i = 0; i < 200; i++) {
        engine_lock_single_enter();
        int in = atomic_fetch_add(&c->inside, 1) + 1;
        if (in > 1) atomic_store(&c->conflict, 1);
        atomic_fetch_sub(&c->inside, 1);
        engine_lock_single_exit();
    }
    return NULL;
}

static void t7_single_mode(void) {
    if (engine_lock_init() != 0) FAIL("T7 init");

    t7_ctx c;
    atomic_store(&c.inside, 0);
    atomic_store(&c.conflict, 0);
    pthread_barrier_init(&c.barrier, NULL, 4);

    pthread_t ths[4];
    for (int i = 0; i < 4; i++) {
        if (pthread_create(&ths[i], NULL, t7_worker, &c) != 0) FAIL("T7 pthread_create");
    }
    for (int i = 0; i < 4; i++) pthread_join(ths[i], NULL);
    pthread_barrier_destroy(&c.barrier);

    if (atomic_load(&c.conflict) != 0) FAIL("T7 single-mode not mutex-exclusive");

    engine_lock_shutdown();
    PASS("T7 single-mode mutex serializes callers");
}

/* ── T8 ─────────────────────────────────────────────────────────── */

static void t8_wait_ns_monotone(void) {
    if (engine_lock_init() != 0) FAIL("T8 init");

    uint64_t w0 = engine_lock_wait_ns_total();

    /* 몇 번 lock 획득시키면 누적 대기시간이 반드시 증가해야 함 */
    for (int i = 0; i < 1000; i++) {
        engine_lock_table_read("t8");
        engine_lock_table_release("t8");
    }

    uint64_t w1 = engine_lock_wait_ns_total();
    if (w1 < w0) FAIL("T8 wait_ns decreased");
    if (w1 == w0) FAIL("T8 wait_ns did not accumulate");

    engine_lock_shutdown();
    PASS("T8 wait_ns accumulates monotonically");
}

/* ── main ───────────────────────────────────────────────────────── */

int main(void) {
    printf("=== engine_lock unit tests ===\n");

    t1_init_shutdown();
    t2_single_thread_rw();
    t3_independent_tables();
    t4_concurrent_readers();
    t5_writer_mutual_exclusion();
    t6_catalog_lock();
    t7_single_mode();
    t8_wait_ns_monotone();

    printf("All engine_lock tests passed.\n");
    return 0;
}
