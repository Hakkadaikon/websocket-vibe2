# WebSocket protocol stack SDK — freestanding C23

cc := "clang"
# freestanding: no libc, no stdlib, hosted runtime off. -O2 for bench realism.
cflags := "-std=c23 -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude"
srcs := "src/mem.c src/sha1.c src/base64.c src/utf8.c src/ws_handshake.c src/ws_frame.c"

# Build the test binary (test harness provides its own _start, links nostdlib).
build:
    {{cc}} {{cflags}} -static {{srcs}} test/test.c -o build/test

# Run self-checking tests (asserts via custom trap; exit 0 == pass).
test: build
    ./build/test && echo "PASS"

# Static analysis.
lint:
    clang-tidy {{srcs}} -- {{cflags}}

# Format check (fails if unformatted).
fmt:
    clang-format --dry-run --Werror src/*.c include/*.h test/*.c bench/*.c

fmt-fix:
    clang-format -i src/*.c include/*.h test/*.c bench/*.c

# Cyclomatic complexity must stay <= 3.
ccn:
    lizard -C 3 -w src include

# Throughput benchmark (mask + parse).
bench:
    {{cc}} {{cflags}} -static {{srcs}} bench/bench.c -o build/bench
    ./build/bench

# Everything CI cares about.
check: fmt ccn lint test
