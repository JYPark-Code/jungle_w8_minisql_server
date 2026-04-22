/* tests/test_threadpool.c — threadpool 단위 테스트 (승진 담당) */

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; printf("  [PASS] %s\n", msg); } \
    else      { ++g_failed; printf("  [FAIL] %s (line %d)\n", msg, __LINE__); } \
} while (0)

typedef struct counter_ctx {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             completed;
    int             target;
} counter_ctx_t;

typedef struct blocking_ctx {
    pthread_mutex_t mutex;
    pthread_cond_t  started_cond;
    pthread_cond_t  release_cond;
    int             started;
    int             release;
} blocking_ctx_t;

typedef struct order_ctx {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int            *order;
    int             next_index;
    int             target;
} order_ctx_t;

typedef struct order_job_arg {
    order_ctx_t *ctx;
    int          value;
} order_job_arg_t;

typedef struct unique_counter_ctx {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int            *seen;
    int             completed;
    int             target;
    int             producer_failed;
} unique_counter_ctx_t;

typedef struct unique_job_arg {
    unique_counter_ctx_t *ctx;
    int                   id;
} unique_job_arg_t;

typedef struct producer_start_gate {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             ready_count;
    int             producer_count;
    int             released;
} producer_start_gate_t;

typedef struct producer_arg {
    threadpool_t           *tp;
    unique_counter_ctx_t   *ctx;
    producer_start_gate_t  *gate;
    int                     start_id;
    int                     job_count;
} producer_arg_t;

static void counter_ctx_init(counter_ctx_t *ctx, int target) {
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->completed = 0;
    ctx->target = target;
}

static void counter_ctx_destroy(counter_ctx_t *ctx) {
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
}

static void blocking_ctx_init(blocking_ctx_t *ctx) {
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->started_cond, NULL);
    pthread_cond_init(&ctx->release_cond, NULL);
    ctx->started = 0;
    ctx->release = 0;
}

static void blocking_ctx_destroy(blocking_ctx_t *ctx) {
    pthread_cond_destroy(&ctx->release_cond);
    pthread_cond_destroy(&ctx->started_cond);
    pthread_mutex_destroy(&ctx->mutex);
}

static void order_ctx_init(order_ctx_t *ctx, int *order, int target) {
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->order = order;
    ctx->next_index = 0;
    ctx->target = target;
}

static void order_ctx_destroy(order_ctx_t *ctx) {
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
}

static void unique_counter_ctx_init(unique_counter_ctx_t *ctx, int *seen, int target) {
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->cond, NULL);
    ctx->seen = seen;
    ctx->completed = 0;
    ctx->target = target;
    ctx->producer_failed = 0;
}

static void unique_counter_ctx_destroy(unique_counter_ctx_t *ctx) {
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
}

static void producer_start_gate_init(producer_start_gate_t *gate, int producer_count) {
    pthread_mutex_init(&gate->mutex, NULL);
    pthread_cond_init(&gate->cond, NULL);
    gate->ready_count = 0;
    gate->producer_count = producer_count;
    gate->released = 0;
}

static void producer_start_gate_destroy(producer_start_gate_t *gate) {
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
}

