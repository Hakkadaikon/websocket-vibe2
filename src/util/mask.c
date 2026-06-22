// Masking (RFC 6455 §5.3). Verified: Lean WsProto.Basic P1 (involution),
// P2 (length preserved). out[i] = in[i] ^ key[i % 4]; same op masks/unmasks.
#include "ws/ws.h"

void ws_mask(uint8_t *data, size_t len, const uint8_t key[4]) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= key[i % 4];
    }
}
