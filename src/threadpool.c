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

#include <pthread.h>
#include <stdlib.h>

typedef struct threadpool_job {
    threadpool_job_fn         fn;
    void                     *arg;
    struct threadpool_job    *next;
} threadpool_job_t;

struct threadpool {
    pthread_mutex_t   mutex;
    pthread_cond_t    cond_nonempty;
    pthread_t        *workers;
    int               n_workers;
    int               workers_started;
    threadpool_job_t *head;
    threadpool_job_t *tail;
    int               queue_depth;
    int               active_workers;
    int               shutdown;
};

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

static void *threadpool_worker_main(void *arg) {
    threadpool_t *tp = (threadpool_t *)arg;

    for (;;) {
        threadpool_job_t *job;
        threadpool_job_fn fn;
        void *job_arg;

        pthread_mutex_lock(&tp->mutex);
        while (tp->head == NULL && !tp->shutdown) {
            pthread_cond_wait(&tp->cond_nonempty, &tp->mutex);
        }

        if (tp->head == NULL && tp->shutdown) {
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
        pthread_mutex_unlock(&tp->mutex);

        fn = job->fn;
        job_arg = job->arg;
        free(job);
        fn(job_arg);

        pthread_mutex_lock(&tp->mutex);
        tp->active_workers--;
        pthread_mutex_unlock(&tp->mutex);
    }

    return NULL;
}

threadpool_t *threadpool_create(int n_workers) {
    threadpool_t *tp;

    if (n_workers <= 0) {
        return NULL;
    }

    tp = calloc(1, sizeof(*tp));
    if (tp == NULL) {
        return NULL;
    }

    tp->workers = calloc((size_t)n_workers, sizeof(*tp->workers));
    if (tp->workers == NULL) {
        free(tp);
        return NULL;
    }

    tp->n_workers = n_workers;

    if (pthread_mutex_init(&tp->mutex, NULL) != 0) {
        free(tp->workers);
        free(tp);
        return NULL;
    }

    if (pthread_cond_init(&tp->cond_nonempty, NULL) != 0) {
        pthread_mutex_destroy(&tp->mutex);
        free(tp->workers);
        free(tp);
        return NULL;
    }

    for (int i = 0; i < n_workers; i++) {
        if (pthread_create(&tp->workers[i], NULL, threadpool_worker_main, tp) != 0) {
            pthread_mutex_lock(&tp->mutex);
            tp->shutdown = 1;
            pthread_cond_broadcast(&tp->cond_nonempty);
            pthread_mutex_unlock(&tp->mutex);

            for (int j = 0; j < tp->workers_started; j++) {
                pthread_join(tp->workers[j], NULL);
            }

            pthread_cond_destroy(&tp->cond_nonempty);
            pthread_mutex_destroy(&tp->mutex);
            free(tp->workers);
            free(tp);
            return NULL;
        }
        tp->workers_started++;
    }

    return tp;
}

int threadpool_submit(threadpool_t *tp, threadpool_job_fn fn, void *arg) {
    threadpool_job_t *job;

    if (tp == NULL || fn == NULL) {
        return -1;
    }

    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }

    job->fn = fn;
    job->arg = arg;

    pthread_mutex_lock(&tp->mutex);
    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mutex);
        free(job);
        return -1;
    }

    if (tp->tail != NULL) {
        tp->tail->next = job;
    } else {
        tp->head = job;
    }
    tp->tail = job;
    tp->queue_depth++;
    pthread_cond_signal(&tp->cond_nonempty);
    pthread_mutex_unlock(&tp->mutex);

    return 0;
}

void threadpool_shutdown(threadpool_t *tp) {
    if (tp == NULL) {
        return;
    }

    pthread_mutex_lock(&tp->mutex);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->cond_nonempty);
    pthread_mutex_unlock(&tp->mutex);

    for (int i = 0; i < tp->workers_started; i++) {
        pthread_join(tp->workers[i], NULL);
    }

    threadpool_free_jobs(tp);
    pthread_cond_destroy(&tp->cond_nonempty);
    pthread_mutex_destroy(&tp->mutex);
    free(tp->workers);
    free(tp);
}

int threadpool_active_workers(const threadpool_t *tp) {
    int active_workers;

    if (tp == NULL) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    active_workers = tp->active_workers;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);
    return active_workers;
}

int threadpool_queue_depth(const threadpool_t *tp) {
    int queue_depth;

    if (tp == NULL) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *)&tp->mutex);
    queue_depth = tp->queue_depth;
    pthread_mutex_unlock((pthread_mutex_t *)&tp->mutex);
    return queue_depth;
}