static void counting_job(void *arg) {
    counter_ctx_t *ctx = (counter_ctx_t *)arg;

    pthread_mutex_lock(&ctx->mutex);
    ctx->completed++;
    if (ctx->completed >= ctx->target) {
        pthread_cond_broadcast(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void blocking_job(void *arg) {
    blocking_ctx_t *ctx = (blocking_ctx_t *)arg;

    pthread_mutex_lock(&ctx->mutex);
    ctx->started++;
    pthread_cond_broadcast(&ctx->started_cond);
    while (!ctx->release) {
        pthread_cond_wait(&ctx->release_cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void ordered_job(void *arg) {
    order_job_arg_t *job = (order_job_arg_t *)arg;
    order_ctx_t *ctx = job->ctx;

    pthread_mutex_lock(&ctx->mutex);
    ctx->order[ctx->next_index++] = job->value;
    if (ctx->next_index >= ctx->target) {
        pthread_cond_broadcast(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->mutex);

    free(job);
}

static void unique_counting_job(void *arg) {
    unique_job_arg_t *job = (unique_job_arg_t *)arg;
    unique_counter_ctx_t *ctx = job->ctx;

    pthread_mutex_lock(&ctx->mutex);
    ctx->seen[job->id]++;
    ctx->completed++;
    if (ctx->completed >= ctx->target) {
        pthread_cond_broadcast(&ctx->cond);
    }
    pthread_mutex_unlock(&ctx->mutex);

    free(job);
}

static void wait_for_counter(counter_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->completed < ctx->target) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void wait_for_blocked_workers(blocking_ctx_t *ctx, int target_started) {
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->started < target_started) {
        pthread_cond_wait(&ctx->started_cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void release_blocked_workers(blocking_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->release = 1;
    pthread_cond_broadcast(&ctx->release_cond);
    pthread_mutex_unlock(&ctx->mutex);
}

static void wait_for_order(order_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->next_index < ctx->target) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void wait_for_unique_counter(unique_counter_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->mutex);
    while (ctx->completed < ctx->target) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
}

static void *producer_main(void *arg) {
    producer_arg_t *producer = (producer_arg_t *)arg;

    pthread_mutex_lock(&producer->gate->mutex);
    producer->gate->ready_count++;
    pthread_cond_broadcast(&producer->gate->cond);
    while (!producer->gate->released) {
        pthread_cond_wait(&producer->gate->cond, &producer->gate->mutex);
    }
    pthread_mutex_unlock(&producer->gate->mutex);

    for (int i = 0; i < producer->job_count; i++) {
        unique_job_arg_t *job = malloc(sizeof(*job));
        if (job == NULL) {
            pthread_mutex_lock(&producer->ctx->mutex);
            producer->ctx->producer_failed = 1;
            pthread_cond_broadcast(&producer->ctx->cond);
            pthread_mutex_unlock(&producer->ctx->mutex);
            return NULL;
        }

        job->ctx = producer->ctx;
        job->id = producer->start_id + i;
        if (threadpool_submit(producer->tp, unique_counting_job, job) != 0) {
            free(job);
            pthread_mutex_lock(&producer->ctx->mutex);
            producer->ctx->producer_failed = 1;
            pthread_cond_broadcast(&producer->ctx->cond);
            pthread_mutex_unlock(&producer->ctx->mutex);
            return NULL;
        }
    }

    return NULL;
}

static void test_invalid_worker_count(void) {
    printf("[TEST] worker 수가 0 이하이면 생성 실패\n");
    CHECK(threadpool_create(0) == NULL, "worker=0 -> NULL");
    CHECK(threadpool_create(-1) == NULL, "worker<0 -> NULL");
}

static void test_submit_executes_all_jobs(void) {
    enum { kJobCount = 64 };

    printf("[TEST] submit 한 job 이 모두 실행됨\n");

    threadpool_t *tp = threadpool_create(4);
    counter_ctx_t ctx;
    int submitted = 0;
    int submit_failed = 0;

    CHECK(tp != NULL, "threadpool_create 성공");
    if (tp == NULL) {
        return;
    }

    counter_ctx_init(&ctx, 0);
    for (int i = 0; i < kJobCount; i++) {
        int rc = threadpool_submit(tp, counting_job, &ctx);
        if (rc != 0) {
            submit_failed = 1;
            break;
        }
        submitted++;
    }

    pthread_mutex_lock(&ctx.mutex);
    ctx.target = submitted;
    pthread_mutex_unlock(&ctx.mutex);

    wait_for_counter(&ctx);

    pthread_mutex_lock(&ctx.mutex);
    CHECK(!submit_failed && submitted == kJobCount, "모든 job enqueue 성공");
    CHECK(ctx.completed == submitted, "모든 job 완료");
    pthread_mutex_unlock(&ctx.mutex);

    threadpool_shutdown(tp);
    counter_ctx_destroy(&ctx);
}

static void test_shutdown_drains_pending_jobs(void) {
    enum { kJobCount = 10000 };

    printf("[TEST] shutdown 이 pending queue 를 drain 함\n");

    threadpool_t *tp = threadpool_create(4);
    counter_ctx_t ctx;
    int submitted = 0;
    int submit_failed = 0;

    CHECK(tp != NULL, "threadpool_create 성공");
    if (tp == NULL) {
        return;
    }

    counter_ctx_init(&ctx, 0);
    for (int i = 0; i < kJobCount; i++) {
        int rc = threadpool_submit(tp, counting_job, &ctx);
        if (rc != 0) {
            submit_failed = 1;
            break;
        }
        submitted++;
    }

    pthread_mutex_lock(&ctx.mutex);
    ctx.target = submitted;
    pthread_mutex_unlock(&ctx.mutex);

    threadpool_shutdown(tp);

    pthread_mutex_lock(&ctx.mutex);
    CHECK(!submit_failed && submitted == kJobCount, "drain 대상 job enqueue 성공");
    CHECK(ctx.completed == submitted, "shutdown 이후에도 모든 job 완료");
    pthread_mutex_unlock(&ctx.mutex);

    counter_ctx_destroy(&ctx);
}

static void test_concurrent_enqueue_10k(void) {
    enum {
        kProducerCount = 4,
        kJobsPerProducer = 2500,
        kTotalJobs = kProducerCount * kJobsPerProducer
    };

    printf("[TEST] 여러 producer 가 동시에 10K enqueue\n");

    threadpool_t *tp = threadpool_create(4);
    unique_counter_ctx_t ctx;
    producer_start_gate_t gate;
    pthread_t producers[kProducerCount];
    producer_arg_t producer_args[kProducerCount];
    int *seen = calloc((size_t)kTotalJobs, sizeof(*seen));
    int created = 0;

    CHECK(tp != NULL, "threadpool_create 성공");
    CHECK(seen != NULL, "10K seen 배열 할당 성공");
    if (tp == NULL || seen == NULL) {
        free(seen);
        threadpool_shutdown(tp);
        return;
    }

    unique_counter_ctx_init(&ctx, seen, kTotalJobs);
    producer_start_gate_init(&gate, kProducerCount);

    for (int i = 0; i < kProducerCount; i++) {
        producer_args[i].tp = tp;
        producer_args[i].ctx = &ctx;
        producer_args[i].gate = &gate;
        producer_args[i].start_id = i * kJobsPerProducer;
        producer_args[i].job_count = kJobsPerProducer;

        if (pthread_create(&producers[i], NULL, producer_main, &producer_args[i]) != 0) {
            CHECK(0, "producer thread 생성 성공");
            pthread_mutex_lock(&ctx.mutex);
            ctx.producer_failed = 1;
            pthread_mutex_unlock(&ctx.mutex);
            break;
        }
        created++;
    }

    pthread_mutex_lock(&gate.mutex);
    while (gate.ready_count < created) {
        pthread_cond_wait(&gate.cond, &gate.mutex);
    }
    gate.released = 1;
    pthread_cond_broadcast(&gate.cond);
    pthread_mutex_unlock(&gate.mutex);

    for (int i = 0; i < created; i++) {
        pthread_join(producers[i], NULL);
    }

    pthread_mutex_lock(&ctx.mutex);
    CHECK(!ctx.producer_failed && created == kProducerCount,
          "4 producer 가 모두 10K enqueue 완료");
    pthread_mutex_unlock(&ctx.mutex);

    if (!ctx.producer_failed && created == kProducerCount) {
        wait_for_unique_counter(&ctx);

        pthread_mutex_lock(&ctx.mutex);
        CHECK(ctx.completed == kTotalJobs, "10K job 이 모두 정확히 한 번 실행됨");
        pthread_mutex_unlock(&ctx.mutex);

        for (int i = 0; i < kTotalJobs; i++) {
            if (seen[i] != 1) {
                CHECK(0, "모든 job id 가 정확히 한 번만 관측됨");
                threadpool_shutdown(tp);
                producer_start_gate_destroy(&gate);
                unique_counter_ctx_destroy(&ctx);
                free(seen);
                return;
            }
        }
        CHECK(1, "모든 job id 가 정확히 한 번만 관측됨");
    }

    threadpool_shutdown(tp);
    producer_start_gate_destroy(&gate);
    unique_counter_ctx_destroy(&ctx);
    free(seen);
}

static void test_submit_rejects_invalid_args(void) {
    printf("[TEST] 잘못된 인자 submit 은 실패\n");

    threadpool_t *tp = threadpool_create(1);
    CHECK(tp != NULL, "threadpool_create 성공");
    if (tp == NULL) {
        return;
    }

    CHECK(threadpool_submit(NULL, counting_job, NULL) == -1,
          "NULL pool -> -1");
    CHECK(threadpool_submit(tp, NULL, NULL) == -1,
          "NULL fn -> -1");
    threadpool_shutdown(tp);
}

static void test_metrics_snapshot(void) {
    printf("[TEST] active_workers / queue_depth 스냅샷\n");

    threadpool_t *tp = threadpool_create(2);
    blocking_ctx_t ctx;
    int submitted = 0;

    CHECK(tp != NULL, "threadpool_create 성공");
    if (tp == NULL) {
        return;
    }

    blocking_ctx_init(&ctx);
    for (int i = 0; i < 4; i++) {
        int rc = threadpool_submit(tp, blocking_job, &ctx);
        CHECK(rc == 0, "blocking job enqueue 성공");
        if (rc != 0) {
            break;
        }
        submitted++;
    }

    if (submitted < 4) {
        release_blocked_workers(&ctx);
        threadpool_shutdown(tp);
        blocking_ctx_destroy(&ctx);
        return;
    }

    wait_for_blocked_workers(&ctx, 2);
    CHECK(threadpool_active_workers(tp) == 2, "2 worker 가 실행 중");
    CHECK(threadpool_queue_depth(tp) == 2, "나머지 2 job 은 queue 에 대기");

    release_blocked_workers(&ctx);
    threadpool_shutdown(tp);
    blocking_ctx_destroy(&ctx);
}

static void test_single_worker_fifo(void) {
    enum { kJobCount = 5 };

    printf("[TEST] single worker 에서 FIFO 유지\n");

    threadpool_t *tp = threadpool_create(1);
    order_ctx_t ctx;
    int observed[kJobCount] = {0};
    int submitted = 0;

    CHECK(tp != NULL, "threadpool_create 성공");
    if (tp == NULL) {
        return;
    }

    order_ctx_init(&ctx, observed, kJobCount);
    for (int i = 0; i < kJobCount; i++) {
        order_job_arg_t *job = malloc(sizeof(*job));
        CHECK(job != NULL, "ordered job arg 할당 성공");
        if (job == NULL) {
            continue;
        }

        job->ctx = &ctx;
        job->value = i + 1;
        if (threadpool_submit(tp, ordered_job, job) != 0) {
            CHECK(0, "ordered job enqueue 성공");
            free(job);
            break;
        }

        CHECK(1, "ordered job enqueue 성공");
        submitted++;
    }

    if (submitted < kJobCount) {
        threadpool_shutdown(tp);
        order_ctx_destroy(&ctx);
        return;
    }

    wait_for_order(&ctx);
    for (int i = 0; i < kJobCount; i++) {
        char message[64];
        snprintf(message, sizeof(message), "FIFO 순서 유지 #%d", i + 1);
        CHECK(observed[i] == i + 1, message);
    }

    threadpool_shutdown(tp);
    order_ctx_destroy(&ctx);
}

int main(void) {
    printf("=== test_threadpool ===\n");

    test_invalid_worker_count();
    test_submit_executes_all_jobs();
    test_shutdown_drains_pending_jobs();
    test_concurrent_enqueue_10k();
    test_submit_rejects_invalid_args();
    test_metrics_snapshot();
    test_single_worker_fifo();

    printf("\n[THREADPOOL TESTS] %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
