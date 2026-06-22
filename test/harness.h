// Minimal freestanding test harness: no libc. Provides _start, exit, write and
// a CHECK macro. Linux x86-64/aarch64 only (test/bench builds, not the SDK).
#ifndef WS_TEST_HARNESS_H
#define WS_TEST_HARNESS_H

#include <stddef.h>
#include <stdint.h>

static void sys_exit(int code);
static long sys_write(int fd, const void *buf, unsigned long len);

#if defined(__x86_64__)
static void sys_exit(int code) {
    register long rax __asm__("rax") = 60;
    register long rdi __asm__("rdi") = code;
    __asm__ volatile("syscall" : : "r"(rax), "r"(rdi) : "memory");
    __builtin_unreachable();
}
static long sys_write(int fd, const void *buf, unsigned long len) {
    register long rax __asm__("rax") = 1;
    register long rdi __asm__("rdi") = fd;
    register long rsi __asm__("rsi") = (long)buf;
    register long rdx __asm__("rdx") = (long)len;
    __asm__ volatile("syscall" : "+r"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
    return rax;
}
#elif defined(__aarch64__)
static void sys_exit(int code) {
    register long x8 __asm__("x8") = 93;
    register long x0 __asm__("x0") = code;
    __asm__ volatile("svc 0" : : "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
static long sys_write(int fd, const void *buf, unsigned long len) {
    register long x8 __asm__("x8") = 64;
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = (long)len;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
#else
#error "test harness supports x86-64 and aarch64 only"
#endif

static int ws_test_failures;

static void ws_put(const char *s) {
    unsigned long n = 0;
    while (s[n]) {
        n++;
    }
    (void)sys_write(2, s, n);
}

// One runnable check behind every non-trivial path.
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ws_test_failures++;                                                                    \
            ws_put("FAIL: " msg "\n");                                                             \
        }                                                                                          \
    } while (0)

void run_tests(void);

// At _start the stack is 16-byte aligned with no return address pushed, so a
// normal prologue mis-aligns SSE spills under -O2. Force realignment here.
#if defined(__x86_64__)
__attribute__((force_align_arg_pointer))
#endif
void
// NOLINTNEXTLINE(bugprone-reserved-identifier) — _start is the mandatory ELF entry point.
_start(void) {
    run_tests();
    sys_exit(ws_test_failures == 0 ? 0 : 1);
}

#endif
