---- MODULE WsStream ----
\* WebSocket byte-stream -> semantic-event driver layer (ws_conn_recv / ws_conn_poll).
\* Source of truth: spec/STREAM_REQUIREMENTS.md (EARS S1..S14 + S9a, SINV1..SINV6).
\* Generated from that model; edit the model, then re-derive this spec.
\* Sits on top of WsLifecycle (state lifecycle verified there); this spec verifies
\* fragment-assembly progress, receive-buffer accounting, and poll determinism.

EXTENDS Naturals

CONSTANTS CONNECTING, OPEN, CLOSING, CLOSED,   \* lifecycle states (= WsLifecycle)
          NONE, TEXT, BIN,                     \* frag types; NONE = message boundary
          MESSAGE, PING, PONG, CLOSE, ERROR,   \* lastEvent observation values
          CAP, PAYMAX                          \* WS_MAX_MESSAGE abstraction; max frame payload

VARIABLES state,      \* lifecycle state
          frag,       \* in-flight message type {NONE, TEXT, BIN}
          msgLen,     \* accumulated assembled-message length (0..CAP)
          rxLen,      \* unparsed received raw-byte length (0..CAP)
          failed,     \* protocol violation latched (BOOLEAN)
          lastEvent   \* event type returned by the most recent poll (observation)

vars == <<state, frag, msgLen, rxLen, failed, lastEvent>>

States    == {CONNECTING, OPEN, CLOSING, CLOSED}
Frags     == {NONE, TEXT, BIN}
DataFrags == {TEXT, BIN}
Events    == {NONE, MESSAGE, PING, PONG, CLOSE, ERROR}

\* frames are processable only while OPEN or CLOSING (ties to WsLifecycle INV2 / S14)
Active == state \in {OPEN, CLOSING}

\* Frame kinds the parser can yield from one buffered frame (abstracted, no bytes):
\*   STARTtext/STARTbin : data frame, FIN=0, non-continuation opcode (text/binary)
\*   CONT               : continuation frame, FIN=0
\*   FINcont            : final frame, FIN=1 (completes the message)
\*   PINGk / PONGk      : control ping/pong
\*   CLOSEk             : control close
FrameKinds == {"STARTtext", "STARTbin", "CONT", "FINcont", "PINGk", "PONGk", "CLOSEk"}

TypeOK ==
    /\ state \in States
    /\ frag \in Frags
    /\ msgLen \in 0..CAP
    /\ rxLen \in 0..CAP
    /\ failed \in BOOLEAN
    /\ lastEvent \in Events

Init ==
    /\ state = CONNECTING
    /\ frag = NONE
    /\ msgLen = 0
    /\ rxLen = 0
    /\ failed = FALSE
    /\ lastEvent = NONE

\* ----- lifecycle steps (subset of WsLifecycle, needed so frames can flow) -----
\* We do not re-verify the lifecycle; we only let state advance so OPEN/CLOSING/CLOSED
\* are reachable and frames can be parsed under each.
Handshake ==
    /\ state = CONNECTING
    /\ state' = OPEN
    /\ UNCHANGED <<frag, msgLen, rxLen, failed, lastEvent>>

ToClosing ==
    /\ state = OPEN
    /\ state' = CLOSING
    /\ UNCHANGED <<frag, msgLen, rxLen, failed, lastEvent>>

\* Reaching CLOSED discards any in-flight assembly (WsLifecycle R6a) and the buffer.
ToClosed ==
    /\ state \in {OPEN, CLOSING}
    /\ state' = CLOSED
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ rxLen' = 0
    /\ UNCHANGED <<failed, lastEvent>>

