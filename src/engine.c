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
#include "types.h"
#include "bptree.h"
#include "index_registry.h"
#include "trie.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} json_buf_t;

typedef enum {
    ENGINE_LOCK_NONE,
    ENGINE_LOCK_TABLE,
    ENGINE_LOCK_CATALOG,
    ENGINE_LOCK_SINGLE
} engine_lock_kind_t;

typedef struct {
    engine_lock_kind_t kind;
    int catalog_read_held;
    char table[64];
} held_lock_t;

static atomic_uint_fast64_t s_total_queries;
static trie_t *s_dictionary_trie;
static atomic_bool s_dictionary_trie_ready;
/* ROUND2-WARMUP: engine_init 완료 ~ engine_shutdown 사이 true.
 * dictionary trie 의 실제 rebuild 상태 (s_dictionary_trie_ready) 와는 독립.
 * Router 의 사전 전용 엔드포인트 (/api/dict, /api/autocomplete, /api/admin/
 * insert) 가 engine 이 시작되기 전의 요청을 503 으로 fast-fail 하는 용도. */
static atomic_bool s_engine_ready;

enum {
    DICTIONARY_TRIE_MAX_ROWS = 4096
};

static uint64_t now_ns(void);
static double ns_to_ms(uint64_t ns);

static int jb_init(json_buf_t *b);
static int jb_reserve(json_buf_t *b, size_t extra);
static int jb_append(json_buf_t *b, const char *s);
static int jb_append_n(json_buf_t *b, const char *s, size_t n);
static int jb_appendf(json_buf_t *b, const char *fmt, ...);
static int jb_append_json_string(json_buf_t *b, const char *s);
static char *jb_take(json_buf_t *b);
static void jb_free(json_buf_t *b);

static const char *query_type_name(QueryType type);
static int split_next_statement(const char **cursor, char **out_stmt);
static char *trim_in_place(char *text);
static int execute_statement(ParsedSQL *sql, json_buf_t *out,
                             bool single_mode,
                             bool *out_index_used, int *out_nodes_visited);
static int execute_select(ParsedSQL *sql, json_buf_t *out,
                          bool *out_index_used, int *out_nodes_visited);
static int append_rowset_json(json_buf_t *out, const RowSet *rs);
static int append_status_json(json_buf_t *out, const ParsedSQL *sql,
                              int status, const char *action);
static int append_error_json(json_buf_t *out, const char *message);
static int parse_int_literal(const char *input, int *out_value);
static void strip_optional_quotes(const char *input, char *output, size_t output_size);
static char *trim_ws(char *text);
static int equals_ignore_case(const char *left, const char *right);
static int should_use_id_index(const ParsedSQL *sql);
static int should_use_id_range(const ParsedSQL *sql);
static int should_use_trie_prefix(const ParsedSQL *sql, char *prefix, size_t prefix_size);
static int is_ascii_lowercase_word(const char *text);
static int int_compare(const void *left, const void *right);
static int dictionary_schema_exists(void);
static int dictionary_trie_rebuild_locked(void);
static void dictionary_trie_replace(trie_t *next, int ready);
static void dictionary_trie_refresh_after_write(const ParsedSQL *sql, int status);
static int parse_between_bounds(const WhereClause *where, int *out_from, int *out_to);
static int lock_for_statement(const ParsedSQL *sql, bool single_mode, held_lock_t *held);
static void unlock_statement(const held_lock_t *held);
static int is_blank_sql(const char *sql);

