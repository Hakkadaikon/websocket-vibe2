#!/usr/bin/env python3
# Minimal RFC 6455 client (stdlib only) to verify the echo example.
import base64
import hashlib
import os
import socket
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def handshake(s, host, port):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += s.recv(1024)
    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    if f"Sec-WebSocket-Accept: {expect}".encode() not in resp:
        print("ACCEPT MISMATCH:", resp)
        sys.exit(1)
    print("handshake ok, accept verified")


def send_text(s, payload):
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(struct.pack("!BB", 0x81, 0x80 | len(payload)) + mask + masked)


def read_frame(s):
    hdr = s.recv(2)
    op, ln = hdr[0], hdr[1] & 0x7F
    body = b""
    while len(body) < ln:
        body += s.recv(ln - len(body))
    return op, body


def main():
    s = socket.create_connection(("127.0.0.1", 8080), timeout=5)
    handshake(s, "127.0.0.1", 8080)
    payload = b"hello"
    send_text(s, payload)
    op, body = read_frame(s)
    print(f"echo opcode={op & 0x0F:#x} payload={body!r}")
    assert body == payload, "echo mismatch"
    print("PASS: hello echoed")
    s.close()


if __name__ == "__main__":
    main()
