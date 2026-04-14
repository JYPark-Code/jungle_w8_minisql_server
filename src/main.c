/* main.c — sqlparser 프로그램의 시작점 (지용)
 * ============================================================================
 *
 * ▣ 이 파일이 하는 일
 *   터미널에서 ./sqlparser 라고 치면 가장 먼저 실행되는 파일이다.
 *   1) 명령줄 옵션 (--debug, --json, ...) 을 읽고
 *   2) 입력으로 받은 .sql 파일을 통째로 메모리에 올리고
 *   3) 세미콜론(;) 단위로 잘라서 한 statement 씩
 *   4) 파서에 넘겨서 ParsedSQL 로 만든 다음
 *   5) 옵션에 따라 출력하거나 실행한다.
 *
 * ▣ 사용 예
 *   ./sqlparser query.sql                  # 파싱 + 실행
 *   ./sqlparser query.sql --debug          # AST 트리도 같이 출력
 *   ./sqlparser query.sql --json           # 파싱 결과를 JSON 으로
 *   ./sqlparser query.sql --tokens         # 토크나이저 결과만
 *   ./sqlparser query.sql --format         # 정규화된 SQL 로 다시 출력
 *   ./sqlparser --help                     # 사용법 보기
 *   ./sqlparser --version                  # 버전 보기
 * ============================================================================
 */

#define _POSIX_C_SOURCE 200809L

#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     /* dup, dup2 */
#include <fcntl.h>      /* open, O_WRONLY */

#define MINISQL_VERSION "0.1.0"

/* --help 옵션이 들어왔을 때 출력되는 사용법.
 * 사람이 읽기 쉽게 옵션과 예시까지 같이 보여준다. */
static void print_help(const char *prog) {
    printf(
        "MiniSQL %s — 파일 기반 SQL 처리기\n"
        "\n"
        "사용:\n"
        "  %s <file.sql> [옵션...]\n"
        "  %s --help | --version\n"
        "\n"
        "옵션:\n"
        "  --debug      파싱 결과를 AST 트리로 출력\n"
        "  --json       파싱 결과를 JSON 으로 출력\n"
        "  --tokens     토크나이저 출력만 (파싱/실행 안 함)\n"
        "  --format     ParsedSQL → 정규화된 SQL 로 재출력\n"
        "  --help, -h   이 도움말 출력\n"
        "  --version    버전 출력\n"
        "\n"
        "예시:\n"
        "  %s query.sql --debug\n"
        "  %s query.sql --json\n"
        "  %s query.sql --format\n",
        MINISQL_VERSION, prog, prog, prog, prog, prog);
}

/* read_file: 파일 하나를 통째로 메모리에 읽어와서 문자열로 돌려준다.
 *
 * 동작 순서:
 *   1) fopen 으로 열기
 *   2) fseek 로 끝까지 이동 → ftell 로 파일 크기 알아내기
 *   3) fseek 로 다시 처음으로
 *   4) malloc 으로 (크기+1) 만큼 메모리 잡기 (+1은 끝의 '\0' 자리)
 *   5) fread 로 통째로 읽기
 *   6) 마지막에 '\0' 추가해서 정상적인 C 문자열로 만들기
 *
 * 호출자가 free() 로 직접 해제해야 한다. */
static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

/* process_stmt: SQL statement 한 개를 처리한다.
 *
 *   tokens_mode  → 토크나이저 출력만 (파싱/실행 안 함)
 *   debug_mode   → AST 트리 출력
 *   json_mode    → JSON 출력
 *   format_mode  → 정규화 SQL 출력
 *
 * 그 다음 execute() 로 실제 실행 (executor 가 storage 호출).
 *
 * ▣ --json 모드의 출력 순서
 *   1) print_json 이 한 줄짜리 JSON 출력
 *   2) execute() → storage 가 SELECT 결과 표 (header + rows + "(N rows)")
 *      를 stdout 에 출력
 *
 *   server.py 같은 소비자는 한 줄씩 읽으면서 JSON parse 가 성공하면 statement
 *   로, 실패하면 직전 statement 의 결과 행으로 귀속시킨다 (server.py 참고).
 *   덕분에 카드 뷰에서 SELECT 결과 표를 카드 안에 inline 표시할 수 있다.
 */
