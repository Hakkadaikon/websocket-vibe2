# 開発

ビルド・テスト・デバッグの手順をまとめる。概要と使い方は [`../README.md`](../README.md)、正しさの保証については [`security.md`](security.md) を参照。

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
| `just bdd <feature>` | TLA+ 由来の機械形式 `.feature` を実装に当てて実行 |
| `just lint` | clang-tidy |
| `just fmt` | clang-format の差分チェック |
| `just ccn` | 循環的複雑度を検査（全関数 CCN ≤ 3） |
| `just bench` | マスキングとフレーム parse のスループット計測 |
| `just debug` | トレース付きデバッグビルド（`build/echo-server-debug`） |
| `just debug-run` | デバッグサーバを起動（トレースは stderr へ） |
| `just verify-design` | TLA+ の状態機械をモデル検査（INV1..INV6） |
| `just verify-proofs` | Lean の証明 P1..P8 を再検査 |
| `just verify` | 形式検証の二層（design + proofs）をまとめて実行 |
| `just check` | fmt/ccn/lint/test を一括実行 |

`just bench` は rdtsc でサイクルを測り、`cycles/byte`（マスキング）と `cycles/frame`（parse）を出力する。
rdtsc の周波数は環境依存なので、MiB/s ではなくサイクルを単位とする。

## デバッグトレース（横断的関心事）

デバッグログは AOP 的に分離している。メインロジックは join point（イベント発生・状態遷移・
I/O ステップ）に 1 行の `WS_TRACE_*` を置くだけで、ログの整形と出力先は `src/trace.c`
（アスペクト）に隔離する。`include/ws/trace.h` がマクロを定義する。

- 通常ビルド: `WS_DEBUG` 未定義。`WS_TRACE_*` は `((void)0)` に展開され、バイナリに
  一切痕跡が残らない（ゼロオーバーヘッド）。
- デバッグビルド（`just debug` / `-DWS_DEBUG` + `src/trace.c`）: 各 join point が
  stderr に 1 行出力する。

```text
[ws] io accept fd=6 detail=-1
[ws] state CONNECTING -> OPEN (handshake)
[ws] io handshake fd=6 detail=-1
[ws] event=MESSAGE len=5
[ws] io close fd=6 detail=-1
```

Nix では `nix build .#echo-server-debug`。通常の `echo-server` は no-op のまま。
