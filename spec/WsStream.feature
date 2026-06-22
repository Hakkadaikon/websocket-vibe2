# Generated from spec/STREAM_REQUIREMENTS.md (EARS S1..S14 + S9a) and the WsStream.tla
# model-checked design. Do not hand-edit: change the EARS source + model, then re-derive.
#
# Negative scenarios are TLC counterexamples (design-level bugs that MUST fail). Positive
# scenarios are the EARS happy paths (S4-S6 / S10 / S11), to become driver acceptance tests
# for ws_conn_recv / ws_conn_poll. State = (state, frag, msgLen, rxLen, failed, lastEvent),
# CAP=3, frame payloads abstracted to lengths.

Feature: WebSocket byte-stream to semantic-event driver (fragment assembly + overflow)

  Background:
    Given a fresh driver with state=CONNECTING frag=NONE msgLen=0 rxLen=0 failed=FALSE lastEvent=NONE
    And the connection has completed its handshake so state=OPEN

  # ---- S4-S6: fragmented message assembled then delivered (happy path) ----
  Scenario: A fragmented text message is assembled across frames and delivered as one MESSAGE
    Given bytes have arrived so rxLen=2
    When poll parses a data START frame (TEXT, FIN=0) with payload 1 while frag=NONE
    Then frag becomes TEXT and msgLen becomes 1 and lastEvent becomes NONE
    When poll parses a final frame (FIN=1) completing the message with payload 1
    Then lastEvent becomes MESSAGE
    And frag becomes NONE and msgLen becomes 0
    # SINV5: right after a MESSAGE emit we are back at a message boundary.

  Scenario: A continuation frame keeps the fragment type and accumulates
    Given a TEXT fragment is in flight with frag=TEXT msgLen=1 rxLen=1
    When poll parses a continuation frame (FIN=0) with payload 1 while frag#NONE
    Then frag stays TEXT and msgLen becomes 2 and lastEvent becomes NONE

  # ---- S10: control frames are interleavable and leave assembly untouched ----
  Scenario: A PING mid-fragment is reported without disturbing the in-flight message
    Given a TEXT fragment is in flight with frag=TEXT msgLen=1 rxLen=1
    When poll parses a PING control frame
    Then lastEvent becomes PING
    And frag stays TEXT and msgLen stays 1
    # S10: SHALL NOT alter frag or msgLen.

  # ---- S11: close frame drives the lifecycle and discards in-flight assembly ----
  Scenario: A CLOSE frame emits CLOSE, reaches CLOSED, and discards any in-flight message
    Given a TEXT fragment is in flight with frag=TEXT msgLen=1 rxLen=1
    When poll parses a CLOSE control frame
    Then lastEvent becomes CLOSE
    And state becomes CLOSED and frag becomes NONE and msgLen becomes 0 and rxLen becomes 0
    # WsLifecycle R6a: incomplete message discarded on close.

  # ---- S2 / SINV1: receive overflow is rejected, buffer never exceeds CAP ----
  Scenario: Bytes that would overflow the CAP-sized buffer are rejected, rxLen unchanged
    Given a fragment is in flight with msgLen=2 rxLen=1 (msgLen+rxLen=CAP)
    When ws_conn_recv is offered a chunk that would push msgLen+rxLen past CAP
    Then the chunk is rejected (WS_ERR_TOO_SMALL) and rxLen stays 1
    # SINV1: msgLen + rxLen <= CAP always.

  # ---- S13 / SINV3 / SINV7: once failed, poll only ever returns ERROR ----
  Scenario: After a protocol violation latches failed, every later poll returns ERROR only
    Given failed=TRUE has been latched and frag=NONE msgLen=0
    When poll parses any further buffered frame
    Then lastEvent becomes ERROR
    And no MESSAGE/PING/PONG/CLOSE is ever emitted again
    # SINV3: failed => lastEvent in {ERROR, NONE}.  SINV7: lastEvent=ERROR => failed.

  # ==== NEGATIVE: TLC counterexamples — these behaviors MUST NOT happen ====

  # Counterexample that motivated SINV8 (S9a). The mutation oracle could not express this as a
  # nondeterministic operator flip; a deterministic "keep frag/msgLen on error" rewrite survived
  # SINV1-SINV7 because keeping frag#NONE AND msgLen>0 *consistently* satisfies SINV2's
  # biconditional. SINV8 (failed => frag=NONE /\ msgLen=0) now kills it.
  Scenario: REJECTED — an interleaving error must not leave the partial message assembled
    Given bytes have arrived so rxLen=2
    When poll parses a data START frame (TEXT, FIN=0) payload 1 so frag=TEXT msgLen=1 rxLen=1
    And poll parses another data START frame (TEXT, non-continuation) while frag#NONE  # S7 interleave
    Then the driver sets failed=TRUE and lastEvent=ERROR
    But it MUST NOT keep frag=TEXT with msgLen=1
    # S9a / SINV8: on any protocol violation the driver abandons assembly -> frag=NONE, msgLen=0.

  Scenario: REJECTED — finishing a message must not forget to reset the accumulator
    Given a TEXT fragment is in flight with frag=TEXT msgLen=1
    When poll parses a final frame and emits MESSAGE
    Then it MUST NOT leave msgLen>0 (which would break the SINV2 message boundary)
    # SINV2 / SINV5: after MESSAGE, frag=NONE and msgLen=0.

  Scenario: REJECTED — an error path must not fail to latch the failed flag
    Given frag=NONE so a parsed continuation frame is a stray continuation  # S8
    When poll classifies it as ERROR
    Then it MUST also set failed=TRUE
    # SINV7: lastEvent=ERROR => failed. An ERROR without latching failed is forbidden.
