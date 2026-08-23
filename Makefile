CC = gcc
CFLAGS = -g -Wall -Wextra -std=c99 -fPIC
LDFLAGS = -shared

# Library source files
LIB_SOURCES = arena.c csv_config.c csv_utils.c csv_parser.c csv_writer.c csv_reader.c \
              query/scanner.c query/parser.c query/expr.c query/query.c \
              query/eval.c query/aggregate.c query/sort.c query/dedupe.c \
              query/validate.c query/record.c query/executor.c
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
LIB_NAME = libcsv.so
STATIC_LIB = libcsv.a

# Build targets
.PHONY: all build static shared tests clean help csvql test test-grammar test-query test-arena test-config test-utils test-parser test-writer test-reader valgrind valgrind-all

# Line-editing library used only by the csvql CLI (kept out of libcsv)
LINENOISE_OBJ = query/deps/linenoise.o

all: build

build: shared static

shared: $(LIB_NAME)

static: $(STATIC_LIB)

$(LIB_NAME): $(LIB_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(STATIC_LIB): $(LIB_OBJECTS)
	ar rcs $@ $^

csvql: query/csvql.c $(STATIC_LIB) $(LINENOISE_OBJ)
	mkdir -p query/build
	$(CC) $(CFLAGS) -I. -Iquery -o query/build/csvql query/csvql.c $(LINENOISE_OBJ) $(STATIC_LIB) -lm

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

query/deps/linenoise.o: query/deps/linenoise.c query/deps/linenoise.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

# Test targets - delegate to tests/Makefile
tests:
	$(MAKE) -C tests all

test:
	$(MAKE) -C tests test

test-arena:
	$(MAKE) -C tests test-arena

test-config:
	$(MAKE) -C tests test-config

test-utils:
	$(MAKE) -C tests test-utils

test-parser:
	$(MAKE) -C tests test-parser

test-grammar: csvql
	mkdir -p query/build
	$(CC) $(CFLAGS) -o query/build/test_grammar query/test_grammar.c libcsv.a -lm
	./query/build/test_grammar

test-query: csvql
	mkdir -p query/build
	$(CC) $(CFLAGS) -o query/build/test_query query/test_query.c libcsv.a -lm
	./query/build/test_query

test-writer:
	$(MAKE) -C tests test-writer

test-reader:
	$(MAKE) -C tests test-reader

# Valgrind targets - delegate to tests/Makefile
valgrind:
	$(MAKE) -C tests valgrind

valgrind-all:
	$(MAKE) -C tests valgrind-all

valgrind-arena:
	$(MAKE) -C tests valgrind-arena

valgrind-config:
	$(MAKE) -C tests valgrind-config

valgrind-utils:
	$(MAKE) -C tests valgrind-utils

valgrind-parser:
	$(MAKE) -C tests valgrind-parser

valgrind-writer:
	$(MAKE) -C tests valgrind-writer

valgrind-reader:
	$(MAKE) -C tests valgrind-reader

clean:
	rm -f *.o *.debug.o *.gcov.o *.gcno *.gcda *.a *.so *.d
	rm -f $(LIB_NAME) $(STATIC_LIB)
	rm -f $(LINENOISE_OBJ)
	rm -rf query/build
	rm -f query/*.o query/*.gcov query/*.gcda query/*.gcno
	rm -f coverage.info profile.txt gmon.out
	rm -rf scan-build-results
	$(MAKE) -C tests clean

help:
	@echo "CSV Library Build System"
	@echo "========================"
	@echo "Library Targets:"
	@echo "  all          - Build shared and static libraries"
	@echo "  build        - Build shared and static libraries"
	@echo "  shared       - Build shared library (libcsv.so)"
	@echo "  static       - Build static library (libcsv.a)"
	@echo ""
	@echo "Test Targets:"
	@echo "  tests        - Build all test executables"
	@echo "  test         - Build and run all tests"
	@echo "  test-grammar - Build and run the SQL grammar suite (query/build/test_grammar)"
	@echo "  test-query   - Build and run the SQL integration checks (query/build/test_query)"
	@echo "  test-arena   - Run only arena tests"
	@echo "  test-config  - Run only CSV config tests"
	@echo "  test-utils   - Run only CSV utils tests"
	@echo "  test-parser  - Run only CSV parser tests"
	@echo "  test-writer  - Run only CSV writer tests"
	@echo "  test-reader  - Run only CSV reader tests"
	@echo ""
	@echo "Valgrind Targets:"
	@echo "  valgrind     - Run all tests under valgrind"
	@echo "  valgrind-all - Run all tests under valgrind (same as valgrind)"
	@echo "  valgrind-arena   - Run arena tests under valgrind"
	@echo "  valgrind-config  - Run config tests under valgrind"
	@echo "  valgrind-utils   - Run utils tests under valgrind"
	@echo "  valgrind-parser  - Run parser tests under valgrind"
	@echo "  valgrind-writer  - Run writer tests under valgrind"
	@echo "  valgrind-reader  - Run reader tests under valgrind"
	@echo ""
	@echo "CLI Tools:"
	@echo "  csvql        - Build the csvql CLI (REPL or csvql \"<sql>\") into query/build/csvql"
	@echo ""
	@echo "Utility Targets:"
	@echo "  clean        - Clean build artifacts"
	@echo "  help         - Show this help"

%.d: %.c
	@$(CC) $(CFLAGS) -MM -MT $(@:.d=.o) $< > $@ 
