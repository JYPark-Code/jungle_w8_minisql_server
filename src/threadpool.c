/* threadpool.c — 동적 확장/축소 + graceful shutdown worker pool
 * ============================================================================
 * Round 1: 고정 크기 pool, blocking queue (승진)
 * Round 2: 동적 resize + monitor 스레드 + graceful shutdown (지용)
 *
 * 자세한 외부 API 와 정책은 include/threadpool.h 참조.
 *
 * 내부 구조 요약
 *   - 워커 배열은 TP_ABS_MAX (64) 크기 고정 (realloc race 회피). 실제 활성 수는
 *     tp->n_workers 가 추적
 *   - 각 슬롯에 alive / retiring 플래그. 확장은 빈 슬롯에 pthread_create,
 *     축소는 retiring 플래그 세팅 → broadcast → join
 *   - Monitor 스레드는 TP_SAMPLE_MS 간격으로 utilization 측정 → 3 회 연속
 *     고점이면 확장, 30 회 연속 저점이면 축소 (hysteresis 2 샘플)
 *   - Graceful shutdown: submit_closed → queue_depth 0 대기 → 정상 종료
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "threadpool.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Round 2 튜닝 상수 ─────────────────────────────────────────────────── */
#define TP_ABS_MAX           64   /* 하드 상한 (슬롯 배열 크기) */
#define TP_SOFT_MAX          16   /* auto-expand cap */
#define TP_STEP               4   /* +/-4 per auto resize */
#define TP_HIGH_WATER_PCT    80   /* expand trigger */
#define TP_LOW_WATER_PCT     30   /* shrink trigger */
#define TP_SAMPLE_MS       1000   /* monitor 샘플 주기 */
#define TP_HIGH_CONSECUTIVE   3   /* 확장 트리거까지의 연속 고점 샘플 수 */
#define TP_LOW_CONSECUTIVE   30   /* 축소 트리거까지의 연속 저점 샘플 수 */
#define TP_HYSTERESIS_SAMPLES 2   /* resize 직후 무시할 샘플 수 */

/* ── 내부 타입 ─────────────────────────────────────────────────────────── */

typedef struct threadpool_job {
    threadpool_job_fn         fn;
    void                     *arg;
    struct threadpool_job    *next;
} threadpool_job_t;

/* worker 에 전달할 자기 자신 slot index. pool 당 독립 heap 으로 할당. */
typedef struct worker_ctx {
    struct threadpool *tp;
    int                slot;
} worker_ctx_t;

struct threadpool {
    pthread_mutex_t   mutex;
    pthread_cond_t    cond_nonempty;
    pthread_cond_t    cond_queue_empty;  /* graceful drain 대기용 */

    pthread_t         workers[TP_ABS_MAX];
    int               worker_alive[TP_ABS_MAX];    /* 1 = thread live, joinable */
    int               worker_retiring[TP_ABS_MAX]; /* 1 = exit on next wake */

    int               n_workers;        /* 현재 활성 worker 수 */
    int               floor_workers;    /* 초기 생성값 — 축소 하한 */

    threadpool_job_t *head;
    threadpool_job_t *tail;
    int               queue_depth;
    int               active_workers;   /* 실행 중 (job 을 쥐고 있는) 수 */

    int               shutdown;         /* hard shutdown flag */
    int               submit_closed;    /* graceful: submit 거부 */

    /* monitor */
    pthread_t         monitor;
    int               monitor_started;
    int               monitor_should_exit;
    int               high_consecutive;
    int               low_consecutive;
    int               skip_samples;
};

/* ── 시간 유틸 ─────────────────────────────────────────────────────────── */

