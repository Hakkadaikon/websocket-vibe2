# WebSocket プロトコルコア — EARS 要求 + ドメインモデル

RFC 6455 準拠。sans-I/O プロトコル状態機械の設計を TLA+ で検証するための要求形式化。
対象は「接続ライフサイクル + close ハンドシェイク + メッセージのフラグメント結合」。
I/O・バイト列パースの数値境界は対象外(後者は Lean で証明する)。

## ドメインモデル(状態変数)

- `state`: 接続状態。値域 `{CONNECTING, OPEN, CLOSING, CLOSED}`。初期 `CONNECTING`。
  - RFC 4.1/4.2: ハンドシェイク完了で CONNECTING→OPEN。
  - RFC 7.1.3/7.1.4: Close フレーム送受で CLOSING を経て CLOSED。
- `frag`: 受信中メッセージのフラグメント状態。値域 `{NONE, TEXT, BIN}`。初期 `NONE`。
  - NONE = メッセージ境界(継続中フラグメント無し)。TEXT/BIN = data フレームの途中。
- `sentClose`: 自分が Close フレームを送ったか。BOOLEAN。初期 FALSE。
- `rcvdClose`: 相手の Close フレームを受けたか。BOOLEAN。初期 FALSE。

## EARS 要求

### ライフサイクル

