/* threadpool.c — 고정 크기 worker pool (stub, 승진 담당)
 * ============================================================================
 * MP0 링크 통과용 stub. 실제 구현: feature/threadpool-stats 브랜치.
 *
 * 구현 가이드:
 *   - struct threadpool { pthread_mutex_t; pthread_cond_t cond_nonempty /
 *     cond_nonfull; circular queue or linked list of jobs; worker 배열;
 *     shutdown flag; active_workers atomic counter; }
 *   - threadpool_create: worker N 개 pthread_create, 각자 consume loop
 *   - threadpool_submit: queue enqueue + cond_signal. shutdown 중이면 -1
 *   - threadpool_shutdown: shutdown=true → broadcast → 모든 worker join →
 *     자원 해제
 *   - active_workers / queue_depth: 모니터링용, 약한 일관성 허용
 * ============================================================================
 */

#include "threadpool.h"

#include <stdlib.h>

struct threadpool {
    int dummy;
};

threadpool_t *threadpool_create(int n_workers) {
    (void)n_workers;
    threadpool_t *tp = calloc(1, sizeof(*tp));
    return tp;
}

int threadpool_submit(threadpool_t *tp, threadpool_job_fn fn, void *arg) {
    (void)tp;
    (void)fn;
    (void)arg;
    return -1;
}

void threadpool_shutdown(threadpool_t *tp) {
    free(tp);
}

int threadpool_active_workers(const threadpool_t *tp) {
    (void)tp;
    return 0;
}

int threadpool_queue_depth(const threadpool_t *tp) {
    (void)tp;
    return 0;
}