static void add_ms_to_timespec(struct timespec *ts, int ms) {
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ── queue helper ──────────────────────────────────────────────────────── */

static void threadpool_free_jobs(threadpool_t *tp) {
    threadpool_job_t *job = tp->head;
    while (job != NULL) {
        threadpool_job_t *next = job->next;
        free(job);
        job = next;
    }
    tp->head = NULL;
    tp->tail = NULL;
    tp->queue_depth = 0;
}

/* ── worker 스레드 ─────────────────────────────────────────────────────── */

static void *threadpool_worker_main(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    threadpool_t *tp  = ctx->tp;
    int           slot = ctx->slot;
    free(ctx);

    for (;;) {
        threadpool_job_t *job;
        threadpool_job_fn fn;
        void             *job_arg;

        pthread_mutex_lock(&tp->mutex);
        while (tp->head == NULL &&
               !tp->shutdown &&
               !tp->worker_retiring[slot]) {
            pthread_cond_wait(&tp->cond_nonempty, &tp->mutex);
        }

        /* 종료 조건: shutdown 이거나 retirement 요청 받음 (+ queue 비었거나
         * retiring 이면 즉시 나감). retiring worker 는 남은 job 을 안 받음. */
        if (tp->shutdown || tp->worker_retiring[slot]) {
            /* queue 는 안 건드림 — shutdown 은 free_jobs 가, retire 는 다른
             * worker 가 처리. */
            pthread_mutex_unlock(&tp->mutex);
            break;
        }

        job = tp->head;
        tp->head = job->next;
        if (tp->head == NULL) {
            tp->tail = NULL;
        }
        tp->queue_depth--;
        tp->active_workers++;
        /* queue 가 비었으면 graceful drain 대기자에게 알림 */
        if (tp->queue_depth == 0) {
            pthread_cond_broadcast(&tp->cond_queue_empty);
        }
        pthread_mutex_unlock(&tp->mutex);

        fn      = job->fn;
        job_arg = job->arg;
        free(job);
        fn(job_arg);

        pthread_mutex_lock(&tp->mutex);
        tp->active_workers--;
        /* active == 0 도 drain 대기자가 관심 있을 수 있음 */
        if (tp->queue_depth == 0 && tp->active_workers == 0) {
            pthread_cond_broadcast(&tp->cond_queue_empty);
        }
        pthread_mutex_unlock(&tp->mutex);
    }

    return NULL;
}

/* ── expand / shrink primitive (호출 전 tp->mutex 가 이미 held) ─────── */

/* 현재 pool 크기를 new_n 까지 늘림. TP_ABS_MAX 로 클램프.
 * 이미 크거나 같으면 no-op. return: 성공한 확장 수 (음수 아님). */
static int threadpool_expand_locked(threadpool_t *tp, int new_n) {
    if (new_n > TP_ABS_MAX) new_n = TP_ABS_MAX;
    if (new_n <= tp->n_workers) return 0;

    int added = 0;
    for (int slot = 0; slot < TP_ABS_MAX && tp->n_workers + added < new_n; slot++) {
        if (tp->worker_alive[slot]) continue;

        worker_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) break;
        ctx->tp   = tp;
        ctx->slot = slot;

        tp->worker_retiring[slot] = 0;
        if (pthread_create(&tp->workers[slot], NULL, threadpool_worker_main, ctx) != 0) {
            free(ctx);
            break;
        }
        tp->worker_alive[slot] = 1;
        added++;
    }

    tp->n_workers += added;
    return added;
}

/* 현재 pool 크기를 new_n 까지 줄임. floor_workers 로 클램프.
 * 호출자는 mutex 를 unlock 한 뒤 returned slot 배열에 대해 pthread_join 해야 함.
 * 최대 TP_STEP 개의 slot 을 반환하므로 호출자가 충분한 버퍼를 제공.
 * return: retire 된 slot 수. */
