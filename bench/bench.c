// Throughput micro-benchmark for the hot paths: ws_mask and ws_parse_header.
// Freestanding (no libc): own _start, raw write syscall, rdtsc timing, and a
// hand-rolled uint->decimal printer. We report cycles/byte and cycles/frame —
// rdtsc has no fixed frequency here, so cycles are the honest unit (no MiB/s).
#include "ws.h"

#include <stddef.h>
#include <stdint.h>

#if !defined(__x86_64__)
#error "bench uses rdtsc; x86-64 only"
#endif

static void sys_exit(int code) {
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = code;
    __asm__ volatile("syscall" : : "r"(rax), "r"(rdi) : "memory");
    __builtin_unreachable();
}

static void sys_write(const void *buf, unsigned long len) {
    register long rax __asm__("rax") = 1;
    register long rdi __asm__("rdi") = 2; // stderr
    register long rsi __asm__("rsi") = (long)buf;
    register long rdx __asm__("rdx") = (long)len;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
}

// Serializing cycle counter (rdtscp drains the pipeline so timing is tight).
static uint64_t rdtsc(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "rcx", "memory");
    return ((uint64_t)hi << 32) | lo;
}

static void put(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    sys_write(s, n);
}

// Print an unsigned as decimal. Builds digits back-to-front in a fixed buffer.
static void put_u64(uint64_t v) {
    char buf[20];
    size_t i = sizeof buf;
    do {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    sys_write(buf + i, sizeof buf - i);
}

// Report "<label> <whole>.<centi> cycles/<unit>" from total cycles over count
// items, as fixed-point hundredths so sub-cycle costs stay visible.
static void report(const char *label, uint64_t cycles, uint64_t count, const char *unit) {
    uint64_t centi = (cycles * 100) / count;
    put(label);
    put_u64(centi / 100);
    put(".");
    put_u64(centi % 100 / 10);
    put_u64(centi % 10);
    put(" cycles/");
    put(unit);
    put("\n");
}

enum { BUF = 64 * 1024, ITERS = 4096 };
static uint8_t g_buf[BUF];

// Mask the whole buffer ITERS times; return total cycles. The XOR result feeds
// the next round so the optimizer cannot hoist the loop away.
static uint64_t bench_mask(void) {
    const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
    uint64_t t0 = rdtsc();
    for (int n = 0; n < ITERS; n++) {
        ws_mask(g_buf, BUF, key);
    }
    return rdtsc() - t0;
}

// Parse a tiny frame header ITERS times; return total cycles. `out` is observed
// via a checksum so the call is not dead code.
static uint64_t bench_parse(uint64_t *sink) {
    const uint8_t frame[6] = {0x82, 0x83, 0x01, 0x02, 0x03, 0x04};
    ws_frame_header h;
    uint64_t acc = 0;
    uint64_t t0 = rdtsc();
    for (int n = 0; n < ITERS; n++) {
        acc += (uint64_t)ws_parse_header(frame, sizeof frame, &h) + h.payload_len;
    }
    *sink = acc;
    return rdtsc() - t0;
}

#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the ELF entry point.
_start(void) {
    uint64_t mask_cycles = bench_mask();
    uint64_t sink = 0;
    uint64_t parse_cycles = bench_parse(&sink);
    report("mask:  ", mask_cycles, (uint64_t)BUF * ITERS, "byte");
    report("parse: ", parse_cycles, (uint64_t)ITERS, "frame");
    sys_exit(sink == 0 ? 1 : 0); // sink != 0 proves the parse loop ran
}
