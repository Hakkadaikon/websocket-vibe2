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

すべて `include/ws/ws.h` で宣言する（`#include "ws/ws.h"`）。

### 高レベル driver（sans-I/O）

受信バイトを供給し、意味イベントを取り出し、送信フレームを組み立てる。
I/O とメッセージバッファは呼び出し側が持つ。

```c
#include "ws/ws.h"

ws_conn c;
uint8_t msg_buf[WS_MAX_MESSAGE];
ws_conn_init(&c, WS_ROLE_SERVER, msg_buf, sizeof msg_buf);
ws_conn_open(&c);                 // HTTP 101 を返した後

ws_conn_recv(&c, rx, rx_len);     // 受信した生バイトをコアに供給

ws_event ev;
while (ws_conn_poll(&c, &ev) != WS_EV_NONE) {
    switch (ev.type) {
    case WS_EV_MESSAGE: /* ev.data / ev.len / ev.op */ break;
    case WS_EV_PING:    /* ws_send_pong で応答 */      break;
    case WS_EV_CLOSE:   /* ev.close_code, ws_send_close */ break;
    case WS_EV_ERROR:   /* プロトコル違反、接続を閉じる */  break;
    default: break;
    }
}

uint8_t out[4096];
size_t n = ws_send_message(&c, WS_OP_TEXT, payload, payload_len, out, sizeof out);
// n バイトを送信する（n == 0 は失敗）。
```

| 関数 | 役割 | 検証 |
|------|------|------|
| `ws_conn_init` / `ws_conn_open` | 接続を初期化し OPEN へ | TLA+ WsLifecycle |
| `ws_conn_recv` | 受信バイトを供給（msg_buf に蓄積） | TLA+ SINV1 |
| `ws_conn_poll` | 意味イベントを drain（フラグメント結合・close 処理込み） | TLA+ WsStream SINV1..8 |
| `ws_send_message` / `ws_send_pong` / `ws_send_close` | 送信フレームを構築 | Lean P1..P7 |

`ws_conn_poll` の不変条件はすべて TLA+ で検査済み。とりわけ、プロトコル違反で
`failed` をラッチした瞬間に組み立て中メッセージを破棄する（SINV8）という設計判断は、
mutation テストが「破棄漏れ」を survivor として検出したことから導いた。

### 低レベルプリミティブ

driver を使わず、フレーム単位で直接扱うこともできる。

| 関数 | 役割 | 検証 |
|------|------|------|
| `ws_mask` | ペイロードのマスク/アンマスク（同一演算） | Lean P1/P2 |
| `ws_parse_header` / `ws_build_header` | フレームヘッダの parse / serialize | Lean P3..P6 |
| `ws_classify_opcode` | opcode を data/control/reserved に分類 | Lean P5 |
| `ws_close_code_sendable` | close コードの送出可否を判定 | Lean P7 |
| `ws_handshake_accept` | `Sec-WebSocket-Accept` を計算 | RFC 6455 §4.2 |
| `ws_utf8_valid` | UTF-8 列を検証 | Lean P8 |

### I/O ランタイム（任意）

プロトコルコアは I/O を持たない。`ws/io.h` はその上に載る**任意の**ランタイムで、
epoll ベースの TCP サーバ（Linux x86-64、raw syscall、freestanding）を提供する。
socket/accept/read/write/HTTP ハンドシェイク/複数接続の多重化はすべて中に隠れ、
利用者はイベントハンドラだけ書けばよい。

```c
#include "ws/io.h"

static void on_event(ws_io *io, ws_conn *c, const ws_event *ev) {
    switch (ev->type) {
    case WS_EV_MESSAGE: ws_io_send_message(io, c, ev->op, ev->data, ev->len); break;
    case WS_EV_PING:    ws_io_send_pong(io, c, ev->data, ev->len);            break;
    case WS_EV_CLOSE:   ws_io_send_close(io, c, ev->close_code);              break;
    default: break;
    }
}

void _start(void) { ws_serve(8080, WS_ROLE_SERVER, on_event); }
```

| 関数 | 役割 |
|------|------|
| `ws_serve` | epoll で複数接続を多重化し、ハンドシェイクと driver を駆動 |
| `ws_io_send_message` / `ws_io_send_pong` / `ws_io_send_close` | ハンドラから接続へ送信 |

プロトコルコアと I/O 層は分離している。epoll を使わず、別の多重化機構・別 OS・組込み環境に
コアだけ載せ替えることもできる（その場合は driver API を直接回す）。

## example: echo サーバー

`example/echo_server.c` は `ws/io.h` を使った WebSocket echo サーバー。本体は `on_event`
ハンドラと `ws_serve` 呼び出しだけ（約 40 行）。フレーム処理・I/O・ハンドシェイク・
複数接続多重化はすべて SDK が持つ。

```sh
nix build .#echo-server   # またはサンドボックス内では just example
./result/bin/echo-server        # :8080 で待ち受ける（既定）
./result/bin/echo-server 9090   # ポートを指定（引数が無効なら 8080）
```

ポートが使用中なら起動せず、失敗した段階を `bind() failed (port already in use?)`
のように stderr に出して終了する（segfault しない）。`ss -tlnp 'sport = :8080'` で
占有プロセスを確認できる。

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

## ライセンス

`LICENSE` を参照。