static int threadpool_mark_retire_locked(threadpool_t *tp, int new_n,
                                         int out_slots[], int out_cap) {
    if (new_n < tp->floor_workers) new_n = tp->floor_workers;
    if (new_n >= tp->n_workers) return 0;

    int n_retire = tp->n_workers - new_n;
    int marked = 0;

    /* 높은 slot 부터 retire (안정성: expand 는 낮은 slot 부터 채우므로) */
    for (int slot = TP_ABS_MAX - 1; slot >= 0 && marked < n_retire && marked < out_cap; slot--) {
        if (tp->worker_alive[slot] && !tp->worker_retiring[slot]) {
            tp->worker_retiring[slot] = 1;
            out_slots[marked++] = slot;
        }
    }

    tp->n_workers -= marked;
    /* 모든 retiring worker 를 깨움 (잠자고 있으면 wake, 작업 중이면 끝나고 체크) */
    pthread_cond_broadcast(&tp->cond_nonempty);

    return marked;
}

/* retire 된 slot 을 join 하고 alive 플래그 해제.
 * mutex 를 hold 해서는 안 됨 (worker 가 exit 하면서 mutex 를 acquire 하므로). */
static void threadpool_join_retired(threadpool_t *tp, const int slots[], int n) {
    for (int i = 0; i < n; i++) {
        int slot = slots[i];
        pthread_join(tp->workers[slot], NULL);
        pthread_mutex_lock(&tp->mutex);
        tp->worker_alive[slot]    = 0;
        tp->worker_retiring[slot] = 0;
        pthread_mutex_unlock(&tp->mutex);
    }
}

/* ── Monitor 스레드 ────────────────────────────────────────────────────── */

static void *threadpool_monitor_main(void *arg) {
    threadpool_t *tp = (threadpool_t *)arg;

    for (;;) {
        struct timespec req = { TP_SAMPLE_MS / 1000, (TP_SAMPLE_MS % 1000) * 1000000L };
        nanosleep(&req, NULL);

        pthread_mutex_lock(&tp->mutex);
        if (tp->monitor_should_exit || tp->shutdown) {
            pthread_mutex_unlock(&tp->mutex);
            break;
        }

        if (tp->skip_samples > 0) {
            tp->skip_samples--;
            pthread_mutex_unlock(&tp->mutex);
            continue;
        }

        /* utilization = active / n_workers (퍼센트로 환산 정수 비교) */
        int cur_n  = tp->n_workers;
        int active = tp->active_workers;
        int util_pct = (cur_n > 0) ? (active * 100 / cur_n) : 0;

        int retire_slots[TP_STEP];
        int n_retired = 0;

        if (util_pct >= TP_HIGH_WATER_PCT) {
            tp->high_consecutive++;
            tp->low_consecutive = 0;
            if (tp->high_consecutive >= TP_HIGH_CONSECUTIVE && cur_n < TP_SOFT_MAX) {
                int target = cur_n + TP_STEP;
                if (target > TP_SOFT_MAX) target = TP_SOFT_MAX;
                (void)threadpool_expand_locked(tp, target);
                tp->skip_samples      = TP_HYSTERESIS_SAMPLES;
                tp->high_consecutive  = 0;
            }
        } else if (util_pct <= TP_LOW_WATER_PCT) {
            tp->low_consecutive++;
            tp->high_consecutive = 0;
            if (tp->low_consecutive >= TP_LOW_CONSECUTIVE && cur_n > tp->floor_workers) {
                int target = cur_n - TP_STEP;
                if (target < tp->floor_workers) target = tp->floor_workers;
                n_retired = threadpool_mark_retire_locked(tp, target,
                                                          retire_slots, TP_STEP);
                tp->skip_samples     = TP_HYSTERESIS_SAMPLES;
                tp->low_consecutive  = 0;
            }
        } else {
            /* 중간대: 양쪽 카운터 모두 리셋 */
            tp->high_consecutive = 0;
            tp->low_consecutive  = 0;
        }

        pthread_mutex_unlock(&tp->mutex);

        /* 축소가 있었다면 mutex 밖에서 join (worker exit 시 mutex 재취득하므로) */
        if (n_retired > 0) {
            threadpool_join_retired(tp, retire_slots, n_retired);
        }
    }

    return NULL;
}

