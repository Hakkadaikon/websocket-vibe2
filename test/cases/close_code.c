// close code wire validity: bridges Lean P7 (ws_close_code_sendable).
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

static void test_close_forbidden(void) {
    CHECK(!ws_close_code_sendable(1005), "1005 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1006), "1006 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1015), "1015 forbidden on wire (P7)");
    CHECK(!ws_close_code_sendable(1004), "1004 reserved (P7)");
}

static void test_close_protocol(void) {
    CHECK(ws_close_code_sendable(1000), "1000 normal closure ok (P7)");
    CHECK(ws_close_code_sendable(1011), "1011 ok (P7)");
    CHECK(!ws_close_code_sendable(999), "999 below range (P7)");
    CHECK(!ws_close_code_sendable(1012), "1012 above protocol range (P7)");
}

static void test_close_app(void) {
    CHECK(ws_close_code_sendable(3000), "3000 app range ok (P7)");
    CHECK(ws_close_code_sendable(4999), "4999 app range ok (P7)");
    CHECK(!ws_close_code_sendable(5000), "5000 above app range (P7)");
}
