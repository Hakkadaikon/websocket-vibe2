// libws trace aspect — the debug logging woven out of the main logic.
//
// Built only with -DWS_DEBUG (the justfile's `debug` target links it; default
// builds omit this TU entirely, so the trace points cost nothing). One line
// per join point goes to stderr via a raw write(2); no libc, no allocation.
//
// All formatting (int->decimal, string concat) is hand-rolled into a small
// stack buffer here, independent of io_posix.c's syscall wrapper.
#include "ws/trace.h"

#if defined(WS_DEBUG) && defined(__x86_64__)

#include "ws/ws.h" // ws_event_type / ws_state name tables

// ---- raw write(2), x86-64 (independent of io_posix.c) ----
static long sys_write2(int fd, const char *buf, unsigned long n) {
    long ret;
    register long r10 __asm__("r10") = 0;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"((long)n), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

// ---- hand-rolled formatting into a fixed line buffer ----
// Append a NUL-terminated string at p; return the new end. Cap-checked by the
// caller's buffer size (lines are short and bounded).
static char *put_str(char *p, const char *end, const char *s) {
    while (*s != '\0' && p < end) {
        *p++ = *s++;
    }
    return p;
}

// Emit n bytes of tmp in reverse (MSB-first), cap-checked.
static char *put_rev(char *p, const char *end, const char *tmp, int n) {
    while (n > 0 && p < end) {
        *p++ = tmp[--n];
    }
    return p;
}

// Append a non-negative decimal. Builds digits LSB-first, then emits reversed.
// Handles 0 explicitly via do-while.
static char *put_num(char *p, const char *end, unsigned long v) {
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    return put_rev(p, end, tmp, n);
}

// Append a signed decimal (detail may be -1).
static char *put_int(char *p, const char *end, long v) {
    if (v < 0) {
        p = put_str(p, end, "-");
        return put_num(p, end, (unsigned long)(-v));
    }
    return put_num(p, end, (unsigned long)v);
}

// ---- enum -> name tables (range-checked; keeps branching out of the body) ----
static const char *ev_name(int t) {
    static const char *const names[] = {"NONE", "MESSAGE", "PING", "PONG", "CLOSE", "ERROR"};
    if (t < 0 || t > WS_EV_ERROR) {
        return "?";
    }
    return names[t];
}

static const char *state_name(int s) {
    static const char *const names[] = {"CONNECTING", "OPEN", "CLOSING", "CLOSED"};
    if (s < 0 || s > WS_CLOSED) {
        return "?";
    }
    return names[s];
}

// ---- join-point sinks (declared in ws/trace.h) ----
void ws_trace_event(int ev_type, size_t len, uint16_t close_code) {
    char line[64];
    char *p = put_str(line, line + sizeof line, "[ws] event=");
    p = put_str(p, line + sizeof line, ev_name(ev_type));
    p = put_str(p, line + sizeof line, " len=");
    p = put_num(p, line + sizeof line, len);
    if (ev_type == WS_EV_CLOSE) {
        p = put_str(p, line + sizeof line, " code=");
        p = put_num(p, line + sizeof line, close_code);
    }
    p = put_str(p, line + sizeof line, "\n");
    (void)sys_write2(2, line, (unsigned long)(p - line));
}

void ws_trace_state(int from, int to, const char *why) {
    char line[80];
    char *p = put_str(line, line + sizeof line, "[ws] state ");
    p = put_str(p, line + sizeof line, state_name(from));
    p = put_str(p, line + sizeof line, " -> ");
    p = put_str(p, line + sizeof line, state_name(to));
    p = put_str(p, line + sizeof line, " (");
    p = put_str(p, line + sizeof line, why);
    p = put_str(p, line + sizeof line, ")\n");
    (void)sys_write2(2, line, (unsigned long)(p - line));
}

void ws_trace_io(const char *what, int fd, long detail) {
    char line[64];
    char *p = put_str(line, line + sizeof line, "[ws] io ");
    p = put_str(p, line + sizeof line, what);
    p = put_str(p, line + sizeof line, " fd=");
    p = put_int(p, line + sizeof line, fd);
    p = put_str(p, line + sizeof line, " detail=");
    p = put_int(p, line + sizeof line, detail);
    p = put_str(p, line + sizeof line, "\n");
    (void)sys_write2(2, line, (unsigned long)(p - line));
}

#elif defined(WS_DEBUG)

// Non-x86-64 debug build: keep the symbols, emit nothing (so the SDK still
// links on other arches). x86-64 is the supported target.
void ws_trace_event(int ev_type, size_t len, uint16_t close_code) {
    (void)ev_type;
    (void)len;
    (void)close_code;
}
void ws_trace_state(int from, int to, const char *why) {
    (void)from;
    (void)to;
    (void)why;
}
void ws_trace_io(const char *what, int fd, long detail) {
    (void)what;
    (void)fd;
    (void)detail;
}

#endif