/* ── 공개 API ──────────────────────────────────────────────────────────── */

threadpool_t *threadpool_create(int n_workers) {
    threadpool_t *tp;

    if (n_workers <= 0 || n_workers > TP_ABS_MAX) {
        return NULL;
    }

    tp = calloc(1, sizeof(*tp));
    if (tp == NULL) return NULL;

    if (pthread_mutex_init(&tp->mutex, NULL) != 0) {
        free(tp);
        return NULL;
    }
    if (pthread_cond_init(&tp->cond_nonempty, NULL) != 0) {
        pthread_mutex_destroy(&tp->mutex);
        free(tp);
        return NULL;
    }
    if (pthread_cond_init(&tp->cond_queue_empty, NULL) != 0) {
        pthread_cond_destroy(&tp->cond_nonempty);
        pthread_mutex_destroy(&tp->mutex);
        free(tp);
        return NULL;
    }

    tp->floor_workers = n_workers;
    tp->n_workers     = 0;   /* expand_locked 가 증가시킴 */

    pthread_mutex_lock(&tp->mutex);
    int added = threadpool_expand_locked(tp, n_workers);
    pthread_mutex_unlock(&tp->mutex);

    if (added != n_workers) {
        /* 부분 기동 실패 — 정리 후 NULL */
        threadpool_shutdown(tp);
        return NULL;
    }

    /* Monitor 스레드 시작 */
    if (pthread_create(&tp->monitor, NULL, threadpool_monitor_main, tp) != 0) {
        threadpool_shutdown(tp);
        return NULL;
    }
    tp->monitor_started = 1;

    return tp;
}

int threadpool_submit(threadpool_t *tp, threadpool_job_fn fn, void *arg) {
    threadpool_job_t *job;

    if (tp == NULL || fn == NULL) return -1;

    job = calloc(1, sizeof(*job));
    if (job == NULL) return -1;
    job->fn  = fn;
    job->arg = arg;

    pthread_mutex_lock(&tp->mutex);
    if (tp->shutdown || tp->submit_closed) {
        pthread_mutex_unlock(&tp->mutex);
        free(job);
        return -1;
    }

    if (tp->tail != NULL) tp->tail->next = job;
    else                  tp->head       = job;
    tp->tail = job;
    tp->queue_depth++;
    pthread_cond_signal(&tp->cond_nonempty);
    pthread_mutex_unlock(&tp->mutex);

    return 0;
}

/* monitor 스레드를 정지. mutex 는 호출 전/후 모두 unlocked 상태여야 함. */
static void threadpool_stop_monitor(threadpool_t *tp) {
    if (!tp->monitor_started) return;

    pthread_mutex_lock(&tp->mutex);
    tp->monitor_should_exit = 1;
    pthread_mutex_unlock(&tp->mutex);

    /* monitor 는 nanosleep 에 걸려있을 수 있음 — 최악 TP_SAMPLE_MS 대기 */
    pthread_join(tp->monitor, NULL);
    tp->monitor_started = 0;
}

void threadpool_shutdown(threadpool_t *tp) {
    if (tp == NULL) return;

    threadpool_stop_monitor(tp);

    pthread_mutex_lock(&tp->mutex);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->cond_nonempty);
    pthread_cond_broadcast(&tp->cond_queue_empty);
    pthread_mutex_unlock(&tp->mutex);

    /* 모든 alive worker join */
    for (int slot = 0; slot < TP_ABS_MAX; slot++) {
        int alive;
        pthread_mutex_lock(&tp->mutex);
        alive = tp->worker_alive[slot];
        pthread_mutex_unlock(&tp->mutex);
        if (alive) {
            pthread_join(tp->workers[slot], NULL);
            pthread_mutex_lock(&tp->mutex);
            tp->worker_alive[slot] = 0;
            pthread_mutex_unlock(&tp->mutex);
        }
    }

    threadpool_free_jobs(tp);
    pthread_cond_destroy(&tp->cond_queue_empty);
    pthread_cond_destroy(&tp->cond_nonempty);
    pthread_mutex_destroy(&tp->mutex);
    free(tp);
}