static void process_stmt(const char *stmt, int debug_mode, int json_mode,
                         int tokens_mode, int format_mode) {
    /* tokens 모드는 토크나이저만 검증하는 모드라 파싱/실행 자체를 건너뛴다. */
    if (tokens_mode) {
        print_tokens(stdout, stmt);
        return;
    }
    ParsedSQL *sql = parse_sql(stmt);
    if (sql && sql->type != QUERY_UNKNOWN) {
        if (debug_mode)  print_ast(stdout, sql);
        if (json_mode)   print_json(stdout, sql);
        if (format_mode) print_format(stdout, sql);
        execute(sql);
    }
    free_parsed(sql);   /* 메모리 누수 방지 */
}

/* run_statements: 파일 전체 (여러 statement 가 ; 로 구분된) 를 처리.
 *
 * 핵심 트릭: 따옴표(' 또는 ") 안의 ; 는 statement 구분자가 아니므로
 * "지금 따옴표 안인지" 를 quote 변수로 추적한다.
 *
 *   "INSERT INTO t VALUES ('hello;world')" 같은 문장이 있을 때
 *   따옴표 안의 ; 를 statement 끝으로 오해하면 안 된다. */
static void run_statements(const char *src, int debug_mode, int json_mode,
                           int tokens_mode, int format_mode) {
    const char *p = src;
    const char *start = p;   /* 현재 statement 의 시작 위치 */
    char quote = 0;          /* 0이면 따옴표 밖, 아니면 어떤 따옴표 안인지 기억 */

    while (*p) {
        if (quote) {
            /* 지금 따옴표 안: 같은 종류 따옴표를 만나면 빠져나옴. */
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            /* 따옴표 시작: 어떤 따옴표인지 기억. */
            quote = *p;
        } else if (*p == ';') {
            /* statement 끝: start ~ p-1 까지를 잘라 처리. */
            size_t len = (size_t)(p - start);
            char *stmt = malloc(len + 1);
            memcpy(stmt, start, len);
            stmt[len] = '\0';
            process_stmt(stmt, debug_mode, json_mode, tokens_mode, format_mode);
            free(stmt);
            start = p + 1;   /* 다음 statement 시작 */
        }
        p++;
    }

    /* 파일이 ; 없이 끝나는 경우 — 마지막 statement 도 처리.
     * 시작 부분의 공백/개행은 건너뛴다. */
    while (*start && (*start == ' ' || *start == '\n' || *start == '\t' || *start == '\r')) start++;
    if (*start) process_stmt(start, debug_mode, json_mode, tokens_mode, format_mode);
}

/* main: C 프로그램의 진짜 시작 함수.
 *
 *   argc — 명령줄 인자의 개수 (./sqlparser 자체 포함)
 *   argv — 인자들의 배열. argv[0]=프로그램 이름, argv[1]=첫 인자, ...
 */
int main(int argc, char **argv) {
    /* --help / --version 은 어디에 있어도 우선 처리하고 프로그램 종료. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("MiniSQL %s\n", MINISQL_VERSION);
            return 0;
        }
    }

    /* 인자가 부족하면 사용법 안내 후 에러로 종료. */
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <file.sql> [--json] [--debug] [--tokens] [--format]\n"
                "       %s --help\n",
                argv[0], argv[0]);
        return 1;
    }

    /* 옵션 플래그 파싱: 두 번째 인자부터 검사. */
    int json_mode = 0, debug_mode = 0, tokens_mode = 0, format_mode = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--json")   == 0) json_mode   = 1;
        if (strcmp(argv[i], "--debug")  == 0) debug_mode  = 1;
        if (strcmp(argv[i], "--tokens") == 0) tokens_mode = 1;
        if (strcmp(argv[i], "--format") == 0) format_mode = 1;
    }

    /* 첫 번째 인자 = SQL 파일 경로. 통째로 읽기. */
    char *src = read_file(argv[1]);
    if (!src) return 1;

    /* 진짜 일 시작. */
    run_statements(src, debug_mode, json_mode, tokens_mode, format_mode);

    free(src);
    return 0;
}
