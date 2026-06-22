// frame: classify (Lean P5), parse_header lengths/errors (RFC 6455 §5.2; P3/P4/P6),
// and build_header + roundtrip (P3/P4).
// #include'd by test/test.c (single TU; ws/ws.h + harness already in scope).

static void test_classify_data(void) {
    CHECK(ws_classify_opcode(0x0) == WS_CLASS_DATA, "0x0 cont = data (P5)");
    CHECK(ws_classify_opcode(0x1) == WS_CLASS_DATA, "0x1 text = data (P5)");
    CHECK(ws_classify_opcode(0x2) == WS_CLASS_DATA, "0x2 bin = data (P5)");
}

static void test_classify_control(void) {
    CHECK(ws_classify_opcode(0x8) == WS_CLASS_CONTROL, "0x8 close = control (P5)");
    CHECK(ws_classify_opcode(0x9) == WS_CLASS_CONTROL, "0x9 ping = control (P5)");
    CHECK(ws_classify_opcode(0xA) == WS_CLASS_CONTROL, "0xA pong = control (P5)");
}

static void test_classify_reserved(void) {
    CHECK(ws_classify_opcode(0x3) == WS_CLASS_RESERVED, "0x3 = reserved (P5)");
    CHECK(ws_classify_opcode(0xB) == WS_CLASS_RESERVED, "0xB = reserved (P5)");
    CHECK(ws_classify_opcode(0xF) == WS_CLASS_RESERVED, "0xF = reserved (P5)");
}

// Predicate helpers keep the && out of CHECK (cognitive complexity). Written as
// guard chains so neither bare bool operands nor `== true` trip clang-tidy.
static _Bool tiny_ok(const ws_frame_header *h) {
    if (!h->fin) {
        return false;
    }
    if (h->masked) {
        return false;
    }
    return (_Bool)(h->opcode == WS_OP_TEXT && h->payload_len == 5);
}
static _Bool masked_ok(const ws_frame_header *h) {
    if (!h->masked) {
        return false;
    }
    return (_Bool)(h->payload_len == 3 && h->opcode == WS_OP_BIN && h->mask_key[0] == 1 &&
                   h->mask_key[3] == 4);
}

static void test_parse_lengths(void) {
    ws_frame_header h;
    const uint8_t tiny[] = {0x81, 0x05};
    CHECK(ws_parse_header(tiny, 2, &h) == 2, "tiny unmasked header = 2 bytes");
    CHECK(tiny_ok(&h), "tiny fields");

    const uint8_t masked[] = {0x82, 0x83, 0x01, 0x02, 0x03, 0x04};
    CHECK(ws_parse_header(masked, 6, &h) == 6, "masked tiny header = 6 bytes");
    CHECK(masked_ok(&h), "masked fields + key");

    const uint8_t s[] = {0x81, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(s, 4, &h) == 4, "short header = 4 bytes");
    CHECK(h.payload_len == 200, "short len 200");

    const uint8_t l[] = {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0};
    CHECK(ws_parse_header(l, 10, &h) == 10, "long header = 10 bytes");
    CHECK(h.payload_len == 0x10000, "long len 0x10000");
}

static void test_parse_errors(void) {
    ws_frame_header h;
    const uint8_t tiny[] = {0x81, 0x05};
    const uint8_t s[] = {0x81, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(tiny, 1, &h) == WS_ERR_NEED_MORE, "1 byte = need more");
    CHECK(ws_parse_header(s, 3, &h) == WS_ERR_NEED_MORE, "short missing len = need more");

    const uint8_t bigctl[] = {0x88, 126, 0x00, 0xC8};
    CHECK(ws_parse_header(bigctl, 4, &h) == WS_ERR_PROTOCOL, "control >125 rejected (P6)");
    const uint8_t okctl[] = {0x88, 125};
    CHECK(ws_parse_header(okctl, 2, &h) == 2, "control =125 ok (P6)");

    const uint8_t msb[] = {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0};
    CHECK(ws_parse_header(msb, 10, &h) == WS_ERR_PROTOCOL, "127 MSB set rejected");
}

// The masked tiny frame parsed back keeps its mask flag and key bytes.
static _Bool masked_key_ok(const ws_frame_header *r) {
    if (!r->masked) {
        return false;
    }
    return (_Bool)(r->mask_key[0] == 0xDE && r->mask_key[3] == 0xEF);
}

// One build+parse roundtrip at a given length; returns true if it survives.
static _Bool roundtrip(uint64_t plen, int want_hdr) {
    uint8_t buf[14];
    ws_frame_header h = {0};
    ws_frame_header r;
    h.fin = true;
    h.opcode = WS_OP_TEXT;
    h.payload_len = plen;
    int n = ws_build_header(&h, buf, sizeof buf);
    if (n != want_hdr) {
        return false;
    }
    return (_Bool)(ws_parse_header(buf, (size_t)n, &r) == n && r.payload_len == plen);
}

static void test_build_header(void) {
    CHECK(roundtrip(125, 2), "roundtrip tiny 125 (2 bytes)");
    CHECK(roundtrip(126, 4), "roundtrip 126 (short 4 bytes)");
    CHECK(roundtrip(0xFFFF, 4), "roundtrip 0xFFFF (short 4 bytes)");
    CHECK(roundtrip(0x10000, 10), "roundtrip 0x10000 (long 10 bytes)");

    // masked build -> +4 bytes, key preserved.
    uint8_t buf[14];
    ws_frame_header h = {0};
    ws_frame_header r;
    h.fin = true;
    h.opcode = WS_OP_BIN;
    h.payload_len = 5;
    h.masked = true;
    h.mask_key[0] = 0xDE;
    h.mask_key[3] = 0xEF;
    CHECK(ws_build_header(&h, buf, sizeof buf) == 6, "build masked tiny = 6 bytes");
    CHECK(ws_parse_header(buf, 6, &r) == 6, "parse masked back");
    CHECK(masked_key_ok(&r), "masked key preserved");

    CHECK(ws_build_header(&h, buf, 1) == WS_ERR_TOO_SMALL, "build cap too small");
}
