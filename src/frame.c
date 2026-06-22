// Frame header parse/build (RFC 6455 §5.2). Verified core: Lean WsProto.Basic
// P3/P4 (length forms), P5 (opcode class), P6 (control payload <= 125),
// P7 (close-code wire validity).
#include "ws.h"
#include "ws_internal.h"

// P7: close codes 1000-1011 except the reserved 1004/1005/1006, plus the
// application range 3000-4999, may appear on the wire (RFC 6455 §7.4.1).
// 1015 is outside 1000-1011 so it is excluded automatically.
static _Bool in_range(uint16_t c, uint16_t lo, uint16_t hi) {
    return (_Bool)(c >= lo && c <= hi);
}

// The three reserved/internal codes carved out of the 1000-1011 block.
static _Bool close_reserved(uint16_t c) {
    return (_Bool)(c == 1004 || c == 1005 || c == 1006);
}

static _Bool close_protocol_ok(uint16_t code) {
    if (!in_range(code, 1000, 1011)) {
        return false;
    }
    return (_Bool)!close_reserved(code);
}

_Bool ws_close_code_sendable(uint16_t code) {
    if (close_protocol_ok(code)) {
        return true;
    }
    return in_range(code, 3000, 4999);
}

// P5: 0x0/0x1/0x2 data, 0x8/0x9/0xA control, else reserved. Total.
// Table over the 4-bit opcode space keeps this branch-free (CCN 1).
static const ws_opclass kOpClass[16] = {
    WS_CLASS_DATA,     WS_CLASS_DATA,     WS_CLASS_DATA,     WS_CLASS_RESERVED,
    WS_CLASS_RESERVED, WS_CLASS_RESERVED, WS_CLASS_RESERVED, WS_CLASS_RESERVED,
    WS_CLASS_CONTROL,  WS_CLASS_CONTROL,  WS_CLASS_CONTROL,  WS_CLASS_RESERVED,
    WS_CLASS_RESERVED, WS_CLASS_RESERVED, WS_CLASS_RESERVED, WS_CLASS_RESERVED,
};

ws_opclass ws_classify_opcode(uint8_t opcode) {
    return kOpClass[opcode & 0x0F];
}

// Read a big-endian unsigned of `n` bytes from buf into *v. Caller ensures len.
static uint64_t read_be(const uint8_t *buf, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | buf[i];
    }
    return v;
}

// Extended-length byte count for the 7-bit field value: 126->2, 127->8, else 0.
static size_t ext_bytes(uint8_t l7) {
    if (l7 == 126) {
        return 2;
    }
    return (l7 == 127) ? 8 : 0;
}

// Raw length value for the field: extended bytes if present, else the 7-bit
// immediate. `ext` is 0/2/8.
static uint64_t raw_len(const uint8_t *buf, size_t ext) {
    if (ext == 0) {
        return buf[1] & 0x7F;
    }
    return read_be(buf + 2, ext);
}

// RFC §5.2: the 64-bit length form must have its most significant bit clear.
static _Bool len_ok(size_t ext, uint64_t v) {
    if (ext < 8) {
        return true;
    }
    return (_Bool)((v >> 63) == 0U);
}

// Decode the length field at buf[1]. Sets *len and *hdr (bytes before the mask
// key). Returns 0, WS_ERR_NEED_MORE, or WS_ERR_PROTOCOL (64-bit MSB set).
static int decode_len(const uint8_t *buf, size_t avail, uint64_t *len, size_t *hdr) {
    size_t ext = ext_bytes(buf[1] & 0x7F);
    *hdr = 2 + ext;
    if (avail < *hdr) {
        return WS_ERR_NEED_MORE;
    }
    *len = raw_len(buf, ext);
    if (!len_ok(ext, *len)) {
        return WS_ERR_PROTOCOL;
    }
    return 0;
}

// P6: a control frame's payload must be <= 125 (RFC §5.5).
static _Bool control_too_big(ws_opcode op, uint64_t plen) {
    return (_Bool)(ws_classify_opcode((uint8_t)op) == WS_CLASS_CONTROL && plen > 125);
}

