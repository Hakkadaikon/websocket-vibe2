#include "../src/ws_internal.h"
#include "ws.h"

#include "harness.h"

// ---- mem ----
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

// ---- masking: bridges Lean P1/P2 ----
static void test_mask(void) {
    const uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};

    // i mod 4 application on a known vector (more than 4 bytes to exercise wrap).
    uint8_t buf[6] = {0x00, 0x00, 0x00, 0x00, 0xff, 0xff};
    ws_mask(buf, 6, key);
    CHECK(buf[0] == 0x37 && buf[1] == 0xfa && buf[2] == 0x21 && buf[3] == 0x3d,
          "mask applies key[i] for i<4");
    CHECK(buf[4] == (0xff ^ 0x37) && buf[5] == (0xff ^ 0xfa), "mask wraps key at i mod 4 (P-impl)");

    // P1 involution: masking twice with the same key restores the original.
    uint8_t orig[13] = {'H', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', 0x80};
    uint8_t work[13];
    ws_memcpy(work, orig, 13);
    ws_mask(work, 13, key);
    CHECK(ws_memcmp(work, orig, 13) != 0, "mask changes data");
    ws_mask(work, 13, key);
    CHECK(ws_memcmp(work, orig, 13) == 0, "mask involution: unmask(mask)=id (Lean P1)");

    // P2 length preservation: bytes outside [0,len) untouched.
    uint8_t guard[5] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    ws_mask(guard, 3, key);
    CHECK(guard[3] == 0xAA && guard[4] == 0xAA, "mask touches only len bytes (Lean P2)");

    // len 0 is a no-op.
    ws_mask(guard, 0, key);
    CHECK(guard[0] == (0xAA ^ 0x37), "mask len 0 is a no-op");
}

void run_tests(void) {
    test_mem();
    test_mask();
}
