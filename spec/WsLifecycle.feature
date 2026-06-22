Feature: WsLifecycle
  # Source of truth: spec/REQUIREMENTS.md (EARS R1..R12, R6a, INV1..INV6).
  # Negative scenarios come from TLC counterexample traces (inner loop); positive
  # scenarios are the EARS happy paths. Regenerate from the model, do not hand-drift.
  #
  # EARS ubiquitous: "The <system> SHALL <response>."
  # EARS event:      "WHEN <trigger> the <system> SHALL <response>."
  # EARS state:      "WHILE <state> the <system> SHALL <response>."
  # EARS unwanted:   "IF <condition> THEN the <system> SHALL <response>."

  # ---------------------------------------------------------------------------
  # NEGATIVE: must-not-happen behaviour, recovered from a TLC INV3 counterexample
  # before the design was fixed. Kept as a regression acceptance test.
  # ---------------------------------------------------------------------------
  Scenario: A close handshake must not leave a half-assembled message behind (INV3)
    # Original TLC trace (pre-fix), Inv (INV3) violated:
    #   State1 Init:      state=CONNECTING frag=NONE sentClose=F rcvdClose=F
    #   State2 Handshake: state=OPEN       frag=NONE sentClose=F rcvdClose=F
    #   State3 StartFrag: state=OPEN       frag=TEXT sentClose=F rcvdClose=F
    #   State4 RecvClose: state=CLOSED     frag=TEXT  <-- CLOSED with frag still TEXT
    Given an OPEN connection
    And a fragmented TEXT message is in flight (frag = TEXT)
    When a Close frame is received and the connection reaches CLOSED
    Then the connection must not remain with frag = TEXT
    # Fixed by R6a: the close transition discards the incomplete message (frag := NONE).
    But the incomplete message is discarded so that frag = NONE

  # ---------------------------------------------------------------------------
  # POSITIVE: EARS happy paths (now hold for all reachable states, TLC: No error)
  # ---------------------------------------------------------------------------
  Scenario: Handshake opens the connection (R1)
    Given a connection in state CONNECTING with no close bits set
    When the handshake succeeds
    Then the connection becomes OPEN

  Scenario: Sender-initiated close goes through CLOSING then CLOSED (R3, R4, R5)
    Given an OPEN connection
    When a Close frame is sent
    Then the connection becomes CLOSING with sentClose set and rcvdClose clear
    When the peer Close frame is then received
    Then the connection becomes CLOSED with both close bits set

  Scenario: Reply-on-receive collapses the handshake to CLOSED (R4, R5)
    Given an OPEN connection that has not sent a Close
    When a Close frame is received
    Then the endpoint replies with a Close (sentClose set) and becomes CLOSED

  Scenario: A fragmented message assembles then completes (R7, R9)
    Given an OPEN connection with frag = NONE
    When a data frame with FIN=0 begins a TEXT message
    Then frag becomes TEXT
    When the final frame with FIN=1 arrives
    Then frag returns to NONE

  Scenario: CLOSED is terminal (R6)
    Given a CLOSED connection
    When time passes
    Then the connection stays CLOSED and processes no further frames

  Scenario: The lifecycle state is fully determined by the close bits (INV1, INV5, INV6)
    Given any reachable connection
    Then CONNECTING and OPEN imply neither close bit is set
    And CLOSING implies exactly sentClose set and rcvdClose clear
    And CLOSED implies both close bits set
