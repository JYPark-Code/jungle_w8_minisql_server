/* ast_print.c — 파싱 결과를 트리 그림으로 보여주는 부품 (지용)
 * ============================================================================
 *
 * ▣ 이 파일이 하는 일
 *   ParsedSQL 구조체를 사람이 읽기 쉬운 트리 모양으로 출력한다.
 *   --debug 옵션을 켜면 이 함수가 호출돼서 화면에 트리가 찍힌다.
 *
 * ▣ 왜 필요하지?
 *   1) 파서가 SQL 을 어떻게 해석했는지 눈으로 확인 → 디버깅에 필수
 *   2) 발표 때 "이렇게 자르고 분류했어요!" 보여주기 좋음
 *
 * ▣ 출력 예시
 *   ParsedSQL
 *   ├─ type:  SELECT
 *   ├─ table: users
 *   ├─ columns (2):
 *   │   • id
 *   │   • name
 *   ├─ where (1):
 *   │   • age > 20
 *   ├─ order_by: name DESC
 *   ├─ limit: 5
 *   └─ end
 *
 * ▣ FILE* 를 인자로 받는 이유
 *   화면(stdout) 에 직접 찍게 하면 단위 테스트에서 검증이 어렵다.
 *   FILE* 로 받으면 메모리 스트림 (open_memstream) 으로 캡처해서
 *   "이런 글자가 들어있어야 해" 를 깔끔하게 검증 가능.
 * ============================================================================
 */

#include "types.h"
#include <stdio.h>
#include <unistd.h>

/* ANSI 컬러 매크로 (세인 PR #31 일부 채택).
 * file_is_tty() 가 0 이면 빈 문자열로 fallback → 단위 테스트의 메모리 스트림에는
 * 색이 들어가지 않아 fixture 검증 깨지지 않음. */
#define AST_RESET "\x1b[0m"
#define AST_TITLE "\x1b[1;95m"   /* 밝은 자홍 — ParsedSQL 헤더 */
#define AST_LABEL "\x1b[1;96m"   /* 밝은 청록 — 라벨 (type, table) */
#define AST_COUNT "\x1b[1;93m"   /* 밝은 노랑 — count 숫자 */

/* file_is_tty: out 이 실제 터미널에 연결되어 있나? 메모리 스트림이면 0. */
static int file_is_tty(FILE *fp) {
    if (fp == NULL) return 0;
    int fd = fileno(fp);
    if (fd < 0) return 0;
    return isatty(fd);
}

/* COLOR(c) — TTY 면 c, 아니면 빈 문자열. printf("%s%s%s", COLOR(AST_TITLE), text, COLOR(AST_RESET)). */
#define COLOR(c) (use_color ? (c) : "")

/* QueryType enum (숫자) → 사람이 읽을 수 있는 문자열 변환. */
static const char *qtype_name(QueryType t) {
    switch (t) {
        case QUERY_SELECT:  return "SELECT";
        case QUERY_INSERT:  return "INSERT";
        case QUERY_DELETE:  return "DELETE";
        case QUERY_UPDATE:  return "UPDATE";
        case QUERY_CREATE:  return "CREATE";
        default:            return "UNKNOWN";
    }
}

/* condition_index 번째 WHERE 조건 앞에 붙는 결합자.
 * Phase 1 에서는 where_links 를 우선 쓰고, 1주차 호환을 위해 where_logic 으로
 * fallback 한다. */
static const char *where_link_at(const ParsedSQL *sql, int condition_index) {
    if (!sql || condition_index <= 0) return NULL;
    if (sql->where_links && sql->where_links[condition_index - 1]) {
        return sql->where_links[condition_index - 1];
    }
    if (sql->where_logic[0]) return sql->where_logic;
    return NULL;
}

/* print_ast: ParsedSQL 트리를 out 으로 출력.
 *
 * 처음에 "ParsedSQL" 한 줄, 그 아래로 ├─ 와 └─ 모양의 트리 가지들.
 * 비어있는 필드는 출력 안 함 (예: WHERE 없으면 where 줄 자체 생략). */