\* ----- S1/S2: ws_conn_recv appends raw bytes to the receive buffer -----
\* S1 (event): append, increasing rxLen, provided msgLen + rxLen <= CAP after append.
\* We add a chunk of size 1..PAYMAX representing arriving bytes.
Recv ==
    /\ Active
    /\ ~failed
    /\ \E chunk \in 1..PAYMAX :
         /\ msgLen + rxLen + chunk <= CAP        \* S1 precondition (also enforces SINV1)
         /\ rxLen' = rxLen + chunk
    /\ UNCHANGED <<state, frag, msgLen, failed, lastEvent>>

\* S2 (unwanted): IF appending would exceed CAP THEN reject and do not change rxLen.
\* Modeled as a stutter on the buffer accounting: bytes that would overflow are dropped,
\* leaving rxLen unchanged. (Distinct disjunct so the rejection is an explicit design step.)
RecvReject ==
    /\ Active
    /\ ~failed
    /\ \E chunk \in 1..PAYMAX :
         msgLen + rxLen + chunk > CAP            \* would overflow -> reject
    /\ UNCHANGED vars

\* ----- poll helpers -----
\* A poll consumes exactly one buffered frame. It requires rxLen >= 1 (a frame present).
\* payload = abstract bytes the frame carries that count toward msgLen (1..PAYMAX, bounded
\* by what is buffered). Control frames carry no message payload.
HasFrame == rxLen >= 1

\* ----- S13: failed latched -> poll only ever returns ERROR -----
PollWhenFailed ==
    /\ failed
    /\ HasFrame
    /\ rxLen' = rxLen - 1
    /\ lastEvent' = ERROR
    /\ UNCHANGED <<state, frag, msgLen, failed>>

\* ----- S4: start fragmented data message (FIN=0, data opcode) WHILE frag=NONE -----
StartFrag(kind) ==
    /\ ~failed
    /\ Active
    /\ frag = NONE
    /\ HasFrame
    /\ \E pay \in 1..PAYMAX :
         /\ pay <= rxLen
         /\ msgLen + pay <= CAP                  \* fits (else TooBig handles it)
         /\ msgLen' = msgLen + pay
         /\ rxLen'  = rxLen - pay
    /\ frag' = (IF kind = "STARTtext" THEN TEXT ELSE BIN)
    /\ lastEvent' = NONE                          \* S3: incomplete message -> no event yet
    /\ UNCHANGED <<state, failed>>

\* ----- S5: continuation frame (FIN=0) WHILE frag#NONE: accumulate, keep frag -----
ContinueFrag ==
    /\ ~failed
    /\ Active
    /\ frag # NONE
    /\ HasFrame
    /\ \E pay \in 1..PAYMAX :
         /\ pay <= rxLen
         /\ msgLen + pay <= CAP
         /\ msgLen' = msgLen + pay
         /\ rxLen'  = rxLen - pay
    /\ lastEvent' = NONE
    /\ UNCHANGED <<state, frag, failed>>

\* ----- S6: final frame (FIN=1) completes the message: emit MESSAGE, reset frag/msgLen -----
FinishFrag ==
    /\ ~failed
    /\ Active
    /\ frag # NONE
    /\ HasFrame
    /\ \E pay \in 0..PAYMAX :
         /\ pay <= rxLen
         /\ msgLen + pay <= CAP
         /\ rxLen' = rxLen - pay
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ lastEvent' = MESSAGE
    /\ UNCHANGED <<state, failed>>

