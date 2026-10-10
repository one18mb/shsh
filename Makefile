CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Isrc -D_GNU_SOURCE

COMMON = src/common.c src/buf.c src/proto.c src/parse.c
SRVLIB = src/pty.c src/exec.c src/compl.c
HDRS   = src/common.h src/buf.h src/proto.h

BIN = bin/shshd bin/shsh

all: $(BIN)

bin/shshd: src/shshd.c src/server.c src/server.h $(COMMON) $(SRVLIB) $(HDRS)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/shshd.c src/server.c $(COMMON) $(SRVLIB)

bin/shsh: src/shsh.c src/client.c src/client.h $(COMMON) $(HDRS)
	@mkdir -p bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/shsh.c src/client.c $(COMMON)

debug: CFLAGS = -std=c11 -g -O0 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer
debug: all

test: all
	python3 tests/run_tests.py

clean:
	rm -rf bin

.PHONY: all debug test clean
