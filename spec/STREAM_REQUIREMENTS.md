# WebSocket ストリーミング driver — EARS 要求 + ドメインモデル

WsLifecycle(接続ライフサイクル)の上に載る「バイトストリーム → 意味イベント」層。
ws_conn_recv で生バイトを供給し、ws_conn_poll が意味イベント(MESSAGE/PING/PONG/CLOSE/
ERROR/NONE)を1つずつ返す。フラグメント結合とオーバーフロー検出をここで固める。
バイト列のパース詳細(長さエンコード等)は Lean P3-P8 の領分。ここは状態遷移の正しさ。

役割分担: ライフサイクル状態(state/sentClose/rcvdClose)は WsLifecycle で検証済み。本 spec
WsStream はそれを前提に、frag 結合の進行・受信バッファ・poll の決定性を検証する。

## ドメインモデル(状態変数)

- `state`: WsLifecycle と同じ {CONNECTING, OPEN, CLOSING, CLOSED}。初期 CONNECTING。
- `frag`: 結合中メッセージ型 {NONE, TEXT, BIN}。初期 NONE。NONE = メッセージ境界。
- `msgLen`: 結合中メッセージの蓄積バイト長(抽象。0 以上、上限 CAP)。初期 0。
- `rxLen`: 未パースの受信生バイト長(抽象。0 以上)。初期 0。
- `failed`: プロトコル違反でラッチされたか。BOOLEAN。初期 FALSE。
- `lastEvent`: 直前の poll が返したイベント型 {NONE, MESSAGE, PING, PONG, CLOSE, ERROR}。観測用。

抽象化方針: バイト内容は持たず長さ(msgLen/rxLen)と型(frag)で抽象する。1 フレームを
「data開始(START)/ data継続(CONT)/ data終了(FIN)/ control(PING/PONG/CLOSE)」の種別と
payload長で表す。CAP は WS_MAX_MESSAGE の抽象(小さい定数、例 3 で状態空間を有限化)。

## EARS 要求

### 受信とパース

- S1 (event): WHEN bytes arrive (ws_conn_recv) the driver SHALL append them to the receive
  buffer, increasing rxLen, provided msgLen + rxLen <= CAP.
- S2 (unwanted): IF appending bytes would exceed CAP THEN the driver SHALL reject them
  (WS_ERR_TOO_SMALL) and SHALL NOT change rxLen.
- S3 (state): WHILE rxLen does not hold a complete frame, ws_conn_poll SHALL return WS_EV_NONE
  and SHALL NOT change frag/msgLen.

### フラグメント結合(RFC 5.4)

- S4 (event): WHEN a data frame with FIN=0 and a data opcode (TEXT/BIN) is parsed WHILE frag=NONE
  THEN the driver SHALL set frag to that type and accumulate its payload into msgLen.
- S5 (event): WHEN a continuation frame (FIN=0) is parsed WHILE frag#NONE THEN the driver SHALL
  accumulate its payload into msgLen and keep frag unchanged.
- S6 (event): WHEN a final frame (FIN=1) completing the message is parsed THEN the driver SHALL
  emit WS_EV_MESSAGE with the whole assembled payload, then reset frag=NONE and msgLen=0.
- S7 (unwanted): IF a new data frame (non-continuation TEXT/BIN) is parsed WHILE frag#NONE THEN it
  is interleaving (R10) -> the driver SHALL set failed and emit WS_EV_ERROR.
- S8 (unwanted): IF a continuation frame is parsed WHILE frag=NONE THEN it is a stray continuation
  (R11) -> the driver SHALL set failed and emit WS_EV_ERROR.
- S9 (unwanted): IF accumulating a fragment would push msgLen past CAP THEN the driver SHALL set
  failed and emit WS_EV_ERROR (message too big, close 1009).
- S9a (unwanted, design decision): WHEN the driver sets failed for any protocol violation
  (S7/S8/S9/S14) THEN it SHALL abandon in-flight message assembly: reset frag=NONE and msgLen=0.
  Rationale: once a protocol violation latches failed the connection is being torn down, the
  partially assembled message is discarded (cf. WsLifecycle R6a), and the buffer-accounting
  invariant SINV2 ((frag=NONE)<=>(msgLen=0)) must keep holding. Without this reset an error that
  fires mid-fragment would leave frag#NONE with msgLen>0 latched forever, breaking SINV2.

### 制御フレーム(RFC 5.4: 割り込み可)

- S10 (event): WHEN a PING/PONG frame is parsed (during OPEN/CLOSING, even mid-fragment) THEN the
  driver SHALL emit WS_EV_PING/PONG and SHALL NOT alter frag or msgLen.
- S11 (event): WHEN a CLOSE frame is parsed THEN the driver SHALL emit WS_EV_CLOSE, drive the
  lifecycle (rcvdClose), and SHALL NOT alter an in-flight frag's accounting beyond the lifecycle's
  R6a discard when reaching CLOSED.

### 出力と決定性

- S12 (ubiquitous): ws_conn_poll SHALL be deterministic: every buffered frame maps to exactly one
  event type; the driver never emits two different events for the same frame.
- S13 (state): WHILE failed holds, ws_conn_poll SHALL only ever return WS_EV_ERROR (latched), and
  no further MESSAGE/PING/PONG SHALL be emitted.
- S14 (unwanted): IF a frame is parsed WHILE state is CONNECTING or CLOSED THEN it is illegal ->
  WS_EV_ERROR (frames only flow during OPEN/CLOSING — ties to WsLifecycle INV2).

## 安全性不変条件(model-check 対象)

- SINV1 (S1/S2/S9): msgLen + rxLen <= CAP always. バッファは決して溢れない。
- SINV2 (S4-S6): frag=NONE  <=>  msgLen=0. 結合中でなければ蓄積はゼロ、逆も。メッセージ境界の健全性。
- SINV3 (S13): failed => lastEvent ∈ {ERROR, NONE}. ラッチされたら ERROR 以外の意味イベントは出ない。
- SINV4 (S14 / WsLifecycle INV2): (frag#NONE \/ rxLen>0 でフレーム処理が起きる) => state ∈ {OPEN, CLOSING}.
  CONNECTING/CLOSED でメッセージ蓄積は無い。
- SINV5 (S6): MESSAGE を emit した直後は frag=NONE かつ msgLen=0(メッセージ境界へ戻る)。
- SINV6 (単調性/温度): failed は一度 TRUE になったら FALSE に戻らない(action property)。
- SINV7 (S7/S8/S9/S13/S14): lastEvent = ERROR => failed. ERROR を観測したら必ず failed が
  ラッチされている(SINV3 の逆向き)。エラー経路の ~failed ガード弱化や failed:=TRUE 漏れを捕まえる。
- SINV8 (S9a): failed => (frag = NONE /\ msgLen = 0). 違反で failed がラッチされたら結合は
  放棄され、メッセージ境界へ戻る。S9a の設計判断を検査可能にしたもの。SINV2 は frag#NONE かつ
  msgLen>0 の「整合した」残留を許してしまう(双条件は両辺 false で成立)ため、S9a を強制するには
  SINV8 が要る。mutation oracle の survivor(Interleave が frag/msgLen を残す)が追加の動機。

## poll の決定的分類(S12 の網羅性)

パースした1フレームは、その (opcode種別, fin, 現 frag, state, failed) から一意に次のいずれかへ:
MESSAGE / PING / PONG / CLOSE / ERROR。全組合せが決定的に1つへ落ちることを検査する(網羅性)。
これは Lean 側の「step 関数の網羅性」とも対応づく(設計=TLA+、関数の決定性=Lean/test)。
