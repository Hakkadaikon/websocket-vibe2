// Opening-handshake accept (RFC 6455 §4.2.2):
//   accept = base64( sha1( key ++ GUID ) ), 28 base64 chars, no NUL.
// A fixed stack buffer holds key++GUID — no allocation.
#include "ws/ws.h"
#include "ws_internal.h"

// RFC 6455 §1.3 magic GUID, 36 chars (no NUL needed in the concatenation).
static const char kGuid[36] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#define WS_KEY_MAX 64 // Sec-WebSocket-Key is base64(16) = 24 chars; 64 is slack.

int ws_handshake_accept(const uint8_t *key, size_t key_len, uint8_t out[28]) {
    if (key_len > WS_KEY_MAX) {
        return WS_ERR_TOO_SMALL; // input larger than the fixed concat buffer
    }
    uint8_t cat[WS_KEY_MAX + 36];
    ws_memcpy(cat, key, key_len);
    ws_memcpy(cat + key_len, kGuid, 36);

    uint8_t digest[20];
    ws_sha1(cat, key_len + 36, digest);
    return (int)ws_base64(digest, 20, out);
}
