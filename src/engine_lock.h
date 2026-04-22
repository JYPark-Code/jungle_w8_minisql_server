/* engine_lock.h — 내부 concurrency primitives (지용)
 * ============================================================================
 * 담당: 지용 (PM)  (src/engine_lock.c)
 * 공개 API 아님. engine.c 만 호출.
 *
 * 락 계층:
 *   1. per-table  RW lock  — SELECT = rdlock, INSERT/UPDATE/DELETE = wrlock
 *   2. catalog    RW lock  — CREATE/DROP TABLE 은 wrlock,
 *                            테이블 존재 확인 등 가벼운 조회는 rdlock
 *   3. single     mutex    — ?mode=single 시 전역 직렬화
 *
 * 통계:
 *   - 매 lock 획득 시 대기한 nanosecond 를 atomic 에 누적
 *   - engine_get_stats 가 이 값을 읽어 나감
 * ============================================================================
 */

#ifndef ENGINE_LOCK_H
#define ENGINE_LOCK_H

#include <stdint.h>

int  engine_lock_init(void);
void engine_lock_shutdown(void);

/* per-table RW lock. 테이블은 lazy 등록. table 이름은 copy 됨. */
void engine_lock_table_read(const char *table);
void engine_lock_table_write(const char *table);
void engine_lock_table_release(const char *table);

/* catalog 전역 RW lock. DDL 은 write, 조회는 read. */
void engine_lock_catalog_read(void);
void engine_lock_catalog_write(void);
void engine_lock_catalog_release(void);

/* ?mode=single 토글. enter 부터 exit 까지 전역 직렬화. */
void engine_lock_single_enter(void);
void engine_lock_single_exit(void);

/* 누적된 모든 락 대기 시간 (nanosecond). atomic load. */
uint64_t engine_lock_wait_ns_total(void);

#endif /* ENGINE_LOCK_H */
