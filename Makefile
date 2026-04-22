# Makefile — minisqld (W8)
# =============================================================================
# 타겟:
#   make            ./minisqld 데몬 빌드
#   make test       W7 회귀 (parser/executor/storage/bptree/index_registry)
#                   + W8 단위 테스트 (engine_lock 등)
#   make tsan       ThreadSanitizer 빌드 (./minisqld_tsan)
#   make valgrind   W7/W8 테스트 바이너리들을 valgrind 로 회귀
#   make bench      B+Tree pure 벤치 (W7 자산)
#   make repl       ./minisqld-repl ANSI REPL 클라이언트 (발표 백업 시연용)
#   make loadtest   (추후) 동시 N 요청 부하 테스트 — 현재는 no-op placeholder
#   make clean      빌드 산출물 + data/ 테스트 잔여물 제거
# =============================================================================

CC          = gcc
CSTD        = -std=c11
CFLAGS_BASE = -Wall -Wextra -Wpedantic $(CSTD) -D_POSIX_C_SOURCE=200809L -I./include -I./src
CFLAGS_REL  = -O2 -g
CFLAGS_TSAN = -O1 -g -fsanitize=thread
LDFLAGS     = -pthread
LDFLAGS_TSAN= -pthread -fsanitize=thread

CFLAGS      = $(CFLAGS_BASE) $(CFLAGS_REL)

SRC_DIR    = src
TEST_DIR   = tests
BENCH_DIR  = bench
CLIENT_DIR = client

# ---------------------------------------------------------------------------
# 소스 그룹
# ---------------------------------------------------------------------------
# W7 엔진 (동현만 수정 권한)
W7_SRCS  = $(SRC_DIR)/parser.c \
           $(SRC_DIR)/executor.c \
           $(SRC_DIR)/storage.c \
           $(SRC_DIR)/bptree.c \
           $(SRC_DIR)/index_registry.c \
           $(SRC_DIR)/ast_print.c \
           $(SRC_DIR)/json_out.c \
           $(SRC_DIR)/sql_format.c

# W8 신규 레이어 (각 담당자)
W8_SRCS  = $(SRC_DIR)/main.c \
           $(SRC_DIR)/server.c \
           $(SRC_DIR)/protocol.c \
           $(SRC_DIR)/router.c \
           $(SRC_DIR)/dict_cache.c \
           $(SRC_DIR)/engine.c \
           $(SRC_DIR)/engine_lock.c \
           $(SRC_DIR)/threadpool.c

DAEMON_SRCS = $(W8_SRCS) $(W7_SRCS)
DAEMON      = minisqld
DAEMON_TSAN = minisqld_tsan

# ---------------------------------------------------------------------------
# W7 테스트 (기존 회귀 — 변경 없음)
# ---------------------------------------------------------------------------
STORAGE_TEST_DEPS    = $(SRC_DIR)/storage.c $(SRC_DIR)/bptree.c $(SRC_DIR)/index_registry.c
SELECT_RESULT_DEPS   = $(SRC_DIR)/storage.c $(SRC_DIR)/parser.c $(SRC_DIR)/bptree.c $(SRC_DIR)/index_registry.c
BPTREE_TEST_DEPS     = $(SRC_DIR)/bptree.c
BENCH_TEST_DEPS      = $(SRC_DIR)/bptree.c
REGISTRY_TEST_DEPS   = $(SRC_DIR)/index_registry.c $(SRC_DIR)/bptree.c
THREADPOOL_TEST_DEPS = $(SRC_DIR)/threadpool.c

STORAGE_TEST_TARGETS = test_storage_insert test_storage_delete \
                       test_storage_update test_storage_select_result

TEST_PARSER_EXEC_SRCS = $(TEST_DIR)/test_parser.c \
                        $(TEST_DIR)/test_executor.c \
                        $(W7_SRCS)
TEST_PARSER_EXEC      = test_parser_executor

BENCH_TARGET = benchmark
BENCH_SRCS   = $(BENCH_DIR)/benchmark.c $(W7_SRCS)

REPL_TARGET = minisqld-repl
REPL_SRCS   = $(CLIENT_DIR)/repl.c

# ---------------------------------------------------------------------------
# .PHONY
# ---------------------------------------------------------------------------
.PHONY: all clean test tsan valgrind bench loadtest repl \
        test_storage_all \
        test_bptree test_benchmark test_index_registry test_engine_lock test_threadpool test_dict_cache

