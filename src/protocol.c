/* protocol.c — HTTP 파서/직렬화 (stub, 용 형님 담당)
 * ============================================================================
 * MP0 링크 통과용 stub. 실제 구현: feature/server-protocol 브랜치.
 *
 * 구현 범위 가이드:
 *   - GET / POST 만
 *   - Content-Length 기반 body 읽기 (chunked 불필요)
 *   - Keep-alive 미지원 (worker 가 연결 닫고 종료)
 *   - 응답: status line + 최소 헤더 (Content-Type, Content-Length,
 *           Connection: close) + body
 * ============================================================================
 */

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

int http_parse_request(int fd, http_request_t *out) {
    (void)fd;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    return -1;  /* stub: 파싱 실패로 간주 */
}

int http_write_response(int fd, const http_response_t *resp) {
    (void)fd;
    (void)resp;
    return -1;
}

void http_request_free(http_request_t *req) {
    if (!req) return;
    free(req->body);
    req->body = NULL;
}
