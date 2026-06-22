#include "../src/ws_internal.h"
#include "harness.h"

static void test_mem(void) {
    unsigned char a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char b[8];
    ws_memset(b, 0, sizeof b);
    CHECK(b[0] == 0 && b[7] == 0, "memset zeroes");
    ws_memcpy(b, a, sizeof a);
    CHECK(ws_memcmp(a, b, sizeof a) == 0, "memcpy then memcmp equal");
    b[3] = 99;
    CHECK(ws_memcmp(a, b, sizeof a) != 0, "memcmp detects diff");
    CHECK(ws_memcmp(a, b, 3) == 0, "memcmp respects length");
}

void run_tests(void) { test_mem(); }
