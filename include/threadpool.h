/* threadpool.h — 고정 크기 + 동적 확장 worker pool (blocking job queue)
 * ============================================================================
 * 담당: 승진 (Round 1 core) + 지용 (Round 2 dynamic resize + graceful shutdown)
 *
 * 역할:
 *   - N 개 worker thread 생성, mutex + condvar 기반 blocking queue 운용
 *   - 제출된 job 을 FIFO 로 꺼내 실행
 *   - 내장 monitor 스레드가 사용률을 샘플링해 자동 +4 확장 / -4 축소
 *     · 확장 트리거: utilization ≥ 0.8 이 3 회 연속 (3 초)
 *     · 축소 트리거: utilization ≤ 0.3 이 30 회 연속 (30 초)
 *     · 상한 16, 하한 (초기 생성값)
 *     · resize 직후 2 초 hysteresis (flapping 방지)
 *   - graceful shutdown 시 submit 거부 → queue drain 대기 (timeout 까지)
 *     → worker join. timeout 초과 시 강제 종료 경로
 *
 * 사용 예:
 *   threadpool_t *tp = threadpool_create(4);     // 초기 4 worker, cap 16 까지 자동 확장
 *   threadpool_submit(tp, handle_conn, (void *)(intptr_t)fd);
 *   ...
 *   threadpool_shutdown_graceful(tp, 5000);      // 5 초 drain 허용
 *   // 또는 기존 방식:
 *   // threadpool_shutdown(tp);                   // 즉시 종료
 *
 * 주의:
 *   - threadpool_shutdown* 호출 후 submit 은 -1 반환
 *   - active_workers / queue_depth / utilization 은 모니터링용, 약한 일관성
 *   - threadpool_resize 는 양수/음수 차이 모두 지원. 상한 (16) 초과 / 하한
 *     (초기값) 미만 요청은 해당 경계로 클램프
 * ============================================================================
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef struct threadpool threadpool_t;

/* job 함수 시그니처. arg 소유권은 job 함수가 받아 처리한다. */
typedef void (*threadpool_job_fn)(void *arg);

/* n_workers 개 worker thread 를 즉시 기동. 실패 시 NULL.
 * Monitor 스레드도 함께 시작되어 자동 resize 가 활성화된다.
 * n_workers 는 하한 (floor) 으로 동작 — 자동 축소 시 이 값 아래로는 줄지 않음. */
threadpool_t *threadpool_create(int n_workers);

/* job 을 queue 에 enqueue. 성공 0, shutdown 중이거나 실패 -1. */
int  threadpool_submit(threadpool_t *tp, threadpool_job_fn fn, void *arg);

/* 즉시 종료: queue drain 없이 shutdown 플래그 → worker join.
 * 큐에 남은 job 은 실행되지 않고 free 만 됨.
 * 호출 후 tp 포인터는 무효. */
void threadpool_shutdown(threadpool_t *tp);

/* graceful 종료:
 *   1. submit 거부 플래그 세팅 (이후 submit 은 -1)
 *   2. queue_depth == 0 이 될 때까지 대기 (최대 timeout_ms)
 *   3. worker 에게 종료 신호 → join
 * return:  0 = drain 성공 후 정상 종료
 *         -1 = timeout 초과 → 강제 종료 경로 (남은 job drop)
 * 호출 후 tp 포인터는 무효. */
int  threadpool_shutdown_graceful(threadpool_t *tp, int timeout_ms);

/* 동적 resize.
 *   new_n > 현재 → 새 worker N 개 기동 (cap 16 으로 클램프)
 *   new_n < 현재 → 초과 worker 에게 retire 플래그 → broadcast → join (floor 로 클램프)
 *   new_n == 현재 → no-op (0 반환)
 * return: 0 성공, -1 실패 (shutdown 중이거나 argument invalid). */
int  threadpool_resize(threadpool_t *tp, int new_n_workers);

/* 현재 사용률 = active_workers / n_workers. 0.0 ~ 1.0. monitor 용 / 외부 stats 용.
 * 락 없이 스냅샷 읽음 (약한 일관성). */
double threadpool_get_utilization(const threadpool_t *tp);

/* 현재 job 을 실행 중인 worker 수 (0 ~ n_workers). 모니터링용. */
int  threadpool_active_workers(const threadpool_t *tp);

/* queue 에 대기 중인 job 수. 모니터링용. */
int  threadpool_queue_depth(const threadpool_t *tp);

/* 현재 총 worker 수 (확장/축소에 따라 변동). 모니터링용. */
int  threadpool_total_workers(const threadpool_t *tp);

/* 큐 상한 (기본 256). bounded + fail-fast backpressure.
 *   max >  0 : queue_depth >= max 이면 threadpool_submit 이 -1 (reject)
 *   max == 0 : unlimited (legacy 동작, 테스트용)
 *   max <  0 : invalid, 변경 안 함
 * return: 이전 값. */
int  threadpool_set_queue_max(threadpool_t *tp, int max);

/* 현재 큐 상한. 모니터링용. */
int  threadpool_queue_max(const threadpool_t *tp);

/* 큐 상한 초과로 거절된 누적 submit 수. 락 없이 atomic 읽기. */
unsigned long threadpool_rejected_total(const threadpool_t *tp);

#endif /* THREADPOOL_H */
