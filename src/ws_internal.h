// Internal declarations shared across SDK translation units (freestanding).
#ifndef WS_INTERNAL_H
#define WS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

void *ws_memcpy(void *dst, const void *src, size_t n);
void *ws_memset(void *dst, int c, size_t n);
int ws_memcmp(const void *a, const void *b, size_t n);

#endif
