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

## driver 層 (WsStream) TDD タスク

- [x] D1. lifecycle.c を内部化: ws_lc_* を ws_internal.h に宣言、旧公開API(ws_conn_step/旧enum)削除
- [x] D2. stream.c: ws_conn_init/open/recv (SINV1) — Red→Green
- [x] D3. stream.c: ws_conn_poll 単一TEXT/incremental(S3)/PING/CLOSE
- [x] D4. フラグメント結合 TEXT+CONT (S4-S6, SINV5)、区画移動
- [x] D5. エラー経路 interleave/stray/too-big/illegal/unmasked → ERROR latched (S7-S9,S13,S14,SINV8)
- [x] D6. UTF-8 検証 (RFC 8.1) → ERROR
- [x] D7. ws_send_message/pong/close (roundtrip)
- [x] D8. 旧 lifecycle テストを内部API向けに書き直し
- [x] D9. justfile srcs に stream.c、echo_server.c 書き直し、実機 echo 確認
- [x] D10. 全チェック緑 (fmt/ccn/lint/test)
  - SINV8 mutation: fail() の reset を外すと SINV8 テスト 6 件が落ちることを確認(ハーネス健全)。
  - poll の S9(too-big)は co-located buffer では recv の S2(SINV1)が先に弾くため到達不能 → 防御コード。

## レビュー欄

全タスク完了。三層検証 + freestanding C23 実装が揃った。

- 設計(TLA+): WsLifecycle, TLC No error / 8 distinct states / INV1-6, mutation oracle 検証済み。
- 数学的核(Lean 4): P1-P8 全て sorry 無し、標準公理のみ(native_decide 不使用)。
- 実装(TDD): 橋渡しテストで証明分岐を1対1固定。`i%4`→`i%3` 変異でテストが落ちることを確認(ハーネス健全)。
- 品質: `just check`(fmt/ccn/lint/test)全緑。CCN 全関数 ≤3。`just verify`(TLA+ + Lean)も緑。
- bench: rdtsc で mask ~1.4 cycles/byte, parse ~20 cycles/frame。

残課題(YAGNI で今回外した範囲、必要になれば追加):
- TCP I/O 層・TLS は sans-I/O 設計なので呼び出し側責務(意図的に範囲外)。
- フレーム payload の UTF-8 ストリーミング検証(フラグメント跨ぎ): 現 ws_utf8_valid は完結列向け。
- RSV ビット/拡張ネゴシエーションは未対応(RFC 上 extension 無しなら 0)。
