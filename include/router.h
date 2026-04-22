/* router.h — method + path → handler 디스패치
 * ============================================================================
 * 담당: 용 형님  (src/router.c)
 *
 * 역할:
 *   - parsed HTTP request 를 받아 적절한 handler 로 분기
 *   - 내부 라우트:
 *       POST /api/query     → engine_exec_sql
 *       POST /api/inject    → 더미 데이터 주입
 *       GET  /api/stats     → stats JSON
 *       GET  /api/explain   → engine_explain
 *       GET  /(...)          → 정적 파일 서빙 (--web-root 하위)
 *   - 매칭 실패 시 resp->status = 404 로 채움
 *
 * 메모리 계약:
 *   - resp->body 는 라우터 내부에서 malloc. 호출자(서버 write 경로)가
 *     write 후 free 해야 함. 정적 파일의 경우에도 동일.
 *   - req 는 read-only 로 취급.
 *
 * 스레드 안전성:
 *   - worker 당 독립 (req, resp) 쌍을 사용하므로 stateless.
 *   - 내부적으로 engine.c 의 thread-safe API 만 호출.
 * ============================================================================
 */

#ifndef ROUTER_H
#define ROUTER_H

#include "protocol.h"

/* req 를 파싱해 resp 에 결과를 채움.
 * 성공 0, 내부 오류 -1 (resp->status 는 500 으로 설정). */
int router_dispatch(const http_request_t *req, http_response_t *resp);

#endif /* ROUTER_H */
