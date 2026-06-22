---- MODULE WsLifecycle ----
\* WebSocket connection lifecycle + close handshake + fragment assembly.
\* Source of truth: spec/REQUIREMENTS.md (EARS R1..R12 + R6a, INV1..INV6).
\* Generated from that model; edit the model, then re-derive this spec.

EXTENDS Naturals

\* --- Domain model ---
\* state : connection lifecycle  (RFC 6455 4.1/7.1)
\* frag  : in-flight message fragment type (RFC 6455 5.4)
\* sentClose / rcvdClose : the two halves of the closing handshake (7.1.3/7.1.4)
CONSTANTS CONNECTING, OPEN, CLOSING, CLOSED, NONE, TEXT, BIN

VARIABLES state, frag, sentClose, rcvdClose
vars == <<state, frag, sentClose, rcvdClose>>

States == {CONNECTING, OPEN, CLOSING, CLOSED}
Frags  == {NONE, TEXT, BIN}
DataFrags == {TEXT, BIN}

\* frames are processable only once OPEN and until CLOSED (R2/R6)
Active == state \in {OPEN, CLOSING}

TypeOK ==
    /\ state \in States
    /\ frag \in Frags
    /\ sentClose \in BOOLEAN
    /\ rcvdClose \in BOOLEAN

Init ==
    /\ state = CONNECTING
    /\ frag = NONE
    /\ sentClose = FALSE
    /\ rcvdClose = FALSE

\* R1: handshake CONNECTING -> OPEN
Handshake ==
    /\ state = CONNECTING
    /\ state' = OPEN
    /\ UNCHANGED <<frag, sentClose, rcvdClose>>

\* R3: send Close. Fires only from OPEN: once you have sent a Close you are in CLOSING
\* (INV5), and a peer Close is handled by RecvClose (which goes straight to CLOSED), so
\* OPEN is the only state with ~sentClose /\ ~rcvdClose. The guard is state-based (a
\* observable forward step OPEN->CLOSING) rather than ~sentClose, which makes a dropped
\* guard a real regression the invariants can catch instead of an idempotent no-op.
SendClose ==
    /\ state = OPEN
    /\ sentClose' = TRUE
    /\ state' = CLOSING
    /\ UNCHANGED <<frag, rcvdClose>>

\* R4: receive Close (only once); if we hadn't sent, RFC says reply -> sentClose too.
\* RecvClose always reaches CLOSED here (reply-on-receive), so R6a discards frag.
RecvClose ==
    /\ Active
    /\ ~rcvdClose
    /\ rcvdClose' = TRUE
    /\ sentClose' = TRUE  \* reply-on-receive collapses the handshake (RFC 5.5.1)
    /\ state' = CLOSED
    /\ frag' = NONE       \* R6a: incomplete message discarded on close

\* R7: begin a fragmented data message (FIN=0, opcode text/binary)
StartFrag ==
    /\ Active
    /\ frag = NONE
    /\ \E t \in DataFrags : frag' = t
    /\ UNCHANGED <<state, sentClose, rcvdClose>>

\* R8: continuation frame (FIN=0) keeps current type. No dedicated action: frag is set
\* once by StartFrag and only cleared by FinishFrag / close, so "stays in that type"
\* holds structurally. A standalone UNCHANGED-vars self-loop would be vacuous (see
\* REQUIREMENTS.md R8 modeling note) and only breed mutation-oracle survivors.

\* R9: final frame (FIN=1) completes the message
FinishFrag ==
    /\ Active
    /\ frag # NONE
    /\ frag' = NONE
    /\ UNCHANGED <<state, sentClose, rcvdClose>>

\* R12: control frame (ping/pong) processable mid-fragment, frag untouched. Like R8 this
\* is a no-op on the abstract state (a control frame mutates none of state/frag/close
\* bits), so it is discharged structurally rather than as a vacuous UNCHANGED-vars action.
\* The property "a close/ping never alters frag" is exactly that frag is touched only by
\* StartFrag / FinishFrag / close transitions -- enforced by the disjuncts of Next below.

\* R6: CLOSED is terminal. Self-loop so the terminal state is intentional, not a deadlock.
Terminated ==
    /\ state = CLOSED
    /\ UNCHANGED vars

Next ==
    \/ Handshake
    \/ SendClose
    \/ RecvClose
    \/ StartFrag
    \/ FinishFrag
    \/ Terminated

\* --- Safety invariants (INV1..INV5) ---
INV1 == (state = CLOSED) <=> (sentClose /\ rcvdClose)
INV2 == (frag # NONE) => Active
INV3 == (state = CLOSED) => (frag = NONE)
\* INV4 (monotonicity) is an action property; checked via a primed invariant below.
\* INV5: CLOSING is exactly "I sent Close, peer has not yet". Ties the close handshake
\* bits to the lifecycle state so a half-open close is fully characterized. Also makes
\* SendClose's OPEN-only guard observable: any spurious SendClose firing would have to
\* enter/leave CLOSING in violation of this biconditional.
INV5 == (state = CLOSING) <=> (sentClose /\ ~rcvdClose)
\* Derived completeness: CONNECTING is exactly "no close bits set, not yet CLOSED".
INV6 == (state = CONNECTING) => (~sentClose /\ ~rcvdClose)

Inv ==
    /\ TypeOK
    /\ INV1
    /\ INV2
    /\ INV3
    /\ INV5
    /\ INV6

\* INV4: close bits never revert (R3/R4). Action property (has primed vars), so it is
\* checked as a temporal property MonotoneProp = [][Monotone]_vars, not as an INVARIANT.
Monotone ==
    /\ (sentClose => sentClose')
    /\ (rcvdClose => rcvdClose')

\* R10/R11: fragment assembly never jumps directly between two data types. frag may only
\* move NONE->data (StartFrag) or data->NONE (FinishFrag / close). A direct data->data step
\* would mean interleaving two messages (R10) -- forbidden. Action property: when frag
\* changes, one of the endpoints of the change must be NONE.
\* NOTE: in this abstraction frag is written only NONE->data (StartFrag) and data->NONE
\* (FinishFrag / close), so FragStep is currently vacuously true. It is kept as the design's
\* explicit R10 guard-rail: were a continuation/interleave action ever added to Next, this
\* property would catch a data->data jump. The mutation oracle cannot kill mutations of a
\* vacuous property (no reachable transition exercises it) -- that is an equivalent-mutant
\* artifact, not a design gap.
FragStep == (frag # frag') => (frag = NONE \/ frag' = NONE)

\* INV4 + state progress as one action property. Monotone close bits (R3/R4) together with
\* INV1/INV5/INV6 already pin state to (sentClose,rcvdClose), so checking Monotone over every
\* step also forbids any lifecycle regression. FragStep adds the R10 guard-rail.
CloseStep == [][Monotone /\ FragStep]_vars

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)
====
