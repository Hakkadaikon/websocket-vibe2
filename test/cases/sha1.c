// sha1 (RFC 3174): known-answer vectors incl. a 2-block padding case.
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

static void test_sha1(void) {
    uint8_t out[20];

    // "abc" -> a9993e364706816aba3e25717850c26c9cd0d89d
    const uint8_t abc[] = {'a', 'b', 'c'};
    const uint8_t abc_h[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                               0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    ws_sha1(abc, 3, out);
    CHECK(ws_memcmp(out, abc_h, 20) == 0, "sha1(abc)");

    // "" -> da39a3ee5e6b4b0d3255bfef95601890afd80709
    const uint8_t empty_h[20] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
                                 0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09};
    ws_sha1((const uint8_t *)"", 0, out);
    CHECK(ws_memcmp(out, empty_h, 20) == 0, "sha1(empty)");

    // 56-byte message forces a second padding block (RFC 3174 §4).
    const uint8_t m56[56] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3',
                             '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7',
                             '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1',
                             '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5'};
    const uint8_t m56_h[20] = {0x0a, 0x40, 0xb8, 0xfb, 0xda, 0xaf, 0xb7, 0xc2, 0x96, 0x51,
                               0x61, 0x8a, 0xc1, 0x5d, 0x27, 0xe7, 0x72, 0x28, 0x71, 0x30};
    ws_sha1(m56, 56, out);
    CHECK(ws_memcmp(out, m56_h, 20) == 0, "sha1(56-byte, 2-block pad)");
}
