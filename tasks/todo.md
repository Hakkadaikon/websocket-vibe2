# WebSocket プロトコルコア SDK — 進行

## 確定方針
- sans-I/O プロトコルコアのみ。freestanding C23(-ffreestanding -nostdlib)。
- 三層検証: 設計=TLA+(済) / 数学的核=Lean(P1-P7 済) / 実装=TDD(これから)。
- Nix でビルド/ツール固定。lint=clang-tidy, fmt=clang-format, CCN=lizard≤3, perf=自己計測。justfile 1発。

## 検証成果(実装の根拠)
- spec/WsLifecycle.tla: state/frag/sentClose/rcvdClose の step。INV1-6。→ C の step 関数に対応。
- proofs/.../Basic.lean: P1 mask involution, P2 長さ保存, P3 len往復, P4 境界126/0xFFFF,
  P5 opcode分類, P6 control≤125, P7 closeコード送出可否。→ C の対応分岐をテストで固定。

## TDD タスク(橋渡しテスト先行 → Red → Green → Refactor、細かく commit)
- [ ] S1. flake.nix + justfile + .clang-format/.clang-tidy 再構築
- [ ] S2. include/ws.h 公開API骨格 + test ハーネス(freestanding _start)
- [ ] S3. mem.c (memcpy/memset/memcmp) — clang が要求するシンボル
- [ ] S4. mask.c — P1/P2 橋渡しテスト(involution, 長さ保存, i mod 4)
- [ ] S5. frame_len.c — P3/P4(encode/decode往復, 境界126/0xFFFF/0x10000)
- [ ] S6. opcode/control/close — P5(分類)/P6(control≤125)/P7(closeコード)
- [ ] S7. sha1.c (RFC3174ベクタ) + base64.c → handshake.c(Sec-WebSocket-Accept, RFC例)
- [ ] S8. frame parse/build 統合(header長, mask適用, control制約, ラウンドトリップ)
- [ ] S9. lifecycle.c — TLA+ Next に対応する step 関数(Handshake/SendClose/RecvClose/Start/FinishFrag)
       + 橋渡しテスト(CLOSED⇔両close, frag破棄 R6a, 単調性)
- [ ] S10. utf8.c — P8(健全性・決定性)。重ければ核補題のみ Lean 追加 → 実装テスト
- [ ] S11. bench.c(mask/parse スループット)+ just bench
- [ ] S12. CCN≤3 / clang-tidy / clang-format 全緑、README

## レビュー欄
(完了後記入)
