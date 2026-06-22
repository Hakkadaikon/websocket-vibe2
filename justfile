# WebSocket protocol stack SDK — freestanding C23

cc := "clang"
# freestanding: no libc, no hosted runtime. -O2 for realistic bench.
cflags := "-std=c23 -ffreestanding -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -Iinclude"
# Sources by layer (low -> high): util -> crypto -> framing -> session.
srcs := "src/util/mem.c src/util/mask.c src/crypto/sha1.c src/crypto/base64.c src/framing/frame.c src/framing/handshake.c src/framing/utf8.c src/session/lifecycle.c src/session/stream.c"
# io layer (epoll runtime) links into the example but not the test binary
# (its raw-syscall server would clash with the test harness's own _start).
io_srcs := "src/io/io_posix.c"

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
    clang-format --dry-run --Werror src/*.c src/*/*.c include/ws/*.h test/*.c test/cases/*.c bench/*.c example/*.c

fmt-fix:
    clang-format -i src/*.c src/*/*.c include/ws/*.h test/*.c test/cases/*.c bench/*.c example/*.c

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

# Run a TLA+-derived .feature as an executable test against the implementation.
# feature_to_c.py translates the machine-generated Gherkin (the form
# trace_to_gherkin.py emits: `When <Action>` / `Then <var> becomes <value>`)
# into CHECK assertions that drive the verified ws_lc_* state machine, then
# links them with the freestanding harness. Hand-written prose features are out
# of scope (they stay in test/test.c). Usage: just bdd spec/Foo.feature
bdd FEATURE:
    mkdir -p build
    python3 "${LOOPENG_HOME:-$HOME/.config/loopeng}/bin/feature_to_c.py" --map spec/bdd_map.json {{FEATURE}} > build/generated_bdd.c
    {{cc}} {{cflags}} -static {{srcs}} test/bdd_main.c \
        -DBDD_GENERATED='"'"$(pwd)/build/generated_bdd.c"'"' -o build/bdd
    ./build/bdd && echo "BDD PASS"

# Fast gate: formatting, complexity, lint, and the self-checking tests.
check: fmt ccn lint test

# Full CI gate: the fast checks plus both formal-verification layers (TLA+
# design model-check and Lean proofs). This is what CI should enforce so the
# three-layer verification is never silently skipped.
ci: check verify
