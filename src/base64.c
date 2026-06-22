// Base64 encode, standard alphabet with padding (RFC 4648). Writes no NUL.
// Used for Sec-WebSocket-Accept (RFC 6455 §4.2.2).
#include "ws_internal.h"

static const char kB64[64] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode one 3-byte group (b is zero-padded by the caller for partial tails).
static void enc3(const uint8_t b[3], uint8_t out[4]) {
    uint32_t v = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    out[0] = (uint8_t)kB64[(v >> 18) & 0x3F];
    out[1] = (uint8_t)kB64[(v >> 12) & 0x3F];
    out[2] = (uint8_t)kB64[(v >> 6) & 0x3F];
    out[3] = (uint8_t)kB64[v & 0x3F];
}

// Encode the trailing 1 or 2 bytes with '=' padding. Returns 4.
static size_t enc_tail(const uint8_t *in, size_t rem, uint8_t *out) {
    uint8_t b[3] = {0, 0, 0};
    ws_memcpy(b, in, rem);
    enc3(b, out);
    out[3] = '=';
    if (rem == 1) {
        out[2] = '=';
    }
    return 4;
}

size_t ws_base64(const uint8_t *in, size_t len, uint8_t *out) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        enc3(in + i, out + o);
        o += 4;
    }
    size_t rem = len - i;
    if (rem != 0) {
        o += enc_tail(in + i, rem, out + o);
    }
    return o;
}
