# libws

RFC 6455 WebSocket プロトコルコアの **freestanding C23** 実装である。

libc に依存しない。
標準ライブラリ関数の代わりに、自前の `ws_memcpy` などを使う。
動的確保はせず、固定長スタックバッファだけで動く。

設計は **sans-I/O** である。
ソケットの読み書き、バッファの確保、スレッド管理は呼び出し側が持つ。
このライブラリはバイト列の変換だけを担う。
フレームの parse と build、マスキング、ハンドシェイクの accept 計算、close コードの判定、UTF-8 検証、接続状態機械の遷移である。

正しさは TLA+（設計）・Lean 4（数学的核）・TDD（実装）の三層で担保している。
詳細は [`docs/security.md`](docs/security.md) を参照。

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

`ws_conn_poll` の不変条件はすべて TLA+ で検査済み（フェイルクローズの設計判断 SINV8 など、
詳細は [`docs/security.md`](docs/security.md)）。

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

## ディレクトリ構造

```text
include/ws/        公開ヘッダ（ws.h / io.h / trace.h）
src/               実装。依存レイヤー順（低 → 高）
  util/              mem, mask        — 純粋ユーティリティ
  crypto/            sha1, base64     — ハッシュ / エンコード
  framing/           frame, handshake, utf8 — ワイヤフォーマット
  session/           lifecycle, stream      — プロトコル状態機械
  io/                io_posix         — epoll ランタイム（任意）
  trace.c            横断的関心事のデバッグアスペクト
  ws_internal.h      レイヤー横断の内部宣言
spec/              TLA+ 設計モデル（WsLifecycle / WsStream）
proofs/WsProto/    Lean 4 証明（P1..P8）
test/              セルフチェックテスト（cases/ に用途別分割）
example/           echo サーバー
bench/             スループットベンチ
docs/              開発手順・検証/健全性のドキュメント
```

## ドキュメント

- [`docs/development.md`](docs/development.md) — ビルド・テスト・デバッグの手順（`just` コマンド一覧、トレース）。
- [`docs/security.md`](docs/security.md) — 三層検証と RFC 6455 セキュリティ性質の保証。

## ライセンス

`LICENSE` を参照。