- R1 (event): WHEN the handshake succeeds THEN the endpoint SHALL move from CONNECTING to OPEN.
- R2 (state): WHILE state is CONNECTING the endpoint SHALL NOT process data or control frames.
- R3 (event): WHEN a Close frame is sent THEN the endpoint SHALL set sentClose and enter CLOSING (from OPEN).
- R4 (event): WHEN a Close frame is received THEN the endpoint SHALL set rcvdClose, and if it has not yet sent one it SHALL reply with a Close frame.
- R5 (state): WHILE both sentClose and rcvdClose hold the endpoint SHALL be CLOSED.
- R6 (unwanted): IF state is CLOSED THEN the endpoint SHALL NOT send or receive any further frames.
- R6a (event): WHEN a transition reaches CLOSED WHILE a fragment is in flight (frag # NONE) THEN the
  incomplete message SHALL be discarded (frag reset to NONE). RFC 6455 7.1.7 "Fail the Connection":
  closing the connection abandons any partially assembled message; it can never be completed. This is
  a design decision: rather than weaken INV3, we keep the strong invariant "CLOSED implies no in-flight
  fragment" and make the close transitions enforce it. CLOSING (one-sided close) does NOT discard, since
  the peer may still legitimately finish the message before the second Close frame arrives.

### フラグメント結合(RFC 5.4)

- R7 (event): WHEN a data frame (text/binary) with FIN=0 arrives while frag is NONE THEN the endpoint SHALL record the message type (TEXT or BIN) into frag.
- R8 (event): WHEN a continuation frame (FIN=0) arrives while frag is not NONE THEN the endpoint SHALL stay in that frag type.
  Modeling note: "stay in that frag type" is a no-op on the abstract state (frag is unchanged between
  StartFrag and FinishFrag by construction). A dedicated ContinueFrag action would be a pure self-loop
  (UNCHANGED vars) that no invariant can constrain, so it is NOT a separate disjunct in Next. R8 is
  discharged structurally: frag is set once by StartFrag (R7) and only cleared by FinishFrag (R9) or a
  close transition (R6a); nothing in between mutates it. Keeping a vacuous action would only create a
  mutation-oracle survivor with no design meaning.
- R9 (event): WHEN a final frame (FIN=1) completing the message arrives THEN the endpoint SHALL reset frag to NONE.
- R10 (unwanted): IF a new data frame (non-continuation, text/binary) arrives WHILE frag is not NONE THEN it is a protocol error (interleaved messages forbidden) and the endpoint SHALL fail the connection.
- R11 (unwanted): IF a continuation frame arrives WHILE frag is NONE THEN it is a protocol error (no message to continue) and the endpoint SHALL fail the connection.

### 制御フレーム(RFC 5.4 / 5.5)

- R12 (ubiquitous): A control frame (close/ping/pong) SHALL be processable at any time during OPEN/CLOSING regardless of frag, and SHALL NOT alter frag (control frames may be injected mid-fragment).
  Modeling note: same as R8. A ping/pong mutates none of the abstract state, so it is not a separate
  Next disjunct (it would be a vacuous self-loop). The Close control frame IS modeled, as SendClose /
  RecvClose. "Control frame does not alter frag" is discharged structurally: only StartFrag, FinishFrag
  and the close transitions ever write frag.

## 安全性不変条件(model-check 対象)

- INV1 (R5/R6): state = CLOSED  <=>  (sentClose /\ rcvdClose). CLOSED は双方向 close 完了と同値。
- INV2 (R2/R6): フレーム処理(data/control)は state \in {OPEN, CLOSING} のときだけ起こる。
- INV3 (R7-R11): frag # NONE は「未完了メッセージあり」と厳密に対応。CLOSED 状態で frag = NONE。
- INV4 (R3/R6): sentClose は単調(一度 TRUE になったら FALSE に戻らない)。rcvdClose も同様。
  これは primed 変数を含む action property のため通常の INVARIANT では検査できない。Monotone /
  FragStep / StateProgress をまとめた temporal action property `CloseStep == [][...]_vars` を
  PROPERTY で検査する(下記 cfg 参照)。
  - FragStep (R10/R11): frag が変化する遷移は端点の一方が NONE。すなわち NONE↔data の往復のみで、
    data→別data の直接遷移(2メッセージのインターリーブ = R10 protocol error)は起きない。
  - StateProgress (R5): ライフサイクルは CONNECTING(0) ≤ OPEN(1) ≤ CLOSING(2) ≤ CLOSED(3) と
    rank が単調増加し、後退しない。CLOSED から再 OPEN するような退行遷移を action property で捕まえる。
- INV5 (R3/R4): state = CLOSING <=> (sentClose /\ ~rcvdClose). CLOSING は「自分は Close 送信済み、
  相手はまだ」という half-open close と同値。close ハンドシェイクの2ビットとライフサイクル状態を完全に
  結びつけ、SendClose が OPEN からのみ前進する(二重送信しない)ことを観測可能にする。これが無いと
  「SendClose のガードを外す」変異が idempotent な no-op として survive してしまう(mutation oracle で発見)。
- INV6 (R1): state = CONNECTING => (~sentClose /\ ~rcvdClose). ハンドシェイク前に close ビットは立たない。
  INV1/INV5 と合わせて、4状態それぞれが (sentClose, rcvdClose) の組と一意対応することを保証する
  (CONNECTING/OPEN は両ビット FALSE、CLOSING は send のみ、CLOSED は両方)。

## 状態遷移と EARS の対応(Next の disjunct)

| Action      | EARS | from                      | to                                  |
|-------------|------|---------------------------|-------------------------------------|
| Handshake   | R1   | CONNECTING                | OPEN                                |
| SendClose   | R3   | OPEN                      | CLOSING, sentClose:=TRUE            |
| RecvClose   | R4   | OPEN/CLOSING              | rcvdClose:=TRUE (+reply if !sent)   |
| StartFrag   | R7   | OPEN/CLOSING, frag=NONE   | frag:=TEXT|BIN                      |
| FinishFrag  | R9   | OPEN/CLOSING, frag#NONE   | frag:=NONE                          |
| Terminated  | R6   | CLOSED                    | self-loop (terminal, no frames)     |

(R8 continuation / R12 ping-pong are no-ops on the abstract state and are discharged
structurally, not as Next disjuncts -- see their modeling notes above.)

R10/R11 は「起きてはならない遷移」= Inv で禁止し、内ループで反例化する。
R6a により SendClose/RecvClose は CLOSED へ遷移する瞬間に frag を NONE にリセットする。
Terminated は CLOSED を終端状態として明示する自己ループ(R6: CLOSED ではフレーム処理が起きない)。
これが無いと CLOSED が吸い込み状態になり TLC が deadlock を報告する(設計上の終端を deadlock と
誤認しないため）。
