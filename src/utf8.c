// P8 UTF-8 validation (RFC 6455 §8.1) — NOT IMPLEMENTED YET, separate task.
// Stub so the SDK links; returns false (not "valid") so nothing mistakes the
// unimplemented path for a passing validator.
#include "ws.h"

_Bool ws_utf8_valid(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    return false;
}
