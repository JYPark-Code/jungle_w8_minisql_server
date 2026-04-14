/* test_storage_select_result.c — Phase 1 RowSet 인프라 + 집계 함수 단위 테스트 (지용)
 *
 * 검증 대상:
 *   - storage_select_result() 가 RowSet 으로 결과를 반환
 *   - print_rowset() 출력 형식
 *   - rowset_free() NULL safe + 메모리 누수 0
 *   - 집계 함수 5종 (COUNT, SUM, AVG, MIN, MAX)
 *   - INT / FLOAT / VARCHAR / DATE 타입별 MIN/MAX
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define TEST_MKDIR(path) mkdir(path, 0775)
#endif

#include "types.h"

#define DATA_DIR    "data"
#define SCHEMA_DIR  "data/schema"
#define TABLES_DIR  "data/tables"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                              \
    if (cond) { g_pass++; }                                                \
    else      { g_fail++; fprintf(stderr, "  FAIL: %s\n", msg); }          \
} while (0)

static int ensure_dir(const char *path) {
    if (TEST_MKDIR(path) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static int write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    if (fputs(content, fp) == EOF) { fclose(fp); return -1; }
    fclose(fp);
    return 0;
}

static int prepare_orders_fixture(void) {
    if (ensure_dir(DATA_DIR)   != 0) return -1;
    if (ensure_dir(SCHEMA_DIR) != 0) return -1;
    if (ensure_dir(TABLES_DIR) != 0) return -1;

    /* schema: id INT, item VARCHAR, price FLOAT, qty INT, dt DATE */
    if (write_file(SCHEMA_DIR "/orders.schema",
                   "id,INT\nitem,VARCHAR\nprice,FLOAT\nqty,INT\ndt,DATE\n") != 0) return -1;

    if (write_file(TABLES_DIR "/orders.csv",
                   "1,apple,1.50,10,2024-01-15\n"
                   "2,banana,0.80,25,2024-03-02\n"
                   "3,cherry,5.00,4,2023-12-31\n"
                   "4,date,2.20,15,2024-06-20\n") != 0) return -1;

    return 0;
}

static void cleanup_orders_fixture(void) {
    remove(SCHEMA_DIR "/orders.schema");
    remove(TABLES_DIR "/orders.csv");
}

/* SELECT * FROM orders 같은 ParsedSQL 만들기 */
static ParsedSQL *make_select_all(void) {
    ParsedSQL *sql = calloc(1, sizeof(ParsedSQL));
    if (!sql) return NULL;
    sql->type = QUERY_SELECT;
    strcpy(sql->table, "orders");
    sql->limit = -1;
    sql->col_count = 1;
    sql->columns = calloc(1, sizeof(char *));
    sql->columns[0] = strdup("*");
    return sql;
}

/* SELECT <col_expr> FROM orders */
static ParsedSQL *make_select_one_column(const char *col_expr) {
    ParsedSQL *sql = calloc(1, sizeof(ParsedSQL));
    if (!sql) return NULL;
    sql->type = QUERY_SELECT;
    strcpy(sql->table, "orders");
    sql->limit = -1;
    sql->col_count = 1;
    sql->columns = calloc(1, sizeof(char *));
    sql->columns[0] = strdup(col_expr);
    return sql;
}

/* ─── 테스트 케이스 ──────────────────────────────────────── */

