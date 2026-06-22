// freestanding mem primitives. clang may emit calls to these even with
// -fno-builtin (e.g. struct copies), so we must define them ourselves.
#include "../ws_internal.h"

void *ws_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *ws_memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

int ws_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}

// Aliases the compiler/linker expects by these exact names.
void *memcpy(void *dst, const void *src, size_t n) {
    return ws_memcpy(dst, src, n);
}
void *memset(void *dst, int c, size_t n) {
    return ws_memset(dst, c, n);
}
int memcmp(const void *a, const void *b, size_t n) {
    return ws_memcmp(a, b, n);
}
