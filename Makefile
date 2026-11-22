# barb: build and test.
#   make          build build/barb
#   make test     the core's unit tests, the agent scenarios, the line editor, the key decoder,
#                 the markdown renderer and the screen (all under the address and undefined-behaviour sanitizers), then the end-to-end
#                 test against a fake API
#   make clean
CC ?= gcc
CFLAGS ?= -O2
WARN = -std=gnu11 -Wall -Wextra -Werror
SAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all

CORE = src/core/arena.c src/core/str.c src/core/json.c src/core/api.c src/core/agent.c src/core/tools.c
UI = src/ui/edit.c src/ui/keys.c src/ui/md.c src/ui/screen.c
POSIX = src/posix/ops.c src/posix/main.c
HEADERS = $(wildcard src/core/*.h src/ui/*.h src/posix/*.h)

all: build/barb

build/barb: $(CORE) $(UI) $(POSIX) $(HEADERS)
	@mkdir -p build
	$(CC) $(WARN) $(CFLAGS) $(CORE) $(UI) $(POSIX) -o $@

build/test_core: tests/test_core.c tests/check.h $(CORE)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_core.c src/core/arena.c src/core/str.c src/core/json.c -o $@

build/test_api: tests/test_api.c tests/check.h $(CORE)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_api.c src/core/arena.c src/core/str.c src/core/json.c src/core/api.c -o $@

build/test_agent: tests/test_agent.c tests/harness.h tests/check.h $(CORE) $(wildcard src/core/*.h)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_agent.c $(CORE) -o $@

build/test_edit: tests/test_edit.c tests/check.h tests/term.h src/ui/edit.c src/ui/edit.h
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_edit.c src/ui/edit.c -o $@

build/test_keys: tests/test_keys.c tests/check.h src/ui/keys.c src/ui/keys.h src/ui/edit.h
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_keys.c src/ui/keys.c -o $@

build/test_md: tests/test_md.c tests/check.h src/ui/md.c src/ui/md.h src/ui/edit.c src/ui/edit.h
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_md.c src/ui/md.c src/ui/edit.c src/core/str.c src/core/arena.c -o $@

build/test_screen: tests/test_screen.c tests/check.h tests/term.h $(UI) $(HEADERS)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) tests/test_screen.c src/ui/screen.c src/ui/md.c src/ui/edit.c src/core/str.c src/core/arena.c -o $@

# The same program with the sanitizers on, for the end-to-end test.
build/barb-san: $(CORE) $(UI) $(POSIX) $(HEADERS)
	@mkdir -p build
	$(CC) $(WARN) $(SAN) $(CORE) $(UI) $(POSIX) -o $@

test: build/test_core build/test_api build/test_agent build/test_edit build/test_keys build/test_md build/test_screen build/barb-san
	build/test_core
	build/test_api
	build/test_agent
	build/test_edit
	build/test_keys
	build/test_md
	build/test_screen
	python3 tests/e2e.py build/barb-san

clean:
	rm -rf build

.PHONY: all test clean
