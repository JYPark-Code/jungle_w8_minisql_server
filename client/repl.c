/* client/repl.c — minisqld REPL 클라이언트 (ANSI) (지용)
 * ============================================================================
 *
 * 목적:
 *   ./minisqld 에 HTTP 로 SQL 을 날리는 CLI. 발표 데모 시 브라우저 문제
 *   발생하면 이 REPL 로 백업 시연. 평상시에는 개발 중 파싱/실행 빠른 확인용.
 *
 * 사용:
 *   ./minisqld-repl                       # localhost:8080 접속
 *   ./minisqld-repl --host 127.0.0.1 --port 8080
 *
 * 내장 명령:
 *   \q            — 종료
 *   \h            — 도움말
 *   \s            — /api/stats 조회 (active workers, qps, lock wait)
 *   \e <SQL>      — /api/explain?sql=... (인덱스 사용 / 노드 visit 수)
 *   \single on    — 이후 모든 쿼리에 ?mode=single (직렬화 baseline)
 *   \single off   — 다시 멀티 모드
 *
 * 구현 메모:
 *   - 외부 의존 0: socket + read/write 만 사용 (프로젝트 규약 준수)
 *   - Connection: close 로 요청당 1 TCP 연결 (서버가 HTTP keep-alive 미지원)
 *   - 응답은 status line + 헤더 + body 로 분리하고 body 만 출력
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── ANSI ─────────────────────────────────────────────────────── */
#define A_RESET   "\x1b[0m"
#define A_BOLD    "\x1b[1m"
#define A_DIM     "\x1b[2m"
#define A_RED     "\x1b[31m"
#define A_GREEN   "\x1b[32m"
#define A_YELLOW  "\x1b[33m"
#define A_CYAN    "\x1b[36m"

#define MAX_LINE      4096
#define RESP_CAP      (1 << 20)   /* 1 MiB 상한 */

typedef struct {
    char host[128];
    int  port;
    bool single_mode;
} repl_cfg_t;

/* ── TCP / HTTP ───────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port) {
    char port_s[16];
    snprintf(port_s, sizeof port_s, "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_s, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static ssize_t write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

/* fd 에서 Connection: close 기준으로 EOF 까지 전부 읽어 heap 반환.
 * out_len 에 총 길이. 실패 시 NULL. */
