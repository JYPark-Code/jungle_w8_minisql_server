/* main.c — minisqld 데몬 엔트리 (지용)
 * ============================================================================
 *
 * W8 에서 바뀐 것:
 *   W7 은 파일 1개를 파싱/실행하고 종료하는 일회성 CLI (sqlparser) 였으나,
 *   W8 은 HTTP 서버로 상주하며 여러 요청을 받는 단일 데몬 (minisqld) 이다.
 *   따라서 main.c 의 역할이 완전히 바뀐다:
 *     - CLI 옵션 파싱 (--port / --workers / --data-dir / --web-root)
 *     - SIGINT 핸들러 설치 → server_shutdown() 으로 graceful drain
 *     - server_run(&cfg) 호출 (blocking, shutdown 까지 return X)
 *
 *   engine_init / threadpool_create / accept loop 는 모두 server_run
 *   내부에서 수행된다 (server.h 참조). main.c 는 얇은 진입점만 유지.
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINISQLD_VERSION "0.1.0"

static void print_help(const char *prog) {
    printf(
        "minisqld %s — Multi-threaded Mini DBMS in Pure C\n"
        "\n"
        "사용:\n"
        "  %s [옵션...]\n"
        "\n"
        "옵션:\n"
        "  --port <N>         listen 포트 (기본 8080)\n"
        "  --workers <N>      worker 스레드 수 (기본 8)\n"
        "  --data-dir <path>  CSV/인덱스 디렉토리 (기본 ./data)\n"
        "  --web-root <path>  정적 파일 루트 (기본 ./web)\n"
        "  --help, -h         이 도움말 출력\n"
        "  --version          버전 출력\n"
        "\n"
        "예시:\n"
        "  %s --port 8080 --workers 8 --data-dir ./data --web-root ./web\n",
        MINISQLD_VERSION, prog, prog);
}

static void on_sigint(int sig) {
    (void)sig;
    server_shutdown();   /* signal-safe: atomic flag 만 건드림 */
}

/* 정수 파싱 헬퍼. 실패 시 기본값 유지. */
static int parse_int_arg(const char *s, int fallback) {
    if (!s) return fallback;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return fallback;
    if (v < 1 || v > 65535) return fallback;
    return (int)v;
}

int main(int argc, char **argv) {
    server_config_t cfg = {
        .port     = 8080,
        .workers  = 8,
        .data_dir = "./data",
        .web_root = "./web",
    };

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            printf("minisqld %s\n", MINISQLD_VERSION);
            return 0;
        }
        if (strcmp(a, "--port") == 0 && i + 1 < argc) {
            cfg.port = parse_int_arg(argv[++i], cfg.port);
        } else if (strcmp(a, "--workers") == 0 && i + 1 < argc) {
            cfg.workers = parse_int_arg(argv[++i], cfg.workers);
        } else if (strcmp(a, "--data-dir") == 0 && i + 1 < argc) {
            cfg.data_dir = argv[++i];
        } else if (strcmp(a, "--web-root") == 0 && i + 1 < argc) {
            cfg.web_root = argv[++i];
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            fprintf(stderr, "try '%s --help'\n", argv[0]);
            return 1;
        }
    }

    /* SIGINT / SIGTERM 수신 시 graceful shutdown.
     * SA_RESTART 안 거는 이유: accept() 가 EINTR 로 돌아와야 loop 탈출 가능. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* SIGPIPE 무시: 클라이언트가 도중에 연결 끊어도 서버가 죽지 않도록. */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr,
            "minisqld %s — port=%d workers=%d data=%s web=%s\n",
            MINISQLD_VERSION, cfg.port, cfg.workers, cfg.data_dir, cfg.web_root);

    return server_run(&cfg);
}
