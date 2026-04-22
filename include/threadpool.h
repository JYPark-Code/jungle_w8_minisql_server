/* threadpool.h — 고정 크기 worker pool + blocking job queue
 * ============================================================================
 * 담당: 승진  (src/threadpool.c)
 *
 * 역할:
 *   - N 개 worker thread 생성, mutex + condvar 기반 blocking queue 운용
 *   - 제출된 job 을 FIFO 로 꺼내 실행
 *   - shutdown 시 queue drain 후 모든 worker join (누수 방지)
 *   - 실시간 통계 조회 (active workers, queue depth)
 *
 * 사용 예:
 *   threadpool_t *tp = threadpool_create(8);
 *   threadpool_submit(tp, handle_conn, (void *)(intptr_t)fd);
 *   ...
 *   threadpool_shutdown(tp);     // 종료 시점에 반드시 호출
 *
 * 주의:
 *   - threadpool_submit 은 queue 가 가득 차면 block (또는 실패 반환, 구현 선택).
 *   - threadpool_shutdown 호출 후 submit 은 실패 (-1) 반환.
 *   - active_workers / queue_depth 는 모니터링용이므로 약한 일관성 허용.
 * ============================================================================
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef struct threadpool threadpool_t;

/* job 함수 시그니처. arg 소유권은 job 함수가 받아 처리한다. */
typedef void (*threadpool_job_fn)(void *arg);

/* n_workers 개 worker thread 를 즉시 기동. 실패 시 NULL. */
threadpool_t *threadpool_create(int n_workers);

/* job 을 queue 에 enqueue. 성공 0, shutdown 중이거나 실패 -1. */
int  threadpool_submit(threadpool_t *tp, threadpool_job_fn fn, void *arg);

/* queue drain + 모든 worker join. 호출 후 tp 포인터는 무효. */
void threadpool_shutdown(threadpool_t *tp);

/* 현재 job 을 실행 중인 worker 수 (0 ~ n_workers). 모니터링용. */
int  threadpool_active_workers(const threadpool_t *tp);

/* queue 에 대기 중인 job 수. 모니터링용. */
int  threadpool_queue_depth(const threadpool_t *tp);

#endif /* THREADPOOL_H */
