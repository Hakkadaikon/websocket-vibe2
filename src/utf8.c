// UTF-8 validation (RFC 6455 §8.1 / RFC 3629). Verified core: Lean WsProto.Utf8
// P8 (range soundness). The decode step mirrors Lean utf8DecodeStep 1:1 (lead
// classify -> first-continuation guard -> tail continuations) so the bridge
// vectors in test/test.c pin code <-> proof.
#include "ws/ws.h"

// Inclusive byte-range test (kept separate so the comparisons don't inflate the
// cyclomatic complexity of the callers; CCN budget is <= 3).
static _Bool in_range(uint8_t b, uint8_t lo, uint8_t hi) {
    return (_Bool)(b >= lo && b <= hi);
}

// Trailing continuation-byte count per lead byte, 255 = not a valid lead byte.
// Mirrors the lead-byte rows of Lean utf8DecodeStep: 0x00..0x7F->0, 0xC2..0xDF
// ->1, 0xE0..0xEF->2, 0xF0..0xF4->3 (0xC0/0xC1 and 0xF5..0xFF are 255). Table
// keeps the classify branch-free (CCN 1).
// clang-format off
static const uint8_t kContCount[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
    2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   2,
    3,   3,   3,   3,   3,   255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};
// clang-format on

// Inclusive low bound for the FIRST continuation byte (overlong guards):
// 0xE0 -> 0xA0, 0xF0 -> 0x90, else plain 0x80. Mirrors Lean's g1.
static uint8_t first_lo(uint8_t b0) {
    if (b0 == 0xE0) {
        return 0xA0;
    }
    return (b0 == 0xF0) ? 0x90 : 0x80;
}

// Inclusive high bound for the FIRST continuation byte (surrogate / U+10FFFF
// guards): 0xED -> 0x9F, 0xF4 -> 0x8F, else plain 0xBF. Mirrors Lean's g1.
static uint8_t first_hi(uint8_t b0) {
    if (b0 == 0xED) {
        return 0x9F;
    }
    return (b0 == 0xF4) ? 0x8F : 0xBF;
}

// Every byte in p[0..n) must be a plain continuation byte (0x80..0xBF).
static _Bool conts_ok(const uint8_t *p, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) {
        if (!in_range(p[i], 0x80, 0xBF)) {
            return false;
        }
    }
    return true;
}

// The first continuation byte must lie in the guarded range; the rest are plain.
static _Bool tail_ok(const uint8_t *data, size_t i, uint8_t b0, uint8_t cont) {
    if (!in_range(data[i + 1], first_lo(b0), first_hi(b0))) {
        return false;
    }
    return conts_ok(data + i + 2, (uint8_t)(cont - 1));
}

// A sequence of `cont` trailing bytes starting at data[i] fits within len, and
// the lead byte was valid (cont != 255).
static _Bool seq_fits(size_t i, size_t len, uint8_t cont) {
    if (cont == 255) {
        return false;
    }
    return (_Bool)(i + 1 + cont <= len);
}

// Continuation bytes (if any) pass their guards. A pure-ASCII lead (cont 0) has
// no continuations to check.
static _Bool tail_guard_ok(const uint8_t *data, size_t i, uint8_t cont) {
    if (cont == 0) {
        return true;
    }
    return tail_ok(data, i, data[i], cont);
}

// Decode one code point from data[i..]. Advances *i past the sequence on
// success; returns false on any malformed prefix. Mirrors Lean utf8DecodeStep.
static _Bool decode_step(const uint8_t *data, size_t len, size_t *i) {
    uint8_t cont = kContCount[data[*i]];
    if (!seq_fits(*i, len, cont)) {
        return false; // bad lead byte, or sequence runs past the end
    }
    if (!tail_guard_ok(data, *i, cont)) {
        return false;
    }
    *i += 1 + cont;
    return true;
}

_Bool ws_utf8_valid(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (!decode_step(data, len, &i)) {
            return false;
        }
    }
    return true;
}
