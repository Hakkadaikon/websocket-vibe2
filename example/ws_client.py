#!/usr/bin/env python3
# Minimal RFC 6455 client (stdlib only) to verify the echo example.
import base64
import hashlib
import os
import socket
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def main():
    host, port = "127.0.0.1", 8080
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1024)
    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if f"Sec-WebSocket-Accept: {expect}".encode() not in resp:
        print("ACCEPT MISMATCH:", resp)
        sys.exit(1)
    print("handshake ok, accept verified")

    # Send a masked TEXT frame "hello".
    payload = b"hello"
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    frame = struct.pack("!BB", 0x81, 0x80 | len(payload)) + mask + masked
    s.sendall(frame)

    # Read the echoed (unmasked) frame.
    hdr = s.recv(2)
    op, ln = hdr[0], hdr[1] & 0x7F
    body = b""
    while len(body) < ln:
        body += s.recv(ln - len(body))
    print(f"echo opcode={op & 0x0F:#x} payload={body!r}")
    assert body == payload, "echo mismatch"
    print("PASS: hello echoed")
    s.close()


if __name__ == "__main__":
    main()
