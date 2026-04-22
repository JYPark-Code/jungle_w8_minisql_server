/* protocol.h — HTTP/1.1 request 파싱 + response 직렬화
 * ============================================================================
 * 담당: 용 형님  (src/protocol.c)
 *
 * 역할:
 *   - 외부 라이브러리 0 으로 HTTP/1.1 최소 기능 직접 구현
 *   - GET/POST + Content-Length + body (text/plain 기준) 파싱
 *   - response status line + header + body 를 fd 에 write
 *   - Keep-alive 미지원 (connection = job 단위, write 후 close)
 *
 * 메모리 계약:
 *   - http_parse_request 가 채운 out->body 는 heap (호출자가 free).
 *   - http_request_free 가 body 를 포함한 모든 동적 필드 해제. NULL safe.
 *   - http_response_t.body 소유권은 호출자에게 있음 (write 는 복사만).
 *
 * 스레드 안전성:
 *   - 모든 함수는 주어진 fd / 구조체에 한해 스레드 안전
 *     (worker 당 독립 request/response 사용 가정).
 * ============================================================================
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

typedef struct http_request {
    char    method[8];          /* "GET", "POST" */
    char    path[512];          /* "/api/query" */
    char    query[512];         /* "mode=single" (query string, '?' 제외) */
    char    content_type[64];   /* "text/plain" 등 */
    size_t  content_length;     /* Content-Length 헤더 값 */
    char   *body;               /* heap, 호출자 free (http_request_free 권장) */
} http_request_t;

typedef struct http_response {
    int         status;         /* 200, 400, 404, 500 등 */
    const char *content_type;   /* "application/json", "text/html" 등 */
    char       *body;           /* 호출자가 관리 (write 는 복사) */
    size_t      body_len;       /* body 바이트 길이 */
} http_response_t;

/* fd 에서 HTTP/1.1 요청 1건을 read 후 out 에 채움.
 * 성공 0, 파싱 실패 -1. 호출자는 out 에 대해 http_request_free 호출 필요. */
int  http_parse_request(int fd, http_request_t *out);

/* resp 를 HTTP 응답으로 직렬화하여 fd 에 write.
 * 성공 0, write 실패 -1. */
int  http_write_response(int fd, const http_response_t *resp);

/* req 내부의 동적 필드 해제. req 자체는 호출자 소유. NULL safe. */
void http_request_free(http_request_t *req);

#endif /* PROTOCOL_H */