void print_ast(FILE *out, const ParsedSQL *sql) {
    /* NULL safe: 둘 중 하나라도 NULL 이면 그냥 종료. 크래시 방지. */
    if (!out || !sql) return;

    /* TTY 일 때만 색을 사용. 메모리 스트림 (단위 테스트) 에서는 빈 문자열. */
    int use_color = file_is_tty(out);

    fprintf(out, "%sParsedSQL%s\n", COLOR(AST_TITLE), COLOR(AST_RESET));
    fprintf(out, "├─ %stype:%s  %s\n", COLOR(AST_LABEL), COLOR(AST_RESET), qtype_name(sql->type));

    /* table 이름은 항상 출력. 비어있으면 "(none)" 으로. */
    fprintf(out, "├─ %stable:%s %s\n", COLOR(AST_LABEL), COLOR(AST_RESET),
            sql->table[0] ? sql->table : "(none)");

    /* SELECT 컬럼 목록 또는 INSERT 컬럼 목록. */
    if (sql->col_count > 0) {
        fprintf(out, "├─ %scolumns%s (%s%d%s):\n",
                COLOR(AST_LABEL), COLOR(AST_RESET),
                COLOR(AST_COUNT), sql->col_count, COLOR(AST_RESET));
        for (int i = 0; i < sql->col_count; i++)
            fprintf(out, "│   • %s\n", sql->columns[i]);
    }

    /* INSERT VALUES (...) 목록. */
    if (sql->val_count > 0) {
        fprintf(out, "├─ %svalues%s (%s%d%s):\n",
                COLOR(AST_LABEL), COLOR(AST_RESET),
                COLOR(AST_COUNT), sql->val_count, COLOR(AST_RESET));
        for (int i = 0; i < sql->val_count; i++)
            fprintf(out, "│   • %s\n", sql->values[i]);
    }

    /* CREATE TABLE 컬럼 정의 ("id INT", "name VARCHAR" 같은 형태). */
    if (sql->col_def_count > 0) {
        fprintf(out, "├─ %scol_defs%s (%s%d%s):\n",
                COLOR(AST_LABEL), COLOR(AST_RESET),
                COLOR(AST_COUNT), sql->col_def_count, COLOR(AST_RESET));
        for (int i = 0; i < sql->col_def_count; i++)
            fprintf(out, "│   • %s\n", sql->col_defs[i]);
    }

    /* WHERE 조건들. 2번째 조건부터는 앞 결합자(AND/OR)도 함께 표시. */
    if (sql->where_count > 0) {
        fprintf(out, "├─ where (%d):\n", sql->where_count);
        for (int i = 0; i < sql->where_count; i++) {
            const char *link = where_link_at(sql, i);
            if (link) {
                fprintf(out, "│   • %s %s %s %s\n",
                        link,
                        sql->where[i].column,
                        sql->where[i].op,
                        sql->where[i].value);
            } else {
                fprintf(out, "│   • %s %s %s\n",
                        sql->where[i].column,
                        sql->where[i].op,
                        sql->where[i].value);
            }
        }
    }

    /* UPDATE SET 의 col=val 쌍들. */
    if (sql->set_count > 0) {
        fprintf(out, "├─ set (%d):\n", sql->set_count);
        for (int i = 0; i < sql->set_count; i++)
            fprintf(out, "│   • %s = %s\n", sql->set[i].column, sql->set[i].value);
    }

    /* ORDER BY (있을 때만). */
    if (sql->order_by) {
        fprintf(out, "├─ order_by: %s %s\n",
                sql->order_by->column,
                sql->order_by->asc ? "ASC" : "DESC");
    }

    /* LIMIT (있을 때만 — 기본값 -1 은 "없음" 표시). */
    if (sql->limit >= 0) {
        fprintf(out, "├─ limit: %d\n", sql->limit);
    }

    /* 마지막 닫는 가지. */
    fprintf(out, "└─ end\n");
}
