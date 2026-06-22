// base64 encode (standard alphabet, padded).
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

static _Bool b64_eq(const uint8_t *in, size_t len, const char *want) {
    uint8_t out[64];
    size_t n = ws_base64(in, len, out);
    size_t wlen = 0;
    while (want[wlen]) {
        wlen++;
    }
    return (_Bool)(n == wlen && ws_memcmp(out, want, wlen) == 0);
}

static void test_base64(void) {
    CHECK(b64_eq((const uint8_t *)"abc", 3, "YWJj"), "b64 abc (no pad)");
    CHECK(b64_eq((const uint8_t *)"", 0, ""), "b64 empty");
    CHECK(b64_eq((const uint8_t *)"a", 1, "YQ=="), "b64 1 byte (==)");
    CHECK(b64_eq((const uint8_t *)"ab", 2, "YWI="), "b64 2 bytes (=)");

    // 20-byte sha1-sized input -> 28 chars (ceil(20/3)*4), one trailing '='.
    const uint8_t twenty[20] = {0};
    uint8_t out[64];
    size_t n = ws_base64(twenty, 20, out);
    CHECK(n == 28, "b64 20 bytes -> 28 chars");
    CHECK(out[27] == '=', "b64 20 bytes one trailing pad");
    CHECK(out[26] != '=', "b64 20 bytes only one pad");
}