int threadpool_shutdown_graceful(threadpool_t *tp, int timeout_ms) {
    if (tp == NULL) return -1;

    /* submit 거부 플래그를 **먼저** 세팅. monitor 정지는 최대 TP_SAMPLE_MS
     * 까지 걸리므로, 이 사이에 들어오는 submit 은 이미 거부되어야 한다. */
    pthread_mutex_lock(&tp->mutex);
    tp->submit_closed = 1;
    pthread_mutex_unlock(&tp->mutex);

    /* Monitor 정지 (auto-resize 가 drain 중 들어오는 것 방지) */
    threadpool_stop_monitor(tp);

    pthread_mutex_lock(&tp->mutex);

    /* timeout 까지 queue drain 대기 */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    add_ms_to_timespec(&deadline, timeout_ms);

    int timed_out = 0;
    while (tp->queue_depth > 0 || tp->active_workers > 0) {
        int rc = pthread_cond_timedwait(&tp->cond_queue_empty, &tp->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            timed_out = 1;
            break;
        }
    }
    pthread_mutex_unlock(&tp->mutex);

    /* 정상 shutdown 경로로 정리 (남은 job 은 shutdown 이 free) */
    threadpool_shutdown(tp);
    return timed_out ? -1 : 0;
}

int threadpool_resize(threadpool_t *tp, int new_n_workers) {
    if (tp == NULL) return -1;
    if (new_n_workers <= 0) return -1;

    int retire_slots[TP_ABS_MAX];
    int n_retired = 0;

    pthread_mutex_lock(&tp->mutex);
    if (tp->shutdown || tp->submit_closed) {
        pthread_mutex_unlock(&tp->mutex);
        return -1;
    }

    /* 경계 클램프 */
    if (new_n_workers > TP_SOFT_MAX)        new_n_workers = TP_SOFT_MAX;
    if (new_n_workers < tp->floor_workers)  new_n_workers = tp->floor_workers;

    if (new_n_workers > tp->n_workers) {
        (void)threadpool_expand_locked(tp, new_n_workers);
    } else if (new_n_workers < tp->n_workers) {
        n_retired = threadpool_mark_retire_locked(tp, new_n_workers,
                                                   retire_slots, TP_ABS_MAX);
    }
    pthread_mutex_unlock(&tp->mutex);

    if (n_retired > 0) {
        threadpool_join_retired(tp, retire_slots, n_retired);
    }

    return 0;
}

double threadpool_get_utilization(const threadpool_t *tp) {
    if (tp == NULL) return 0.0;

    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    int cur_n  = tp->n_workers;
    int active = tp->active_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);

    double u = (cur_n > 0) ? ((double)active / (double)cur_n) : 0.0;
    /* shrink 직후 퇴장 대기 worker 가 진행 중 job 을 처리하는 짧은 윈도우에서
     * active > n_workers 가 되어 1.0 을 넘을 수 있음. 계약 (0.0~1.0) 유지. */
    if (u > 1.0) u = 1.0;
    return u;
}

int threadpool_active_workers(const threadpool_t *tp) {
    int active;
    if (tp == NULL) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    active = tp->active_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);
    return active;
}

int threadpool_queue_depth(const threadpool_t *tp) {
    int depth;
    if (tp == NULL) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    depth = tp->queue_depth;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);
    return depth;
}

int threadpool_total_workers(const threadpool_t *tp) {
    int total;
    if (tp == NULL) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    total = tp->n_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);
    return total;
}