# ---------------------------------------------------------------------------
# 기본 빌드 — 데몬
# ---------------------------------------------------------------------------
all: $(DAEMON)

$(DAEMON): $(DAEMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---------------------------------------------------------------------------
# ThreadSanitizer 빌드 (동시성 회귀용)
#   · 같은 소스를 TSan 플래그로 별도 바이너리에 빌드
#   · 팀원은 ./minisqld_tsan 을 직접 실행해 부하 재현 가능
# ---------------------------------------------------------------------------
tsan: $(DAEMON_TSAN)

$(DAEMON_TSAN): $(DAEMON_SRCS)
	$(CC) $(CFLAGS_BASE) $(CFLAGS_TSAN) -o $@ $^ $(LDFLAGS_TSAN)

# ---------------------------------------------------------------------------
# 회귀 테스트 (W7 + 추후 W8)
# ---------------------------------------------------------------------------
test: $(TEST_PARSER_EXEC) test_storage_all test_bptree test_benchmark test_index_registry test_engine_lock test_threadpool test_dict_cache
	./$(TEST_PARSER_EXEC)

$(TEST_PARSER_EXEC): $(TEST_PARSER_EXEC_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_storage_insert: $(TEST_DIR)/test_storage_insert.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_storage_delete: $(TEST_DIR)/test_storage_delete.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_storage_update: $(TEST_DIR)/test_storage_update.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_storage_select_result: $(TEST_DIR)/test_storage_select_result.c $(SELECT_RESULT_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_storage_all: $(STORAGE_TEST_TARGETS)

test_bptree: $(TEST_DIR)/test_bptree.c $(BPTREE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_benchmark: $(TEST_DIR)/test_benchmark.c $(BENCH_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_index_registry: $(TEST_DIR)/test_index_registry.c $(REGISTRY_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_engine_lock: $(TEST_DIR)/test_engine_lock.c $(SRC_DIR)/engine_lock.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_threadpool: $(TEST_DIR)/test_threadpool.c $(THREADPOOL_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

test_dict_cache: $(TEST_DIR)/test_dict_cache.c $(SRC_DIR)/dict_cache.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	./$@

# ---------------------------------------------------------------------------
# valgrind — W7 테스트 바이너리 회귀 (데몬은 상주형이라 미포함)
# ---------------------------------------------------------------------------
valgrind: $(TEST_PARSER_EXEC) $(STORAGE_TEST_TARGETS) test_bptree test_benchmark test_index_registry test_engine_lock test_threadpool test_dict_cache
	@echo "=== valgrind: parser/executor ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./$(TEST_PARSER_EXEC)
	@echo "=== valgrind: storage_insert ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./test_storage_insert
	@echo "=== valgrind: bptree ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./test_bptree
	@echo "=== valgrind: engine_lock ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./test_engine_lock
	@echo "=== valgrind: threadpool ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./test_threadpool
	@echo "=== valgrind: dict_cache ==="
	valgrind --leak-check=full --error-exitcode=1 --quiet ./test_dict_cache

# ---------------------------------------------------------------------------
# B+Tree pure 벤치 (W7 자산)
# ---------------------------------------------------------------------------
bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

$(BENCH_TARGET): $(BENCH_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---------------------------------------------------------------------------
# REPL — ANSI CLI 클라이언트 (HTTP SQL, 발표 백업 시연용)
#   MP0 시점엔 빌드만 검증. 정식 연결은 용 형님 server.c 머지 이후.
# ---------------------------------------------------------------------------
repl: $(REPL_TARGET)

$(REPL_TARGET): $(REPL_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

# ---------------------------------------------------------------------------
# loadtest — 추후 구현 placeholder
#   (용 형님 / 승진 PR 머지 후 동시 N 요청 스크립트 연결 예정)
# ---------------------------------------------------------------------------
loadtest:
	@echo "[loadtest] MP0 시점 placeholder. 추후 scripts/ 에 스크립트 추가 예정."

# ---------------------------------------------------------------------------
# clean
# ---------------------------------------------------------------------------
clean:
	rm -f $(DAEMON) $(DAEMON_TSAN) \
	      $(TEST_PARSER_EXEC) $(STORAGE_TEST_TARGETS) \
	      test_bptree test_benchmark test_index_registry test_engine_lock test_threadpool test_dict_cache \
	      $(BENCH_TARGET) tree_shape $(REPL_TARGET)
	rm -rf data/*.csv data/*.schema
	rm -rf data/schema data/tables
