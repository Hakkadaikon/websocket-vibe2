// utf8 validation (RFC 6455 §8.1; Lean P8). Vectors mirror proofs/WsProto/Utf8.lean
// exactly so code <-> proof stay pinned.
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

// reject: overlong / surrogate / out-of-range (Lean reject_* theorems).
static void test_utf8_reject(void) {
    const uint8_t over2[] = {0xC0, 0x80};
    const uint8_t over3[] = {0xE0, 0x80, 0x80};
    const uint8_t surr[] = {0xED, 0xA0, 0x80};
    const uint8_t above[] = {0xF4, 0x90, 0x80, 0x80};
    CHECK(!ws_utf8_valid(over2, 2), "reject 2B overlong C0 80 (Lean reject_overlong2)");
    CHECK(!ws_utf8_valid(over3, 3), "reject 3B overlong E0 80 80 (Lean reject_overlong3)");
    CHECK(!ws_utf8_valid(surr, 3), "reject surrogate ED A0 80 (Lean reject_surrogate)");
    CHECK(!ws_utf8_valid(above, 4), "reject >U+10FFFF F4 90 80 80 (Lean reject_above_max)");
}

// accept: 1/2/3/4-byte + empty (Lean accept_* theorems).
static void test_utf8_accept(void) {
    const uint8_t dollar[] = {0x24};
    const uint8_t cent[] = {0xC2, 0xA2};
    const uint8_t euro[] = {0xE2, 0x82, 0xAC};
    const uint8_t clef[] = {0xF0, 0x9D, 0x84, 0x9E};
    CHECK(ws_utf8_valid(dollar, 1), "accept $ (Lean accept_dollar)");
    CHECK(ws_utf8_valid(cent, 2), "accept cent (Lean accept_cent)");
    CHECK(ws_utf8_valid(euro, 3), "accept euro (Lean accept_euro)");
    CHECK(ws_utf8_valid(clef, 4), "accept clef U+1D11E (Lean accept_clef)");
    CHECK(ws_utf8_valid((const uint8_t *)"", 0), "accept empty");
}

// boundary: lone continuation / truncated multibyte / F5 / ASCII run.
static void test_utf8_boundary(void) {
    const uint8_t lone[] = {0x80};
    const uint8_t trunc[] = {0xC2};
    const uint8_t f5[] = {0xF5, 0x80, 0x80, 0x80};
    CHECK(!ws_utf8_valid(lone, 1), "reject lone continuation 80");
    CHECK(!ws_utf8_valid(trunc, 1), "reject truncated 2B C2");
    CHECK(!ws_utf8_valid(f5, 4), "reject F5 lead byte");
    CHECK(ws_utf8_valid((const uint8_t *)"hello", 5), "accept ASCII run");
}