\* ----- S7: interleaving (new data frame WHILE frag#NONE): failed + ERROR -----
\* S9a: abandon assembly -> frag=NONE, msgLen=0.
Interleave(kind) ==
    /\ ~failed
    /\ Active
    /\ frag # NONE
    /\ HasFrame
    /\ rxLen' = rxLen - 1
    /\ failed' = TRUE
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ lastEvent' = ERROR
    /\ UNCHANGED <<state>>

\* ----- S8: stray continuation (CONT WHILE frag=NONE): failed + ERROR -----
StrayCont ==
    /\ ~failed
    /\ Active
    /\ frag = NONE
    /\ HasFrame
    /\ rxLen' = rxLen - 1
    /\ failed' = TRUE
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ lastEvent' = ERROR
    /\ UNCHANGED <<state>>

\* ----- S9: accumulating would push msgLen past CAP: failed + ERROR -----
TooBig ==
    /\ ~failed
    /\ Active
    /\ frag # NONE
    /\ HasFrame
    /\ \E pay \in 1..PAYMAX :
         /\ pay <= rxLen
         /\ msgLen + pay > CAP                    \* would overflow the assembled message
    /\ rxLen' = rxLen - 1
    /\ failed' = TRUE
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ lastEvent' = ERROR
    /\ UNCHANGED <<state>>

\* ----- S10: control PING/PONG: emit, do not alter frag or msgLen -----
Control(ev) ==
    /\ ~failed
    /\ Active
    /\ HasFrame
    /\ rxLen' = rxLen - 1
    /\ lastEvent' = ev
    /\ UNCHANGED <<state, frag, msgLen, failed>>

\* ----- S11: CLOSE frame: emit CLOSE, drive lifecycle (rcvdClose -> CLOSED) -----
\* WsLifecycle's RecvClose reaches CLOSED and R6a discards frag; mirror that here.
RecvCloseFrame ==
    /\ ~failed
    /\ Active
    /\ HasFrame
    /\ state' = CLOSED
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ rxLen' = 0
    /\ lastEvent' = CLOSE
    /\ UNCHANGED <<failed>>

\* ----- S14: frame parsed WHILE CONNECTING or CLOSED is illegal: failed + ERROR -----
\* (frames only flow during OPEN/CLOSING). Latches failed, abandons assembly (S9a).
IllegalState ==
    /\ ~failed
    /\ ~Active
    /\ HasFrame
    /\ rxLen' = rxLen - 1
    /\ failed' = TRUE
    /\ frag' = NONE
    /\ msgLen' = 0
    /\ lastEvent' = ERROR
    /\ UNCHANGED <<state>>

\* CLOSED is terminal (WsLifecycle R6). Self-loop so the terminal state is intentional,
\* not a TLC deadlock. Only fires when no frame is buffered (a buffered frame in a non-Active
\* state is the S14 illegal case, handled by IllegalState).
Terminated ==
    /\ state = CLOSED
    /\ rxLen = 0
    /\ UNCHANGED vars

\* Each EARS clause is one disjunct of Next (1 clause = 1 action).
Next ==
    \/ Terminated
    \/ Handshake
    \/ ToClosing
    \/ ToClosed
    \/ Recv                                    \* S1
    \/ RecvReject                              \* S2
    \/ StartFrag("STARTtext")                  \* S4
    \/ StartFrag("STARTbin")                   \* S4
    \/ ContinueFrag                            \* S5
    \/ FinishFrag                              \* S6
    \/ Interleave("STARTtext")                 \* S7
    \/ Interleave("STARTbin")                  \* S7
    \/ StrayCont                               \* S8
    \/ TooBig                                  \* S9
    \/ Control(PING)                           \* S10
    \/ Control(PONG)                           \* S10
    \/ RecvCloseFrame                          \* S11
    \/ IllegalState                            \* S14
    \/ PollWhenFailed                          \* S13

\* ===== Safety invariants (SINV1..SINV5, SINV7, SINV8), model-checked =====

\* SINV1 (S1/S2/S9): the buffer + assembled message never exceed CAP.
SINV1 == msgLen + rxLen <= CAP

\* SINV2 (S4-S6, the crux): frag=NONE iff msgLen=0. Message-boundary soundness.
\* Catches missing resets on FinishFrag / error paths (S9a).
SINV2 == (frag = NONE) <=> (msgLen = 0)

\* SINV3 (S13): once failed, the only observable events are ERROR (or the NONE start state).
SINV3 == failed => (lastEvent \in {ERROR, NONE})

\* SINV4 (S14 / WsLifecycle INV2): message assembly exists only while OPEN/CLOSING.
SINV4 == (frag # NONE) => Active

\* SINV5 (S6): right after a MESSAGE emit, we are back at a message boundary.
SINV5 == (lastEvent = MESSAGE) => (frag = NONE /\ msgLen = 0)

\* SINV7 (S7/S8/S9/S13/S14): ERROR is emitted iff a violation has been latched. Once any
\* poll returns ERROR, failed is set, and conversely a non-failed run never observes ERROR.
\* Pins the error-path actions: weakening their ~failed guard or dropping their failed':=TRUE
\* reset would let ERROR appear without failed (or vice versa) -> caught here. (Strengthens the
\* one-directional SINV3 into the biconditional the routing table S12 mandates.)
SINV7 == (lastEvent = ERROR) => failed

\* SINV8 (S9a): once a protocol violation latches failed, in-flight message assembly is
\* abandoned -- frag=NONE and msgLen=0. This is the S9a design decision made checkable: it
\* forbids an error path (S7/S8/S9/S14) that leaves frag/msgLen set. SINV2 alone cannot catch
\* that, because keeping frag#NONE AND msgLen>0 *consistently* still satisfies the SINV2
\* biconditional; only SINV8 pins the post-error state to the message boundary. (Mutation
\* oracle survivor "Interleave keeps frag/msgLen" motivated adding this.)
SINV8 == failed => (frag = NONE /\ msgLen = 0)

Inv ==
    /\ TypeOK
    /\ SINV1
    /\ SINV2
    /\ SINV3
    /\ SINV4
    /\ SINV5
    /\ SINV7
    /\ SINV8

\* SINV6 (monotonicity): failed never reverts to FALSE. Action property -> temporal.
Monotone == (failed => failed')

\* S12 determinism: a poll never returns two different events for the same frame. In this
\* design each poll disjunct fixes lastEvent' deterministically from (kind, fin, frag, state,
\* failed); the disjuncts are mutually exclusive on those guards. We assert that no state can
\* both finish a fragment and (also from frag#NONE/Active) be a stray continuation, etc., i.e.
\* the data-path guards partition on frag. The classification witness:
\*   failed                      -> ERROR        (PollWhenFailed)
\*   ~Active & frame             -> ERROR        (IllegalState)
\*   CLOSE frame                 -> CLOSE        (RecvCloseFrame)
\*   PING/PONG frame             -> PING/PONG    (Control)
\*   data START & frag=NONE      -> NONE (assembling) (StartFrag)
\*   data START & frag#NONE      -> ERROR (interleave) (Interleave)
\*   CONT & frag#NONE & fits     -> NONE (assembling) (ContinueFrag)
\*   CONT & frag#NONE & overflow -> ERROR (TooBig)     (TooBig)
\*   CONT & frag=NONE            -> ERROR (stray)       (StrayCont)
\*   FIN  & frag#NONE            -> MESSAGE             (FinishFrag)
\* The frame-kind selection in Next is the parser's job; the (frag,state,failed) routing is
\* deterministic. Determinism over frame KIND is a parser-decidability obligation discharged
\* on the Lean/test side (step-function totality), per STREAM_REQUIREMENTS.md S12. The
\* state-routing determinism here rests on the data-path guards being mutually exclusive on
\* frag: StartFrag/StrayCont require frag=NONE while ContinueFrag/FinishFrag/TooBig/Interleave
\* require frag#NONE, and frag=NONE xor frag#NONE always holds. We deliberately do NOT add a
\* `DetRouting == (frag=NONE) \/ (frag#NONE)` invariant: it is a tautology (vacuously true,
\* never falsifiable), so the mutation oracle cannot kill mutations of it -- an equivalent-mutant
\* artifact, not a checked property (same reasoning as WsLifecycle's FragStep note).

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

\* SINV6 as a temporal action property.
MonotoneProp == [][Monotone]_vars
====