static char *read_all(int fd, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    while (len < RESP_CAP) {
        if (cap - len < 1024) {
            size_t new_cap = cap * 2;
            if (new_cap > RESP_CAP) new_cap = RESP_CAP;
            char *nb = realloc(buf, new_cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = new_cap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }
        if (r == 0) break;   /* EOF */
        len += (size_t)r;
    }
    if (out_len) *out_len = len;
    return buf;
}

/* "\r\n\r\n" 수동 검색. POSIX 만 써야 해서 memmem(GNU) 미사용. */
static const char *find_header_body_sep(const char *buf, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

/* HTTP status line + 헤더 → body 포인터만 리턴. out_status 에 3자리 코드. */
static const char *parse_response(const char *resp, size_t len, int *out_status) {
    if (out_status) *out_status = 0;
    if (len < 12 || memcmp(resp, "HTTP/1.", 7) != 0) return NULL;

    /* status code: "HTTP/1.1 200 OK\r\n" */
    int status = 0;
    if (sscanf(resp + 9, "%d", &status) != 1) return NULL;
    if (out_status) *out_status = status;

    /* body 시작은 "\r\n\r\n" 뒤 */
    const char *sep = find_header_body_sep(resp, len);
    if (!sep) return NULL;
    return sep + 4;
}

/* HTTP 요청 1건 송신. body 가 NULL 이면 GET, 있으면 POST. */
static char *http_request(const repl_cfg_t *cfg, const char *method,
                          const char *path, const char *body,
                          int *out_status, size_t *out_resp_len) {
    int fd = tcp_connect(cfg->host, cfg->port);
    if (fd < 0) return NULL;

    char req[MAX_LINE + 512];
    size_t body_len = body ? strlen(body) : 0;
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     method, path, cfg->host, cfg->port, body_len);
    if (n < 0 || n >= (int)sizeof req) { close(fd); return NULL; }

    if (write_all(fd, req, (size_t)n) < 0)              { close(fd); return NULL; }
    if (body_len && write_all(fd, body, body_len) < 0)  { close(fd); return NULL; }

    size_t rlen = 0;
    char *resp = read_all(fd, &rlen);
    close(fd);
    if (out_resp_len) *out_resp_len = rlen;
    if (!resp) { if (out_status) *out_status = 0; return NULL; }

    (void)parse_response(resp, rlen, out_status);   /* status 채움, 본문은 호출자에서 다시 찾음 */
    return resp;
}

/* 응답 출력: 색상 + body 분리. */
static void print_response(const char *resp, size_t rlen, int status) {
    if (!resp) {
        printf(A_RED "× 연결 실패 (서버 실행 중?): %s\n" A_RESET, strerror(errno));
        return;
    }

    int parsed = 0;
    const char *body = parse_response(resp, rlen, &parsed);
    if (!body) {
        printf(A_RED "× 응답 파싱 실패 (status=%d)\n" A_RESET, status);
        return;
    }

    const char *color = (status >= 200 && status < 300) ? A_GREEN
                       : (status >= 400)                ? A_RED
                       : A_YELLOW;
    size_t body_len = rlen - (size_t)(body - resp);

    printf(A_DIM "[%d]" A_RESET " %s%.*s%s\n",
           status, color, (int)body_len, body, A_RESET);
}

/* ── 내장 명령 ────────────────────────────────────────────────── */

static void cmd_help(void) {
    printf(A_BOLD "명령\n" A_RESET);
    printf("  <SQL>             → POST /api/query%s\n",
           "");
    printf("  \\q                → 종료\n");
    printf("  \\h                → 이 도움말\n");
    printf("  \\s                → GET /api/stats (workers/qps/lock_wait)\n");
    printf("  \\e <SQL>          → GET /api/explain?sql=... (인덱스 사용/노드 visit)\n");
    printf("  \\single on|off    → ?mode=single 토글 (직렬화 baseline)\n");
}

static void cmd_stats(const repl_cfg_t *cfg) {
    int status = 0;
    size_t rlen = 0;
    char *resp = http_request(cfg, "GET", "/api/stats", NULL, &status, &rlen);
    print_response(resp, rlen, status);
    free(resp);
}

/* URL-escape 최소: 공백 → %20, 제어문자/> 는 그냥 drop. (쿼리 대부분은 ASCII) */
static void url_escape_min(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else if (c < 0x20 || c == '#' || c == '&' || c == '?' || c == '=' || c == '+') {
            out[o++] = '%';
            static const char hex[] = "0123456789ABCDEF";
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void cmd_explain(const repl_cfg_t *cfg, const char *sql) {
    char encoded[MAX_LINE * 3];
    url_escape_min(sql, encoded, sizeof encoded);

    char path[MAX_LINE * 3 + 64];
    snprintf(path, sizeof path, "/api/explain?sql=%s", encoded);

    int status = 0;
    size_t rlen = 0;
    char *resp = http_request(cfg, "GET", path, NULL, &status, &rlen);
    print_response(resp, rlen, status);
    free(resp);
}

static void cmd_query(const repl_cfg_t *cfg, const char *sql) {
    const char *path = cfg->single_mode ? "/api/query?mode=single" : "/api/query";
    int status = 0;
    size_t rlen = 0;
    char *resp = http_request(cfg, "POST", path, sql, &status, &rlen);
    print_response(resp, rlen, status);
    free(resp);
}

/* 반환: 1 = 내장 명령 처리됨, 0 = SQL 로 취급, -1 = 종료 요청 */
static int handle_builtin(repl_cfg_t *cfg, char *line) {
    if (line[0] != '\\') return 0;

    if (strcmp(line, "\\q") == 0 || strcmp(line, "\\quit") == 0) return -1;
    if (strcmp(line, "\\h") == 0 || strcmp(line, "\\help") == 0) { cmd_help(); return 1; }
    if (strcmp(line, "\\s") == 0 || strcmp(line, "\\stats") == 0) { cmd_stats(cfg); return 1; }

    if (strncmp(line, "\\e ", 3) == 0) {
        cmd_explain(cfg, line + 3);
        return 1;
    }
    if (strncmp(line, "\\single", 7) == 0) {
        const char *arg = line + 7;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0)  { cfg->single_mode = true;  printf(A_YELLOW "→ single mode ON\n" A_RESET); return 1; }
        if (strcmp(arg, "off") == 0) { cfg->single_mode = false; printf(A_CYAN   "→ multi mode\n"   A_RESET); return 1; }
        printf(A_RED "usage: \\single on|off\n" A_RESET);
        return 1;
    }

    printf(A_RED "unknown command: %s (\\h for help)\n" A_RESET, line);
    return 1;
}

/* ── main ─────────────────────────────────────────────────────── */

static void print_help_cli(const char *prog) {
    printf("minisqld REPL — HTTP SQL client\n\n"
           "사용: %s [--host HOST] [--port N]\n"
           "기본: --host localhost --port 8080\n"
           "인터랙티브 모드에서 \\h 치면 명령 목록.\n", prog);
}

int main(int argc, char **argv) {
    repl_cfg_t cfg;
    strncpy(cfg.host, "localhost", sizeof cfg.host - 1);
    cfg.host[sizeof cfg.host - 1] = '\0';
    cfg.port = 8080;
    cfg.single_mode = false;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
            print_help_cli(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(cfg.host, argv[++i], sizeof cfg.host - 1);
            cfg.host[sizeof cfg.host - 1] = '\0';
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = atoi(argv[++i]);
            if (cfg.port <= 0 || cfg.port > 65535) { cfg.port = 8080; }
        } else {
            fprintf(stderr, "unknown option: %s (try --help)\n", argv[i]);
            return 1;
        }
    }

    printf(A_BOLD "minisqld REPL" A_RESET " " A_DIM "— \\h 도움말 · \\q 종료\n" A_RESET);
    printf(A_DIM "target: %s:%d\n" A_RESET, cfg.host, cfg.port);

    char line[MAX_LINE];
    while (1) {
        printf("%sminisqld%s%s> %s",
               A_CYAN,
               cfg.single_mode ? A_YELLOW "[single]" A_CYAN : "",
               A_RESET, A_RESET);
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {
            printf("\n");
            break;
        }

        /* 오른쪽 공백 / 개행 / 세미콜론 제거 */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' '  || line[len - 1] == '\t' ||
                           line[len - 1] == ';')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        int b = handle_builtin(&cfg, line);
        if (b < 0) break;
        if (b > 0) continue;

        cmd_query(&cfg, line);
    }

    printf(A_DIM "bye.\n" A_RESET);
    return 0;
}
