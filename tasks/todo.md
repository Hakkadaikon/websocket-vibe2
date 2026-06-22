# WebSocket プロトコルスタック SDK 計画

## 方針(確定)
- **sans-I/O プロトコルコアのみ**。I/O・メモリ確保は呼び出し側。
- **freestanding 厳密**: `-ffreestanding -nostdlib`、libc 関数(malloc/memcpy/strlen 等)非使用。memcpy 相当・SHA-1・Base64・UTF-8 検証は自前。
- C23。Nix flake でビルド/ツール固定。lint=clang-tidy, format=clang-format, CCN=lizard で 3 以下、perf=自己計測、全部 justfile。
- RFC 6455 準拠(抽出済み: フレーム/opcode/マスキング/ハンドシェイク GUID/close コード/UTF-8)。

## 構成(最小)
```
flake.nix          # nix devShell + ビルド
justfile           # build test lint fmt ccn bench
include/ws.h       # 公開 API(単一ヘッダ)
src/ws_frame.c     # フレーム parse/build, マスキング
src/ws_handshake.c # Sec-WebSocket-Accept 計算
src/sha1.c         # SHA-1(ハンドシェイク用)
src/base64.c       # Base64 encode
src/utf8.c         # text/close reason の UTF-8 検証
src/mem.c          # freestanding memcpy/memset/memcmp
test/test.c        # assert ベース自己検証(RFC 例ベクタ含む)
bench/bench.c      # parse/mask スループット計測
```

## RFC 抽出メモ
- フレーム: byte0=FIN|RSV1-3|opcode(4), byte1=MASK|len(7), ext len 16/64, mask key 4B, payload
- opcode: 0x0 cont,0x1 text,0x2 bin,0x8 close,0x9 ping,0xA pong。0x8-0xF control。
- control: payload ≤125、非分割。
- mask: out[i]=in[i]^key[i%4](双方向)
- accept: base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
  - 例: "dGhlIHNhbXBsZSBub25jZQ==" -> "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
- close: 1000-1011 定義, 1005/1006/1015 は wire 禁止, app 用 3000-4999
- text/close reason は UTF-8 不正なら 1007

## TDD タスク(細かく commit)
- [ ] 1. flake.nix + justfile + .clang-format/.clang-tidy + .gitignore
- [ ] 2. mem.c(memcpy/memset/memcmp)+ test → Red/Green
- [ ] 3. sha1.c + RFC3174 テストベクタ
- [ ] 4. base64.c encode + テストベクタ
- [ ] 5. ws_handshake.c(accept 計算)+ RFC 6455 例
- [ ] 6. ws_frame.c parse(header 長/mask/payload, control 制約)+ テスト
- [ ] 7. ws_frame.c build(server→client 非マスク)+ ラウンドトリップ
- [ ] 8. utf8.c 検証 + 境界テストベクタ
- [ ] 9. close コード検証ヘルパ
- [ ] 10. bench.c + just bench
- [ ] 11. lizard CCN≤3 確認、clang-tidy/format 通す、README

## レビュー欄
(完了後記入)
