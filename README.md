# libws

RFC 6455 WebSocket プロトコルコアの **freestanding C23** 実装である。

libc に依存しない。
標準ライブラリ関数の代わりに、自前の `ws_memcpy` などを使う。
動的確保はせず、固定長スタックバッファだけで動く。

設計は **sans-I/O** である。
ソケットの読み書き、バッファの確保、スレッド管理は呼び出し側が持つ。
このライブラリはバイト列の変換だけを担う。
フレームの parse と build、マスキング、ハンドシェイクの accept 計算、close コードの判定、UTF-8 検証、接続状態機械の遷移である。

## 三層検証

正しさを三つの層で担保する。
各層は別の道具で別の対象を保証し、機械変換では結線しない。

- **設計（TLA+、`spec/`）**：接続状態機械の安全性を網羅検査する。CONNECTING/OPEN/CLOSING/CLOSED の遷移と close ハンドシェイクが不変条件 INV1..INV6 を保つことを、有限状態空間の全探索で確かめる。
- **数学的核（Lean 4、`proofs/`）**：バイト列とビット演算レベルの性質 P1..P8 を証明する。マスキングが自己逆元であること（P1）、長さエンコードの往復と境界分類（P3/P4）、UTF-8 validator が accept する列が妥当な Unicode スカラ値であること（P8）などである。
- **実装（TDD）**：各性質を C のテストベクタへ落とし、先に失敗させてから実装をモデルに合わせる。UTF-8 の検証コードは Lean の `utf8DecodeStep` と分岐を1対1で対応させ、橋渡しテストで証明と実装を固定している。

## 公開 API

すべて `include/ws.h` で宣言する。

| 関数 | 役割 | 検証 |
|------|------|------|
| `ws_mask` | ペイロードのマスク/アンマスク（同一演算） | Lean P1/P2 |
| `ws_parse_header` | フレームヘッダを parse | Lean P3..P6 |
| `ws_build_header` | フレームヘッダを serialize | Lean P3/P4 |
| `ws_classify_opcode` | opcode を data/control/reserved に分類 | Lean P5 |
| `ws_close_code_sendable` | close コードの送出可否を判定 | Lean P7 |
| `ws_handshake_accept` | `Sec-WebSocket-Accept` を計算 | RFC 6455 §4.2 |
| `ws_utf8_valid` | UTF-8 列を検証 | Lean P8 |
| `ws_conn_init` / `ws_conn_step` | 接続状態機械を駆動 | TLA+ INV1..INV6 |

## ビルドと開発

開発環境は Nix flake で固定する。

```sh
nix develop
```

`just` のコマンドで各タスクを実行する。

| コマンド | 内容 |
|----------|------|
| `just build` | テストバイナリをビルド |
| `just test` | セルフチェックテストを実行（exit 0 が全 CHECK 通過） |
| `just lint` | clang-tidy |
| `just fmt` | clang-format の差分チェック |
| `just ccn` | 循環的複雑度を検査（全関数 CCN ≤ 3） |
| `just bench` | マスキングとフレーム parse のスループット計測 |
| `just verify-proofs` | Lean の証明と TLA+ の設計を再検査 |
| `just check` | fmt/ccn/lint/test を一括実行 |

`just bench` は rdtsc でサイクルを測り、`cycles/byte`（マスキング）と `cycles/frame`（parse）を出力する。
rdtsc の周波数は環境依存なので、MiB/s ではなくサイクルを単位とする。

## ライセンス

`LICENSE` を参照。
