# WebSocket protocol stack SDK — freestanding C23

cc := "clang"
# freestanding: no libc, no hosted runtime. -O2 for realistic bench.
cflags := "-std=c23 -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude"
srcs := "src/mem.c src/mask.c src/frame.c src/handshake.c src/sha1.c src/base64.c src/utf8.c src/lifecycle.c"

default: check

# Build the test binary (harness provides its own _start, links -nostdlib).
build:
    mkdir -p build
    {{cc}} {{cflags}} -static {{srcs}} test/test.c -o build/test

# Run self-checking tests (exit 0 == all CHECKs passed).
test: build
    ./build/test && echo "PASS"

lint:
    clang-tidy {{srcs}} test/test.c -- {{cflags}}

fmt:
    clang-format --dry-run --Werror src/*.c include/*.h test/*.c bench/*.c

fmt-fix:
    clang-format -i src/*.c include/*.h test/*.c bench/*.c

# Cyclomatic complexity must stay <= 3.
ccn:
    lizard -C 3 -w src include

# Throughput benchmark (masking + frame parse).
bench:
    mkdir -p build
    {{cc}} {{cflags}} -static {{srcs}} bench/bench.c -o build/bench
    ./build/bench

# Re-check the Lean proofs and TLA+ design behind the implementation.
verify-proofs:
    cd proofs/WsProto && lake build

# Everything CI cares about.
check: fmt ccn lint test
