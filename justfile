# WebSocket protocol stack SDK — freestanding C23

cc := "clang"
# freestanding: no libc, no hosted runtime. -O2 for realistic bench.
cflags := "-std=c23 -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude"
srcs := "src/mem.c src/mask.c src/frame.c src/handshake.c src/sha1.c src/base64.c src/utf8.c src/lifecycle.c src/stream.c"
# io_posix.c (epoll runtime) links into the example but not the test binary
# (its raw-syscall server would clash with the test harness's own _start).
io_srcs := "src/io_posix.c"

default: check

# Build the test binary (harness provides its own _start, links -nostdlib).
build:
    mkdir -p build
    {{cc}} {{cflags}} -static {{srcs}} test/test.c -o build/test

# Run self-checking tests (exit 0 == all CHECKs passed).
test: build
    ./build/test && echo "PASS"

lint:
    clang-tidy {{srcs}} {{io_srcs}} test/test.c example/echo_server.c -- {{cflags}}
    # trace.c is an empty TU without -DWS_DEBUG, so tidy it with the flag set.
    clang-tidy src/trace.c -- {{cflags}} -DWS_DEBUG

fmt:
    clang-format --dry-run --Werror src/*.c include/ws/*.h test/*.c bench/*.c example/*.c

fmt-fix:
    clang-format -i src/*.c include/ws/*.h test/*.c bench/*.c example/*.c

# Cyclomatic complexity must stay <= 3 (C sources only; -l cpp skips the
# Python test client living under example/).
ccn:
    lizard -l cpp -C 3 -w src include example

# Build the echo server example (freestanding, links the SDK).
example:
    mkdir -p build
    {{cc}} {{cflags}} -static {{srcs}} {{io_srcs}} example/echo_server.c -o build/echo-server

# Run the echo server on :8080 (Ctrl-C to stop).
example-run: example
    ./build/echo-server

# Debug build: weave in the trace aspect (-DWS_DEBUG + src/trace.c). Default
# builds stay no-op; this one writes one [ws] line per join point to stderr.
debug:
    mkdir -p build
    {{cc}} {{cflags}} -DWS_DEBUG -static {{srcs}} {{io_srcs}} src/trace.c example/echo_server.c -o build/echo-server-debug

# Run the debug server (trace goes to stderr).
debug-run: debug
    ./build/echo-server-debug

# Throughput benchmark (masking + frame parse).
bench:
    mkdir -p build
    {{cc}} {{cflags}} -static {{srcs}} bench/bench.c -o build/bench
    ./build/bench

# Re-model-check the TLA+ designs (lifecycle INV1..6, streaming SINV1..8).
verify-design:
    mkdir -p "${TMPDIR:-/tmp}/tla"
    cd spec && _JAVA_OPTIONS="-Djava.io.tmpdir=${TMPDIR:-/tmp}/tla" tlc -config WsLifecycle.cfg WsLifecycle.tla
    cd spec && _JAVA_OPTIONS="-Djava.io.tmpdir=${TMPDIR:-/tmp}/tla" tlc -config WsStream.cfg WsStream.tla

# Re-check the Lean proofs (P1..P8) behind the byte/bit-level code.
verify-proofs:
    cd proofs/WsProto && lake build

# Both formal layers.
verify: verify-design verify-proofs

# Everything CI cares about.
check: fmt ccn lint test