int engine_init(const char *data_dir) {
    (void)data_dir;
    atomic_store_explicit(&s_total_queries, 0, memory_order_relaxed);
    atomic_store_explicit(&s_dictionary_trie_ready, false, memory_order_release);
    atomic_store_explicit(&s_engine_ready, false, memory_order_release);
    s_dictionary_trie = NULL;

    if (engine_lock_init() != 0) {
        return -1;
    }

    s_dictionary_trie = trie_create();
    if (s_dictionary_trie == NULL) {
        engine_lock_shutdown();
        return -1;
    }

    if (dictionary_schema_exists()) {
        (void)dictionary_trie_rebuild_locked();
    } else {
        /* dictionary 테이블이 없으면 빈 스키마로 자동 생성.
         * 이유: engine_is_ready() 가 s_dictionary_trie_ready 를 요구하므로,
         * 테이블이 없으면 /api/admin/insert 까지 503 warming_up 에 갇혀
         * chicken-and-egg (insert 로 테이블을 만들 수도 없음) 가 됨.
         * CREATE 성공 시 dictionary_trie_refresh_after_write 가 내부적으로
         * trie_rebuild 를 호출해 s_dictionary_trie_ready 를 true 로 올림. */
        engine_result_t bootstrap = engine_exec_sql(
            "CREATE TABLE dictionary (id INT, english VARCHAR, korean VARCHAR);",
            false);
        engine_result_free(&bootstrap);
    }

    atomic_store_explicit(&s_engine_ready, true, memory_order_release);
    return 0;
}

engine_result_t engine_exec_sql(const char *sql, bool single_mode) {
    engine_result_t r;
    json_buf_t out;
    const char *cursor;
    int statement_count = 0;
    int first = 1;
    bool all_ok = true;
    bool index_used = false;
    int nodes_visited = 0;
    uint64_t start_ns;
    uint64_t wait_before;
    uint64_t wait_after;

    memset(&r, 0, sizeof(r));

    start_ns = now_ns();
    wait_before = engine_lock_wait_ns_total();

    if (jb_init(&out) != 0) {
        r.ok = false;
        return r;
    }

    if (is_blank_sql(sql)) {
        jb_append(&out, "{\"ok\":false,\"error\":\"empty sql\"}");
        r.ok = false;
        r.json = jb_take(&out);
        r.elapsed_ms = ns_to_ms(now_ns() - start_ns);
        return r;
    }

    if (jb_append(&out, "{\"ok\":") != 0) goto oom;
    /* ok 값은 마지막에 알 수 없으므로 statements 앞까지는 뒤에서 재작성하지 않고
     * top-level ok 는 별도 prefix buffer 없이 마지막 JSON 조립에서 보수적으로 처리한다. */
    out.len = 0;
    if (jb_append(&out, "{\"statements\":[") != 0) goto oom;

    cursor = sql;
    for (;;) {
        char *stmt = NULL;
        ParsedSQL *parsed;
        bool stmt_index_used = false;
        int stmt_nodes = 0;
        int status;

        if (split_next_statement(&cursor, &stmt) != 0) {
            all_ok = false;
            if (!first) jb_append(&out, ",");
            append_error_json(&out, "failed to split sql statement");
            first = 0;
            break;
        }
        if (stmt == NULL) {
            break;
        }

        parsed = parse_sql(stmt);
        free(stmt);

        if (!first) {
            if (jb_append(&out, ",") != 0) {
                free_parsed(parsed);
                goto oom;
            }
        }
        first = 0;
        statement_count++;

        if (parsed == NULL || parsed->type == QUERY_UNKNOWN) {
            all_ok = false;
            append_error_json(&out, "parse error or unsupported query");
            free_parsed(parsed);
            continue;
        }

        status = execute_statement(parsed, &out, single_mode, &stmt_index_used, &stmt_nodes);
        if (status != 0) {
            all_ok = false;
        }
        if (stmt_index_used) {
            index_used = true;
        }
        if (stmt_nodes > nodes_visited) {
            nodes_visited = stmt_nodes;
        }

        free_parsed(parsed);
    }

    if (statement_count == 0) {
        all_ok = false;
        append_error_json(&out, "empty sql");
    }

    if (jb_append(&out, "],\"ok\":") != 0) goto oom;
    if (jb_append(&out, all_ok ? "true" : "false") != 0) goto oom;
    if (jb_appendf(&out, ",\"statement_count\":%d", statement_count) != 0) goto oom;
    if (jb_append(&out, ",\"index_used\":") != 0) goto oom;
    if (jb_append(&out, index_used ? "true" : "false") != 0) goto oom;
    if (jb_appendf(&out, ",\"nodes_visited\":%d", nodes_visited) != 0) goto oom;

    wait_after = engine_lock_wait_ns_total();
    r.lock_wait_ms = ns_to_ms(wait_after - wait_before);
    r.elapsed_ms = ns_to_ms(now_ns() - start_ns);
    if (jb_appendf(&out, ",\"elapsed_ms\":%.3f,\"lock_wait_ms\":%.3f}",
                   r.elapsed_ms, r.lock_wait_ms) != 0) goto oom;

    r.ok = all_ok;
    r.json = jb_take(&out);
    r.index_used = index_used;
    r.nodes_visited = nodes_visited;
    atomic_fetch_add_explicit(&s_total_queries, (uint64_t)statement_count,
                              memory_order_relaxed);
    return r;

oom:
    jb_free(&out);
    memset(&r, 0, sizeof(r));
    r.ok = false;
    r.elapsed_ms = ns_to_ms(now_ns() - start_ns);
    return r;
}

