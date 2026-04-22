/* router.c — method + path → handler (stub, 용 형님 담당)
 * ============================================================================
 * MP0 링크 통과용 stub. 실제 구현: feature/server-protocol 브랜치.
 *
 * 구현 가이드:
 *   - POST /api/query    → engine_exec_sql(body, single_mode)
 *   - POST /api/inject   → 더미 데이터 주입
 *   - GET  /api/stats    → stats JSON
 *   - GET  /api/explain  → engine_explain(sql)
 *   - GET  /             → 정적 파일 서빙 (cfg->web_root 하위)
 *   - else               → 404
 * ============================================================================
 */

#include "router.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

int router_dispatch(const http_request_t *req, http_response_t *resp) {
    (void)req;
    if (!resp) return -1;
    memset(resp, 0, sizeof(*resp));
    resp->status       = 501;   /* Not Implemented */
    resp->content_type = "text/plain";
    return 0;
}
