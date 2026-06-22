#!/usr/bin/env python3
# Multi-connection verification: open N sockets, handshake all, then interleave
# distinct messages and confirm each connection echoes its own payload back.
# Proves the epoll runtime multiplexes concurrent clients (not one-at-a-time).
import base64
import hashlib
import os
import socket
import struct
import sys

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def handshake(s):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:  # read the full response, no frame mixed in
        resp += s.recv(1024)
    expect = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    assert f"Sec-WebSocket-Accept: {expect}".encode() in resp, resp


def send_text(s, payload):
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(struct.pack("!BB", 0x81, 0x80 | len(payload)) + mask + masked)


def read_frame(s):
    hdr = s.recv(2)
    ln = hdr[1] & 0x7F
    body = b""
    while len(body) < ln:
        body += s.recv(ln - len(body))
    return body


def main():
    n = 3
    socks = [socket.create_connection(("127.0.0.1", 8080), timeout=5) for _ in range(n)]
    for s in socks:
        handshake(s)
    print(f"{n} connections open + handshaked concurrently")
    msgs = [f"conn-{i}-says-hi".encode() for i in range(n)]
    # interleave: send on every socket before reading any reply
    for s, m in zip(socks, msgs):
        send_text(s, m)
    ok = True
    for i, (s, m) in enumerate(zip(socks, msgs)):
        echo = read_frame(s)
        match = echo == m
        ok = ok and match
        print(f"conn {i}: sent {m!r} got {echo!r} {'OK' if match else 'MISMATCH'}")
    for s in socks:
        s.close()
    print("PASS: each connection echoed its own message" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