static void test_select_all_returns_rowset(void) {
    fprintf(stderr, "[ROWSET: SELECT * 기본 동작]\n");
    ParsedSQL *sql = make_select_all();
    RowSet *rs = NULL;
    int status = storage_select_result("orders", sql, &rs);

    CHECK(status == 0, "status == 0");
    CHECK(rs != NULL, "rs != NULL");
    if (rs) {
        CHECK(rs->row_count == 4, "row_count == 4");
        CHECK(rs->col_count == 5, "col_count == 5");
        CHECK(rs->col_names && strcmp(rs->col_names[0], "id") == 0, "col[0] == id");
        CHECK(rs->col_names && strcmp(rs->col_names[1], "item") == 0, "col[1] == item");
        CHECK(rs->rows && strcmp(rs->rows[0][1], "apple") == 0, "row[0].item == apple");
        CHECK(rs->rows && strcmp(rs->rows[3][1], "date") == 0, "row[3].item == date");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_count_star(void) {
    fprintf(stderr, "[AGGREGATE: COUNT(*)]\n");
    ParsedSQL *sql = make_select_one_column("COUNT(*)");
    RowSet *rs = NULL;
    int status = storage_select_result("orders", sql, &rs);

    CHECK(status == 0, "status");
    CHECK(rs && rs->row_count == 1 && rs->col_count == 1, "1x1 RowSet");
    if (rs && rs->row_count == 1 && rs->col_count == 1) {
        CHECK(strcmp(rs->col_names[0], "COUNT(*)") == 0, "col name");
        CHECK(strcmp(rs->rows[0][0], "4") == 0, "value == 4");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_sum_int(void) {
    fprintf(stderr, "[AGGREGATE: SUM(qty)]\n");
    ParsedSQL *sql = make_select_one_column("SUM(qty)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        /* 10 + 25 + 4 + 15 = 54 */
        CHECK(strcmp(rs->rows[0][0], "54") == 0, "SUM == 54");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_avg_float(void) {
    fprintf(stderr, "[AGGREGATE: AVG(price)]\n");
    ParsedSQL *sql = make_select_one_column("AVG(price)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        /* (1.50 + 0.80 + 5.00 + 2.20) / 4 = 9.50/4 = 2.375 → "2.38" */
        CHECK(strcmp(rs->rows[0][0], "2.38") == 0, "AVG == 2.38");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_min_float(void) {
    fprintf(stderr, "[AGGREGATE: MIN(price)]\n");
    ParsedSQL *sql = make_select_one_column("MIN(price)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        CHECK(strcmp(rs->rows[0][0], "0.80") == 0, "MIN == 0.80");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_max_int(void) {
    fprintf(stderr, "[AGGREGATE: MAX(qty)]\n");
    ParsedSQL *sql = make_select_one_column("MAX(qty)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        CHECK(strcmp(rs->rows[0][0], "25") == 0, "MAX == 25");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_max_varchar(void) {
    fprintf(stderr, "[AGGREGATE: MAX(item) — VARCHAR]\n");
    ParsedSQL *sql = make_select_one_column("MAX(item)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        /* "apple" / "banana" / "cherry" / "date" → max = "date" */
        CHECK(strcmp(rs->rows[0][0], "date") == 0, "MAX == date");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_max_date(void) {
    fprintf(stderr, "[AGGREGATE: MAX(dt) — DATE]\n");
    ParsedSQL *sql = make_select_one_column("MAX(dt)");
    RowSet *rs = NULL;
    storage_select_result("orders", sql, &rs);
    CHECK(rs && rs->row_count == 1, "1 row");
    if (rs && rs->row_count == 1) {
        /* '2024-01-15' / '2024-03-02' / '2023-12-31' / '2024-06-20' → max = '2024-06-20' */
        CHECK(strcmp(rs->rows[0][0], "2024-06-20") == 0, "MAX == 2024-06-20");
    }
    rowset_free(rs);
    free_parsed(sql);
}

static void test_sum_on_varchar_fails(void) {
    fprintf(stderr, "[AGGREGATE: SUM(item) — 타입 에러]\n");
    ParsedSQL *sql = make_select_one_column("SUM(item)");
    RowSet *rs = NULL;
    int status = storage_select_result("orders", sql, &rs);
    CHECK(status != 0, "status != 0 (SUM on VARCHAR rejected)");
    CHECK(rs == NULL, "rs == NULL on error");
    rowset_free(rs);
    free_parsed(sql);
}

static void test_aggregate_unknown_column(void) {
    fprintf(stderr, "[AGGREGATE: SUM(noexist) — 컬럼 없음 에러]\n");
    ParsedSQL *sql = make_select_one_column("SUM(noexist)");
    RowSet *rs = NULL;
    int status = storage_select_result("orders", sql, &rs);
    CHECK(status != 0, "status != 0");
    CHECK(rs == NULL, "rs == NULL");
    rowset_free(rs);
    free_parsed(sql);
}

static void test_rowset_free_null_safe(void) {
    fprintf(stderr, "[ROWSET: NULL safe free]\n");
    rowset_free(NULL);     /* must not crash */
    g_pass++;
}

static void test_print_rowset_null_safe(void) {
    fprintf(stderr, "[ROWSET: NULL safe print]\n");
    print_rowset(NULL, NULL);
    print_rowset(stderr, NULL);
    g_pass++;
}

/* ─── 메인 ─────────────────────────────────────────────── */

int main(void) {
    if (prepare_orders_fixture() != 0) {
        fprintf(stderr, "[ROWSET TESTS] FAIL: fixture preparation\n");
        return 1;
    }

    test_select_all_returns_rowset();
    test_count_star();
    test_sum_int();
    test_avg_float();
    test_min_float();
    test_max_int();
    test_max_varchar();
    test_max_date();
    test_sum_on_varchar_fails();
    test_aggregate_unknown_column();
    test_rowset_free_null_safe();
    test_print_rowset_null_safe();

    cleanup_orders_fixture();

    fprintf(stderr, "\n[ROWSET TESTS] %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
