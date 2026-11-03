CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror

.PHONY: all test rust clean
all: build/udp-server build/test-vsfs

build:
	mkdir -p build

build/udp-server: udp/server.c fs/vsfs.c fs/vsfs.h | build
	$(CC) $(CFLAGS) -pthread udp/server.c fs/vsfs.c -o $@

build/test-vsfs: tests/test_vsfs.c fs/vsfs.c fs/vsfs.h | build
	$(CC) $(CFLAGS) tests/test_vsfs.c fs/vsfs.c -o $@

test: all
	./build/test-vsfs
	python3 -m unittest discover -s tests -p 'test_*.py' -v

rust:
	cd rust-web && cargo build && cargo test

clean:
	rm -rf build rust-web/target
