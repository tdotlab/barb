# barb: build and test.
#   make          build build/barb
#   make test     the core's unit tests, the agent scenarios and the line editor (all under the
#                 address and undefined-behaviour sanitizers), then the end-to-end test against a fake API
#   make clean
CC ?= gcc
CFLAGS ?= -O2
WARN = -std=gnu11 -Wall -Wextra -Werror
SAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

CORE = src/core/arena.c src/core/str.c src/core/json.c src/core/api.c src/core/agent.c src/core/tools.c
POSIX = src/posix/ops.c src/posix/main.c

all: build/barb

build/barb: $(CORE) $(POSIX) $(wildcard src/core/*.h src/posix/*.h)
	@mkdir -p build
	$(CC) $(WARN) $(CFLAGS) $(CORE) $(POSIX) -o $@

build/test_core: tests/test_core.c tests/check.h $(CORE)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_core.c src/core/arena.c src/core/str.c src/core/json.c -o $@

build/test_api: tests/test_api.c tests/check.h $(CORE)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_api.c src/core/arena.c src/core/str.c src/core/json.c src/core/api.c -o $@

build/test_agent: tests/test_agent.c tests/harness.h tests/check.h $(CORE) $(wildcard src/core/*.h)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_agent.c $(CORE) -o $@

# The Windows line editor is pure bytes (no system calls), so it is tested here too.
build/test_edit: tests/test_edit.c tests/check.h src/win32/edit.c src/win32/edit.h
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_edit.c src/win32/edit.c -o $@

# The same program with the sanitizers on, for the end-to-end test.
build/barb-san: $(CORE) $(POSIX) $(wildcard src/core/*.h src/posix/*.h)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) $(CORE) $(POSIX) -o $@

test: build/test_core build/test_api build/test_agent build/test_edit build/barb-san
	build/test_core
	build/test_api
	build/test_agent
	build/test_edit
	python3 tests/e2e.py build/barb-san

clean:
	rm -rf build

.PHONY: all test clean
