// handshake accept (RFC 6455 §4.2.2 worked example).
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

static void test_handshake(void) {
    const uint8_t key[] = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *want = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    uint8_t out[28];
    int n = ws_handshake_accept(key, 24, out);
    CHECK(n == 28, "accept length = 28");
    CHECK(ws_memcmp(out, want, 28) == 0, "accept = RFC 4.2.2 example");
}
