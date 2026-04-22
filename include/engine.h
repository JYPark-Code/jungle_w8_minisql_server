/* engine.h — SQL 실행 진입점 (W7 엔진 thread-safe wrapping)
 * ============================================================================
 * 담당: 동현  (src/engine.c)
 *
 * 역할:
 *   - W7 의 parser/executor/storage/bptree 를 thread-safe 하게 호출
 *   - 테이블 RW lock + 글로벌 catalog lock 하에 SQL 실행
 *   - mode=single 토글 시 전역 mutex 로 직렬화 (비교 baseline)
 *   - EXPLAIN (인덱스 사용 여부, 방문 노드 수, lock wait) 지원
 *   - atomic 통계 수집 (total queries, lock wait ns)
 *
 * 메모리 계약:
 *   - engine_exec_sql / engine_explain 이 반환하는 engine_result_t 의
 *     json 필드는 호출자가 engine_result_free 로 해제해야 한다.
 *   - engine_result_free 는 NULL safe.
 *
 * 스레드 안전성:
 *   - engine_init / engine_shutdown 은 단일 스레드에서만 호출.
 *   - engine_exec_sql / engine_explain / engine_get_stats 는 스레드 안전.
 * ============================================================================
 */

#ifndef ENGINE_H
#define ENGINE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct engine_result {
    bool    ok;                 /* 쿼리 성공 여부 */
    char   *json;               /* 결과 JSON (호출자가 free) */
    double  elapsed_ms;         /* 쿼리 총 소요 (ms) */
    bool    index_used;         /* B+Tree 인덱스 사용 여부 */
    int     nodes_visited;      /* EXPLAIN 용, 인덱스 조회 시 방문 노드 수 */
    double  lock_wait_ms;       /* 락 대기 시간 (ms) */
} engine_result_t;

/* 엔진 초기화. data_dir 하의 CSV / 인덱스 로드, 락 구조 초기화.
 * 성공 0, 실패 비 0. 단일 스레드에서 1회만 호출. */
int  engine_init(const char *data_dir);

/* SQL 실행. single_mode=true 면 전역 mutex 로 강제 직렬화. */
engine_result_t engine_exec_sql(const char *sql, bool single_mode);

/* 실행 계획 조회 (SELECT 에 대해서만 의미 있음).
 * result.json 은 {"index_used":..,"nodes_visited":..} 형태. */
engine_result_t engine_explain(const char *sql);

/* 누적 통계 조회 (atomic load). out 포인터 NULL 허용. */
void engine_get_stats(uint64_t *total_queries,
                      uint64_t *lock_wait_ns_total);

/* 엔진 종료. in-flight 쿼리 drain 후 락/리소스 해제.
 * 단일 스레드에서 1회만 호출. */
void engine_shutdown(void);

/* engine_result_t 내부의 json 버퍼 해제. r 자체는 stack/heap 무관. */
void engine_result_free(engine_result_t *r);

/* dictionary 관련 인덱스 (trie) 가 준비됐는지 여부.
 *   - engine_init 후 dictionary 테이블이 존재하면 trie 를 rebuild 한 뒤 true
 *   - 테이블이 없거나 rebuild 실패한 경우 false
 *   - write 경로 중 잠시 rebuild 동안 false 가 될 수 있음
 * router 가 /api/dict, /api/autocomplete, /api/admin/insert 같은 사전 전용
 * 엔드포인트 진입 시 이 값을 체크해서 false 면 503 warming_up 으로 응답.
 * Atomic load (락 없음). 스레드 안전. */
bool engine_is_ready(void);

#endif /* ENGINE_H */