engine_result_t engine_explain(const char *sql) {
    engine_result_t r;
    ParsedSQL *parsed = NULL;
    json_buf_t out;
    char *stmt = NULL;
    const char *cursor = sql;
    const char *plan = "UNKNOWN";

    memset(&r, 0, sizeof(r));

    if (jb_init(&out) != 0) {
        r.ok = false;
        return r;
    }

    if (sql == NULL || split_next_statement(&cursor, &stmt) != 0 || stmt == NULL) {
        jb_append(&out, "{\"ok\":false,\"error\":\"empty sql\"}");
        r.json = jb_take(&out);
        r.ok = false;
        return r;
    }

    parsed = parse_sql(stmt);
    free(stmt);
    if (parsed == NULL || parsed->type == QUERY_UNKNOWN) {
        jb_append(&out, "{\"ok\":false,\"error\":\"parse error or unsupported query\"}");
        free_parsed(parsed);
        r.json = jb_take(&out);
        r.ok = false;
        return r;
    }

    if (parsed->type == QUERY_SELECT) {
        char prefix[256];
        if (should_use_id_range(parsed)) {
            plan = "BPTREE_RANGE_SCAN";
            r.index_used = true;
            r.nodes_visited = 1;
        } else if (should_use_id_index(parsed)) {
            plan = "BPTREE_POINT_LOOKUP";
            r.index_used = true;
            r.nodes_visited = 1;
        } else if (should_use_trie_prefix(parsed, prefix, sizeof(prefix)) &&
                   atomic_load_explicit(&s_dictionary_trie_ready, memory_order_acquire)) {
            plan = "TRIE_PREFIX_SCAN";
            r.index_used = true;
            r.nodes_visited = (int)strlen(prefix);
        } else {
            plan = "FULL_SCAN";
        }
    } else if (parsed->type == QUERY_CREATE) {
        plan = "CATALOG_WRITE";
    } else {
        plan = "TABLE_WRITE";
    }

    jb_append(&out, "{\"ok\":true,\"type\":");
    jb_append_json_string(&out, query_type_name(parsed->type));
    jb_append(&out, ",\"table\":");
    jb_append_json_string(&out, parsed->table);
    jb_append(&out, ",\"plan\":");
    jb_append_json_string(&out, plan);
    jb_append(&out, ",\"index_used\":");
    jb_append(&out, r.index_used ? "true" : "false");
    jb_appendf(&out, ",\"nodes_visited\":%d}", r.nodes_visited);

    free_parsed(parsed);
    r.ok = true;
    r.json = jb_take(&out);
    return r;
}

void engine_get_stats(uint64_t *total_queries, uint64_t *lock_wait_ns_total) {
    if (total_queries)       *total_queries       = atomic_load_explicit(&s_total_queries, memory_order_relaxed);
    if (lock_wait_ns_total)  *lock_wait_ns_total  = engine_lock_wait_ns_total();
}

void engine_shutdown(void) {
    atomic_store_explicit(&s_engine_ready, false, memory_order_release);
    dictionary_trie_replace(NULL, 0);
    storage_reset_internal_caches();
    index_registry_destroy_all();
    engine_lock_shutdown();
}

void engine_result_free(engine_result_t *r) {
    if (!r) return;
    free(r->json);
    r->json = NULL;
}

