/* engine.c — W7 엔진 thread-safe wrapper (stub, 동현 담당)
 * ============================================================================
 * MP0 링크 통과용 stub. 실제 구현: feature/engine-threadsafe 브랜치.
 *
 * 구현 가이드:
 *   1. engine_init: engine_lock_init() + W7 엔진 상태 초기화 (storage 등)
 *   2. engine_exec_sql:
 *        - single_mode 면 engine_lock_single_enter()
 *        - parse_sql → 테이블명/DDL 여부 판정
 *        - DDL 이면 engine_lock_catalog_write()
 *        - SELECT 면 engine_lock_table_read(t), 그 외 DML 은 _write(t)
 *        - execute() 후 결과를 JSON 으로 직렬화
 *        - 모든 경로에서 lock release 보장 (실패 경로 포함)
 *   3. engine_explain: EXPLAIN 용 메타데이터 채움
 *   4. engine_get_stats: engine_lock_wait_ns_total + 내부 atomic
 *   5. engine_shutdown: engine_lock_shutdown() + 캐시/파일 정리
 * ============================================================================
 */

#include "engine.h"
#include "engine_lock.h"

#include <stdlib.h>
#include <string.h>

int engine_init(const char *data_dir) {
    (void)data_dir;
    return engine_lock_init();
}

engine_result_t engine_exec_sql(const char *sql, bool single_mode) {
    (void)sql;
    (void)single_mode;
    engine_result_t r;
    memset(&r, 0, sizeof(r));
    r.ok = false;
    return r;
}

engine_result_t engine_explain(const char *sql) {
    (void)sql;
    engine_result_t r;
    memset(&r, 0, sizeof(r));
    r.ok = false;
    return r;
}

void engine_get_stats(uint64_t *total_queries, uint64_t *lock_wait_ns_total) {
    if (total_queries)       *total_queries       = 0;
    if (lock_wait_ns_total)  *lock_wait_ns_total  = engine_lock_wait_ns_total();
}

void engine_shutdown(void) {
    engine_lock_shutdown();
}

void engine_result_free(engine_result_t *r) {
    if (!r) return;
    free(r->json);
    r->json = NULL;
}
