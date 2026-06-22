// SHA-1 (RFC 3174), single-shot. Streams the message 64-byte block at a time
// over a fixed 64-byte buffer, then appends the 0x80/zero/length padding.
// Used only for the opening-handshake accept (RFC 6455 §4.2.2).
#include "../ws_internal.h"

static uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

// Round constant per 20-round stage (RFC 3174 §5).
static const uint32_t kK[4] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};

// Nonlinear round function per stage. Stages 1 and 3 share b^c^d.
static uint32_t round_f(unsigned stage, uint32_t b, uint32_t c, uint32_t d) {
    if (stage == 0) {
        return (b & c) | (~b & d);
    }
    if (stage == 2) {
        return (b & c) | (b & d) | (c & d);
    }
    return b ^ c ^ d;
}

// Expand the 16-word block into the 80-word schedule (big-endian input).
static void expand(const uint8_t *blk, uint32_t w[80]) {
    for (size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | blk[i * 4 + 3];
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
}

// One full 80-round compression of a single block into state h[5].
static void compress(uint32_t h[5], const uint8_t *blk) {
    uint32_t w[80];
    expand(blk, w);
    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    uint32_t e = h[4];
    for (unsigned i = 0; i < 80; i++) {
        unsigned stage = i / 20;
        uint32_t t = rotl32(a, 5) + round_f(stage, b, c, d) + e + w[i] + kK[stage];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// Build the final block(s): copy the tail, append 0x80, zero-fill, and write the
// 64-bit big-endian bit length. Returns how many 64-byte blocks `last` holds.
static size_t finalize(uint8_t last[128], const uint8_t *tail, size_t rem, uint64_t bits) {
    ws_memset(last, 0, 128);
    ws_memcpy(last, tail, rem);
    last[rem] = 0x80;
    size_t blocks = (rem >= 56) ? 2 : 1;
    uint8_t *lenp = last + blocks * 64 - 8;
    for (unsigned i = 0; i < 8; i++) {
        lenp[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    return blocks;
}

// Compress `n` consecutive 64-byte blocks of state.
static void compress_n(uint32_t h[5], const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        compress(h, buf + i * 64);
    }
}

// Serialize the 5 state words big-endian into out[20].
static void emit(const uint32_t h[5], uint8_t out[20]) {
    for (size_t i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

void ws_sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = len / 64;
    compress_n(h, msg, full);

    uint8_t last[128];
    size_t rem = len - full * 64;
    size_t blocks = finalize(last, msg + full * 64, rem, (uint64_t)len * 8);
    compress_n(h, last, blocks);
    emit(h, out);
}