bool engine_is_ready(void) {
    /* engine_init 자체 완료 && dictionary trie 실데이터 준비 상태 모두 true.
     * engine_init 안 됐거나, dictionary 테이블이 아직 없거나 rebuild 중이면 false.
     * Router 는 이 값이 false 일 때 /api/dict, /api/autocomplete, /api/admin/insert
     * 의 engine_exec_sql 직전 체크에서 503 warming_up 반환. 각 handler 의
     * format 검증 (405/400) 은 gate 이전이라 영향 없음. */
    return atomic_load_explicit(&s_engine_ready, memory_order_acquire)
        && atomic_load_explicit(&s_dictionary_trie_ready, memory_order_acquire);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double ns_to_ms(uint64_t ns)
{
    return (double)ns / 1000000.0;
}

static int jb_init(json_buf_t *b)
{
    b->cap = 256;
    b->len = 0;
    b->data = malloc(b->cap);
    if (b->data == NULL) {
        b->cap = 0;
        return -1;
    }
    b->data[0] = '\0';
    return 0;
}

static int jb_reserve(json_buf_t *b, size_t extra)
{
    size_t need;
    size_t new_cap;
    char *grown;

    if (b == NULL || b->data == NULL) return -1;
    if (extra > SIZE_MAX - b->len - 1U) return -1;

    need = b->len + extra + 1U;
    if (need <= b->cap) return 0;

    new_cap = b->cap;
    while (new_cap < need) {
        if (new_cap > SIZE_MAX / 2U) return -1;
        new_cap *= 2U;
    }

    grown = realloc(b->data, new_cap);
    if (grown == NULL) return -1;
    b->data = grown;
    b->cap = new_cap;
    return 0;
}

static int jb_append(json_buf_t *b, const char *s)
{
    return jb_append_n(b, s, s ? strlen(s) : 0U);
}

static int jb_append_n(json_buf_t *b, const char *s, size_t n)
{
    if (n == 0U) return 0;
    if (s == NULL || jb_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int jb_appendf(json_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_list cp;
    int n;

    va_start(ap, fmt);
    va_copy(cp, ap);
    n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0) {
        va_end(ap);
        return -1;
    }
    if (jb_reserve(b, (size_t)n) != 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

static int jb_append_json_string(json_buf_t *b, const char *s)
{
    if (jb_append(b, "\"") != 0) return -1;
    if (s != NULL) {
        const unsigned char *p = (const unsigned char *)s;
        while (*p != '\0') {
            char esc[8];
            switch (*p) {
                case '"':
                    if (jb_append(b, "\\\"") != 0) return -1;
                    break;
                case '\\':
                    if (jb_append(b, "\\\\") != 0) return -1;
                    break;
                case '\n':
                    if (jb_append(b, "\\n") != 0) return -1;
                    break;
                case '\r':
                    if (jb_append(b, "\\r") != 0) return -1;
                    break;
                case '\t':
                    if (jb_append(b, "\\t") != 0) return -1;
                    break;
                default:
                    if (*p < 0x20U) {
                        snprintf(esc, sizeof(esc), "\\u%04x", *p);
                        if (jb_append(b, esc) != 0) return -1;
                    } else if (jb_append_n(b, (const char *)p, 1U) != 0) {
                        return -1;
                    }
                    break;
            }
            p++;
        }
    }
    return jb_append(b, "\"");
}

static char *jb_take(json_buf_t *b)
{
    char *data = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return data;
}

static void jb_free(json_buf_t *b)
{
    if (b == NULL) return;
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static const char *query_type_name(QueryType type)
{
    switch (type) {
        case QUERY_SELECT: return "SELECT";
        case QUERY_INSERT: return "INSERT";
        case QUERY_DELETE: return "DELETE";
        case QUERY_UPDATE: return "UPDATE";
        case QUERY_CREATE: return "CREATE";
        default: return "UNKNOWN";
    }
}

static int split_next_statement(const char **cursor, char **out_stmt)
{
    const char *p;
    const char *start;
    char quote = 0;
    size_t len;
    char *stmt;

    if (cursor == NULL || out_stmt == NULL) return -1;
    *out_stmt = NULL;
    p = *cursor;
    if (p == NULL) return 0;

    while (*p != '\0' && isspace((unsigned char)*p)) p++;
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }

    start = p;
    while (*p != '\0') {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == ';') {
            break;
        }
        p++;
    }

    len = (size_t)(p - start);
    stmt = malloc(len + 1U);
    if (stmt == NULL) return -1;
    memcpy(stmt, start, len);
    stmt[len] = '\0';
    trim_in_place(stmt);

    if (*p == ';') p++;
    *cursor = p;

    if (stmt[0] == '\0') {
        free(stmt);
        return split_next_statement(cursor, out_stmt);
    }

    *out_stmt = stmt;
    return 0;
}

static char *trim_in_place(char *text)
{
    char *start;
    char *end;

    if (text == NULL) return text;
    start = trim_ws(text);
    if (start != text) memmove(text, start, strlen(start) + 1U);

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static int execute_statement(ParsedSQL *sql, json_buf_t *out,
                             bool single_mode,
                             bool *out_index_used, int *out_nodes_visited)
{
    held_lock_t held;
    int status = -1;

    if (out_index_used) *out_index_used = false;
    if (out_nodes_visited) *out_nodes_visited = 0;
    memset(&held, 0, sizeof(held));

    if (lock_for_statement(sql, single_mode, &held) != 0) {
        append_error_json(out, "failed to acquire engine lock");
        return -1;
    }

    switch (sql->type) {
        case QUERY_SELECT:
            status = execute_select(sql, out, out_index_used, out_nodes_visited);
            break;
        case QUERY_CREATE:
            status = storage_create(sql->table, sql->col_defs, sql->col_def_count);
            dictionary_trie_refresh_after_write(sql, status);
            append_status_json(out, sql, status, "create");
            break;
        case QUERY_INSERT:
            status = storage_insert(sql->table, sql->columns, sql->values, sql->val_count);
            dictionary_trie_refresh_after_write(sql, status);
            append_status_json(out, sql, status, "insert");
            break;
        case QUERY_DELETE:
            status = storage_delete(sql->table, sql);
            dictionary_trie_refresh_after_write(sql, status);
            append_status_json(out, sql, status, "delete");
            break;
        case QUERY_UPDATE:
            status = storage_update(sql->table, sql);
            dictionary_trie_refresh_after_write(sql, status);
            append_status_json(out, sql, status, "update");
            break;
        default:
            append_error_json(out, "unsupported query type");
            status = -1;
            break;
    }

    unlock_statement(&held);
    return status;
}

static int execute_select(ParsedSQL *sql, json_buf_t *out,
                          bool *out_index_used, int *out_nodes_visited)
{
    RowSet *rs = NULL;
    int status = -1;

    storage_ensure_index(sql->table);

    if (should_use_id_range(sql)) {
        BPTree *tree = index_registry_get(sql->table);
        int from;
        int to;
        int *row_indices = NULL;
        size_t capacity = 16U;
        int count;

        if (tree != NULL && parse_between_bounds(&sql->where[0], &from, &to) == 0) {
            row_indices = malloc(sizeof(*row_indices) * capacity);
            if (row_indices == NULL) return -1;
            for (;;) {
                int *grown;
                count = bptree_range(tree, from, to, row_indices, (int)capacity);
                if ((size_t)count < capacity) break;
                if (capacity > (size_t)INT_MAX / 2U) {
                    free(row_indices);
                    return -1;
                }
                grown = realloc(row_indices, sizeof(*row_indices) * capacity * 2U);
                if (grown == NULL) {
                    free(row_indices);
                    return -1;
                }
                row_indices = grown;
                capacity *= 2U;
            }
            status = storage_select_result_by_row_indices(sql->table, sql, row_indices, count, &rs);
            free(row_indices);
            if (status == 0) {
                if (out_index_used) *out_index_used = true;
                if (out_nodes_visited) *out_nodes_visited = 1;
                return append_rowset_json(out, rs);
            }
            rowset_free(rs);
            rs = NULL;
        }
    } else if (should_use_id_index(sql)) {
        BPTree *tree = index_registry_get(sql->table);
        int lookup_id;

        if (tree != NULL && parse_int_literal(sql->where[0].value, &lookup_id) == 0) {
            int row_index = bptree_search(tree, lookup_id);
            status = storage_select_result_by_row_index(sql->table, sql, row_index, &rs);
            if (status == 0) {
                if (out_index_used) *out_index_used = true;
                if (out_nodes_visited) *out_nodes_visited = 1;
                return append_rowset_json(out, rs);
            }
            rowset_free(rs);
            rs = NULL;
        }
    }

    {
        char prefix[256];
        if (should_use_trie_prefix(sql, prefix, sizeof(prefix)) &&
            atomic_load_explicit(&s_dictionary_trie_ready, memory_order_acquire) &&
            s_dictionary_trie != NULL) {
            int *row_indices;
            int count;

            row_indices = malloc(sizeof(*row_indices) * DICTIONARY_TRIE_MAX_ROWS);
            if (row_indices == NULL) {
                return -1;
            }

            count = trie_search_prefix(s_dictionary_trie, prefix, row_indices,
                                       DICTIONARY_TRIE_MAX_ROWS);
            if (count >= DICTIONARY_TRIE_MAX_ROWS) {
                free(row_indices);
            } else {
                qsort(row_indices, (size_t)count, sizeof(*row_indices), int_compare);
                status = storage_select_result_by_row_indices(sql->table, sql,
                                                              row_indices, count, &rs);
                free(row_indices);
                if (status == 0) {
                    if (out_index_used) *out_index_used = true;
                    if (out_nodes_visited) *out_nodes_visited = (int)strlen(prefix);
                    return append_rowset_json(out, rs);
                }
                rowset_free(rs);
                rs = NULL;
            }
        }
    }

    status = storage_select_result(sql->table, sql, &rs);
    if (status != 0) {
        rowset_free(rs);
        append_status_json(out, sql, status, "select");
        return -1;
    }
    return append_rowset_json(out, rs);
}

static int append_rowset_json(json_buf_t *out, const RowSet *rs)
{
    int i;
    int j;

    if (jb_append(out, "{\"ok\":true,\"type\":\"SELECT\",\"columns\":[") != 0) return -1;
    if (rs != NULL) {
        for (i = 0; i < rs->col_count; i++) {
            if (i && jb_append(out, ",") != 0) return -1;
            if (jb_append_json_string(out, rs->col_names ? rs->col_names[i] : "") != 0) return -1;
        }
    }
    if (jb_append(out, "],\"rows\":[") != 0) return -1;
    if (rs != NULL) {
        for (i = 0; i < rs->row_count; i++) {
            if (i && jb_append(out, ",") != 0) return -1;
            if (jb_append(out, "[") != 0) return -1;
            for (j = 0; j < rs->col_count; j++) {
                if (j && jb_append(out, ",") != 0) return -1;
                if (jb_append_json_string(out, rs->rows[i][j]) != 0) return -1;
            }
            if (jb_append(out, "]") != 0) return -1;
        }
    }
    if (jb_appendf(out, "],\"row_count\":%d}", rs ? rs->row_count : 0) != 0) return -1;
    rowset_free((RowSet *)rs);
    return 0;
}

static int append_status_json(json_buf_t *out, const ParsedSQL *sql,
                              int status, const char *action)
{
    if (jb_append(out, "{\"ok\":") != 0) return -1;
    if (jb_append(out, status == 0 ? "true" : "false") != 0) return -1;
    if (jb_append(out, ",\"type\":") != 0) return -1;
    if (jb_append_json_string(out, query_type_name(sql ? sql->type : QUERY_UNKNOWN)) != 0) return -1;
    if (jb_append(out, ",\"table\":") != 0) return -1;
    if (jb_append_json_string(out, sql ? sql->table : "") != 0) return -1;
    if (jb_append(out, ",\"action\":") != 0) return -1;
    if (jb_append_json_string(out, action) != 0) return -1;
    if (status != 0 && jb_append(out, ",\"error\":\"storage operation failed\"") != 0) return -1;
    return jb_append(out, "}");
}

static int append_error_json(json_buf_t *out, const char *message)
{
    if (jb_append(out, "{\"ok\":false,\"error\":") != 0) return -1;
    if (jb_append_json_string(out, message) != 0) return -1;
    return jb_append(out, "}");
}

static int parse_int_literal(const char *input, int *out_value)
{
    char literal[256];
    char *trimmed;
    char *end = NULL;
    long parsed;

    if (input == NULL || out_value == NULL) return -1;
    strip_optional_quotes(input, literal, sizeof(literal));
    trimmed = trim_ws(literal);
    if (trimmed[0] == '\0') return -1;

    errno = 0;
    parsed = strtol(trimmed, &end, 10);
    if (errno != 0 || end == trimmed) return -1;
    while (*end != '\0' && isspace((unsigned char)*end)) end++;
    if (*end != '\0' || parsed < INT_MIN || parsed > INT_MAX) return -1;
    *out_value = (int)parsed;
    return 0;
}

static void strip_optional_quotes(const char *input, char *output, size_t output_size)
{
    size_t length;
    size_t copy_length;

    if (output == NULL || output_size == 0U) return;
    if (input == NULL) {
        output[0] = '\0';
        return;
    }
    length = strlen(input);
    if (length >= 2U &&
        ((input[0] == '\'' && input[length - 1U] == '\'') ||
         (input[0] == '"' && input[length - 1U] == '"'))) {
        input++;
        length -= 2U;
    }
    copy_length = (length < output_size - 1U) ? length : output_size - 1U;
    memcpy(output, input, copy_length);
    output[copy_length] = '\0';
}

static char *trim_ws(char *text)
{
    char *end;
    while (text != NULL && *text != '\0' && isspace((unsigned char)*text)) text++;
    if (text == NULL) return NULL;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return text;
}

static int equals_ignore_case(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return 0;
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return 0;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static int should_use_id_index(const ParsedSQL *sql)
{
    if (sql == NULL || sql->where_count != 1 || sql->where == NULL) return 0;
    return equals_ignore_case(sql->where[0].column, "id") &&
           strcmp(sql->where[0].op, "=") == 0;
}

static int should_use_id_range(const ParsedSQL *sql)
{
    if (sql == NULL || sql->where_count != 1 || sql->where == NULL) return 0;
    return equals_ignore_case(sql->where[0].column, "id") &&
           equals_ignore_case(sql->where[0].op, "BETWEEN");
}

static int should_use_trie_prefix(const ParsedSQL *sql, char *prefix, size_t prefix_size)
{
    char literal[256];
    size_t length;
    size_t prefix_length;
    size_t i;

    if (prefix == NULL || prefix_size == 0U) return 0;
    prefix[0] = '\0';

    if (sql == NULL || sql->where_count != 1 || sql->where == NULL) return 0;
    if (!equals_ignore_case(sql->table, "dictionary")) return 0;
    if (!equals_ignore_case(sql->where[0].column, "english")) return 0;
    if (strcmp(sql->where[0].op, "LIKE") != 0) return 0;

    strip_optional_quotes(sql->where[0].value, literal, sizeof(literal));
    length = strlen(literal);
    if (length < 2U || literal[length - 1U] != '%') return 0;

    prefix_length = length - 1U;
    if (prefix_length == 0U || prefix_length >= prefix_size) return 0;

    for (i = 0; i < prefix_length; ++i) {
        if (literal[i] == '%' || literal[i] == '_') return 0;
        prefix[i] = literal[i];
    }
    prefix[prefix_length] = '\0';

    return is_ascii_lowercase_word(prefix);
}

static int is_ascii_lowercase_word(const char *text)
{
    const char *p;

    if (text == NULL || text[0] == '\0') return 0;
    for (p = text; *p != '\0'; ++p) {
        if (*p < 'a' || *p > 'z') return 0;
    }
    return 1;
}

static int int_compare(const void *left, const void *right)
{
    const int a = *(const int *)left;
    const int b = *(const int *)right;

    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static int dictionary_schema_exists(void)
{
    static const char *paths[] = {
        "data/schema/dictionary.schema",
        "data/dictionary.schema"
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *fp = fopen(paths[i], "r");
        if (fp != NULL) {
            fclose(fp);
            return 1;
        }
    }
    return 0;
}

static int dictionary_trie_rebuild_locked(void)
{
    trie_t *next = NULL;
    ParsedSQL *select_sql = NULL;
    RowSet *rs = NULL;
    int status = -1;
    int i;

    next = trie_create();
    if (next == NULL) {
        dictionary_trie_replace(NULL, 0);
        return -1;
    }

    if (!dictionary_schema_exists()) {
        dictionary_trie_replace(next, 0);
        return 0;
    }

    select_sql = parse_sql("SELECT english FROM dictionary;");
    if (select_sql == NULL) {
        goto fail;
    }

    if (storage_select_result("dictionary", select_sql, &rs) != 0) {
        goto fail;
    }

    for (i = 0; rs != NULL && i < rs->row_count; ++i) {
        const char *word = "";
        if (rs->rows != NULL && rs->rows[i] != NULL && rs->rows[i][0] != NULL) {
            word = rs->rows[i][0];
        }
        if (trie_insert(next, word, i) != 0) {
            goto fail;
        }
    }

    status = 0;

fail:
    rowset_free(rs);
    free_parsed(select_sql);

    if (status == 0) {
        dictionary_trie_replace(next, 1);
        return 0;
    }

    trie_destroy(next);
    dictionary_trie_replace(trie_create(), 0);
    return -1;
}

static void dictionary_trie_replace(trie_t *next, int ready)
{
    trie_t *old = s_dictionary_trie;

    s_dictionary_trie = next;
    atomic_store_explicit(&s_dictionary_trie_ready,
                          (ready && next != NULL) ? true : false,
                          memory_order_release);
    trie_destroy(old);
}

static void dictionary_trie_refresh_after_write(const ParsedSQL *sql, int status)
{
    if (status != 0 || sql == NULL) return;
    if (!equals_ignore_case(sql->table, "dictionary")) return;
    (void)dictionary_trie_rebuild_locked();
}

static int parse_between_bounds(const WhereClause *where, int *out_from, int *out_to)
{
    int from;
    int to;
    if (where == NULL || out_from == NULL || out_to == NULL) return -1;
    if (parse_int_literal(where->value, &from) != 0 ||
        parse_int_literal(where->value_to, &to) != 0) {
        return -1;
    }
    if (from > to) {
        int temp = from;
        from = to;
        to = temp;
    }
    *out_from = from;
    *out_to = to;
    return 0;
}

static int lock_for_statement(const ParsedSQL *sql, bool single_mode, held_lock_t *held)
{
    if (held == NULL) return -1;
    held->kind = ENGINE_LOCK_NONE;
    held->table[0] = '\0';

    if (single_mode) {
        engine_lock_single_enter();
        held->kind = ENGINE_LOCK_SINGLE;
        return 0;
    }

    if (sql == NULL) return -1;
    if (sql->type == QUERY_CREATE) {
        engine_lock_catalog_write();
        held->kind = ENGINE_LOCK_CATALOG;
        return 0;
    }

    engine_lock_catalog_read();
    held->catalog_read_held = 1;
    snprintf(held->table, sizeof(held->table), "%s", sql->table);
    if (sql->type == QUERY_SELECT) {
        engine_lock_table_read(sql->table);
    } else {
        engine_lock_table_write(sql->table);
    }
    held->kind = ENGINE_LOCK_TABLE;
    return 0;
}

static void unlock_statement(const held_lock_t *held)
{
    if (held == NULL) return;
    switch (held->kind) {
        case ENGINE_LOCK_SINGLE:
            engine_lock_single_exit();
            break;
        case ENGINE_LOCK_CATALOG:
            engine_lock_catalog_release();
            break;
        case ENGINE_LOCK_TABLE:
            engine_lock_table_release(held->table);
            if (held->catalog_read_held) {
                engine_lock_catalog_release();
            }
            break;
        default:
            break;
    }
}

static int is_blank_sql(const char *sql)
{
    if (sql == NULL) return 1;
    while (*sql != '\0') {
        if (!isspace((unsigned char)*sql)) return 0;
        sql++;
    }
    return 1;
}