// Fill the fixed fields from the first two bytes and the decoded length.
static void fill_fields(const uint8_t *buf, uint64_t plen, ws_frame_header *out) {
    out->fin = (_Bool)((buf[0] & 0x80) != 0);
    out->opcode = (ws_opcode)(buf[0] & 0x0F);
    out->masked = (_Bool)((buf[1] & 0x80) != 0);
    out->payload_len = plen;
}

// Read the 4-byte mask key after the header. Returns total bytes or NEED_MORE.
static int parse_mask(const uint8_t *buf, size_t len, size_t hdr, ws_frame_header *out) {
    if (!out->masked) {
        return (int)hdr;
    }
    if (len < hdr + 4) {
        return WS_ERR_NEED_MORE;
    }
    ws_memcpy(out->mask_key, buf + hdr, 4);
    return (int)(hdr + 4);
}

// Validate the decoded frame and read the mask key. Returns header bytes or err.
static int finish_parse(const uint8_t *buf, size_t len, size_t hdr, ws_frame_header *out) {
    if (control_too_big(out->opcode, out->payload_len)) {
        return WS_ERR_PROTOCOL;
    }
    return parse_mask(buf, len, hdr, out);
}

int ws_parse_header(const uint8_t *buf, size_t len, ws_frame_header *out) {
    if (len < 2) {
        return WS_ERR_NEED_MORE;
    }
    uint64_t plen;
    size_t hdr;
    int rc = decode_len(buf, len, &plen, &hdr);
    if (rc != 0) {
        return rc;
    }
    fill_fields(buf, plen, out);
    return finish_parse(buf, len, hdr, out);
}

// Write a big-endian unsigned of `n` bytes.
static void write_be(uint8_t *buf, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[n - 1 - i] = (uint8_t)(v >> (8 * i));
    }
}

// Header bytes before the mask key for a payload length (P3/P4 boundaries):
// <126 -> 2, <=0xFFFF -> 4, else 10.
static size_t hdr_len(uint64_t plen) {
    if (plen < 126) {
        return 2;
    }
    return (plen <= 0xFFFF) ? 4 : 10;
}

// The 7-bit length-field value for a header form: tiny immediate, or 126/127.
static uint8_t len_field(uint64_t plen, size_t hdr) {
    if (hdr == 2) {
        return (uint8_t)plen;
    }
    return (hdr == 4) ? 126 : 127;
}

// Set bit 7 of *b when flag is set (avoids a bool->int ternary).
static void set_high_bit(uint8_t *b, _Bool flag) {
    if (flag) {
        *b |= 0x80;
    }
}

// Emit byte1 (mask flag + length field) and any extended length bytes.
static void build_len(const ws_frame_header *h, uint8_t *buf, size_t hdr) {
    buf[1] = len_field(h->payload_len, hdr);
    set_high_bit(&buf[1], h->masked);
    if (hdr > 2) {
        write_be(buf + 2, h->payload_len, hdr - 2);
    }
}

// First header byte: FIN flag in bit 7, opcode in the low nibble.
static uint8_t build_byte0(const ws_frame_header *h) {
    uint8_t b = (uint8_t)h->opcode & 0x0F;
    set_high_bit(&b, h->fin);
    return b;
}

// Append the 4-byte mask key when masked.
static void build_mask(const ws_frame_header *h, uint8_t *buf, size_t hdr) {
    if (h->masked) {
        ws_memcpy(buf + hdr, h->mask_key, 4);
    }
}

// Total header size including the mask key when present.
static size_t total_len(size_t hdr, _Bool masked) {
    if (masked) {
        return hdr + 4;
    }
    return hdr;
}

int ws_build_header(const ws_frame_header *h, uint8_t *buf, size_t cap) {
    size_t hdr = hdr_len(h->payload_len);
    size_t need = total_len(hdr, h->masked);
    if (cap < need) {
        return WS_ERR_TOO_SMALL;
    }
    buf[0] = build_byte0(h);
    build_len(h, buf, hdr);
    build_mask(h, buf, hdr);
    return (int)need;
}
