CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -g -I./include
SRC_DIR = src
TEST_DIR = tests
BUILD_DIR = build

SRCS    = $(SRC_DIR)/main.c \
          $(SRC_DIR)/parser.c \
          $(SRC_DIR)/ast_print.c \
          $(SRC_DIR)/json_out.c \
          $(SRC_DIR)/sql_format.c \
          $(SRC_DIR)/executor.c \
          $(SRC_DIR)/storage.c

TEST_SRCS = $(TEST_DIR)/test_parser.c \
            $(TEST_DIR)/test_executor.c \
            $(SRC_DIR)/parser.c \
            $(SRC_DIR)/ast_print.c \
            $(SRC_DIR)/json_out.c \
            $(SRC_DIR)/sql_format.c \
            $(SRC_DIR)/executor.c \
            $(SRC_DIR)/storage.c

STORAGE_TEST_TARGETS = test_storage_insert test_storage_delete test_storage_update test_storage_select_result
STORAGE_TEST_DEPS = $(SRC_DIR)/storage.c
SELECT_RESULT_DEPS = $(SRC_DIR)/storage.c $(SRC_DIR)/parser.c

TARGET  = sqlparser
TEST_TARGET = test_runner

.PHONY: all clean test valgrind test_storage_all

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

$(TEST_TARGET): $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $(TEST_TARGET) $^

test_storage_insert: $(TEST_DIR)/test_storage_insert.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^
	./$@

test_storage_delete: $(TEST_DIR)/test_storage_delete.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^
	./$@

test_storage_update: $(TEST_DIR)/test_storage_update.c $(STORAGE_TEST_DEPS)
	$(CC) $(CFLAGS) -o $@ $^
	./$@

test_storage_select_result: $(TEST_DIR)/test_storage_select_result.c $(SELECT_RESULT_DEPS)
	$(CC) $(CFLAGS) -o $@ $^
	./$@

test_storage_all: $(STORAGE_TEST_TARGETS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)
	$(MAKE) test_storage_all

valgrind: $(TARGET)
	valgrind --leak-check=full --error-exitcode=1 ./$(TARGET) $(SQL)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(STORAGE_TEST_TARGETS)
	rm -f data/*.csv data/*.schema
	rm -f data/schema/*.schema data/tables/*.csv data/tables/*.csv.tmp

run:
	./$(TARGET) $(SQL)

json:
	./$(TARGET) $(SQL) --json
