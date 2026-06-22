/-
WsProto — formal model of the byte/bit-level WebSocket primitives (RFC 6455).
Properties P1..P7 (see proofs/PROPERTIES.md). State machine is verified in TLA+
(spec/WsLifecycle.tla); this file owns the arithmetic/bitwise core.

Definitions are shaped to map 1:1 onto the C implementation so the bridge
tests can pin proof <-> code. Proofs are filled by the proof-repair loop;
`sorry` marks open goals — none may remain in a "verified" claim.
-/

-- Kept so `Main.lean` (which prints `hello`) still builds.
def hello := "world"

namespace WsProto

/-! ## P1, P2 — masking (RFC 6455 §5.3) -/

/-- Mask/unmask one payload: `out[i] = in[i] XOR key[i mod 4]`. Single op for both. -/
def maskBytes (key : Fin 4 → UInt8) (p : List UInt8) : List UInt8 :=
  p.mapIdx (fun i b => b ^^^ key (Fin.ofNat 4 i % 4))

/-- P2: masking preserves length. -/
theorem mask_length (key : Fin 4 → UInt8) (p : List UInt8) :
    (maskBytes key p).length = p.length := by
  simp [maskBytes]

/-- UInt8 XOR is its own inverse: `x ^^^ k ^^^ k = x`. -/
theorem uint8_xor_xor_cancel (x k : UInt8) : x ^^^ k ^^^ k = x := by
  apply UInt8.toBitVec_inj.1
  simp [BitVec.xor_assoc]

/-- P1: masking is an involution (XOR self-inverse) — unmask(mask(p)) = p. -/
theorem mask_involution (key : Fin 4 → UInt8) (p : List UInt8) :
    maskBytes key (maskBytes key p) = p := by
  unfold maskBytes
  rw [List.mapIdx_mapIdx]
  -- the composite map sends each byte b at index i to `b ^^^ k_i ^^^ k_i = b`.
  apply List.ext_getElem
  · simp
  · intro i h₁ h₂
    simp only [List.getElem_mapIdx, Function.comp_apply]
    exact uint8_xor_xor_cancel _ _

/-! ## P3, P4 — payload length encoding (RFC 6455 §5.2) -/

/-- The three on-wire length forms. -/
inductive LenForm where
  | tiny  (n : UInt8)            -- 0..125 immediate in the 7-bit field
  | short (n : UInt16)           -- 126: next 2 bytes
  | long  (n : UInt64)           -- 127: next 8 bytes
  deriving DecidableEq, Repr

/-- Encode a payload length into the minimal wire form (RFC 6455 §5.2). -/
def encodeLen (n : Nat) : LenForm :=
  if n < 126 then .tiny (UInt8.ofNat n)
  else if n ≤ 0xFFFF then .short (UInt16.ofNat n)
  else .long (UInt64.ofNat n)

/-- Decode a wire form back to a length. -/
def decodeLen : LenForm → Nat
  | .tiny n  => n.toNat
  | .short n => n.toNat
  | .long n  => n.toNat

/-- P3: encode/decode roundtrip for any length within the 64-bit wire range. -/
theorem len_roundtrip (n : Nat) (h : n < 2 ^ 64) :
    decodeLen (encodeLen n) = n := by
  unfold encodeLen
  split
  · -- n < 126 < 256, tiny path
    rename_i h126
    simp only [decodeLen]
    rw [UInt8.toNat_ofNat_of_lt' (by simp only [UInt8.size]; omega)]
  · split
    · -- 126 ≤ n ≤ 0xFFFF < 2^16, short path
      rename_i _ hF
      simp only [decodeLen]
      rw [UInt16.toNat_ofNat_of_lt' (by simp only [UInt16.size]; omega)]
    · -- 0xFFFF < n < 2^64, long path
      rename_i _ hF
      simp only [decodeLen]
      rw [UInt64.toNat_ofNat_of_lt' (by simp only [UInt64.size]; omega)]

/-- P4: the chosen form is the minimal one and the boundaries are decisive. -/
theorem encode_tiny  (n : Nat) (h : n < 126) :
    encodeLen n = .tiny (UInt8.ofNat n) := by
  unfold encodeLen; simp [h]
theorem encode_short (n : Nat) (h1 : 126 ≤ n) (h2 : n ≤ 0xFFFF) :
    encodeLen n = .short (UInt16.ofNat n) := by
  unfold encodeLen
  rw [if_neg (by omega), if_pos h2]
theorem encode_long  (n : Nat) (h : 0xFFFF < n) :
    encodeLen n = .long (UInt64.ofNat n) := by
  unfold encodeLen
  rw [if_neg (by omega), if_neg (by omega)]

/-! ## P5, P6 — opcode classification + control constraint (RFC 6455 §5.2, §5.5) -/

inductive OpClass where
  | data | control | reserved
  deriving DecidableEq, Repr

/-- Classify a 4-bit opcode. cont/text/binary = data; close/ping/pong = control;
    rest reserved. (RFC 6455 §5.2, §11.8) -/
def classifyOpcode (op : Fin 16) : OpClass :=
  match (op : Nat) with
  | 0x0 | 0x1 | 0x2 => .data
  | 0x8 | 0x9 | 0xA => .control
  | _ => .reserved

/-- P5: classification is total (every opcode lands somewhere — true by totality of
    the function; stated to pin exhaustiveness) and the known opcodes are exact. -/
theorem classify_close   : classifyOpcode 0x8 = .control := by decide
theorem classify_text    : classifyOpcode 0x1 = .data := by decide
theorem classify_reserved3 : classifyOpcode 0x3 = .reserved := by decide

/-- P5 exhaustiveness: every 4-bit opcode is classified into exactly one class. -/
theorem classify_total (op : Fin 16) :
    classifyOpcode op = .data ∨ classifyOpcode op = .control ∨
    classifyOpcode op = .reserved := by
  revert op; decide

/-- P6: a control frame's payload must be ≤ 125 (RFC 6455 §5.5). The validity
    predicate must reject any control frame with a larger payload. -/
def controlPayloadOk (cls : OpClass) (len : Nat) : Bool :=
  match cls with
  | .control => len ≤ 125
  | _        => true

theorem control_rejects_big (len : Nat) (h : 125 < len) :
    controlPayloadOk .control len = false := by
  simp only [controlPayloadOk, decide_eq_false_iff_not, Nat.not_le]
  omega
theorem control_accepts_small (len : Nat) (h : len ≤ 125) :
    controlPayloadOk .control len = true := by
  simp only [controlPayloadOk, decide_eq_true_eq]
  omega

/-! ## P7 — close code validity on the wire (RFC 6455 §7.4.1) -/

/-- May this close code appear on the wire? 1000-1011 except the reserved
    1004/1005/1006 (and 1015); plus app range 3000-4999.

    NOTE (def-body change, for the proof): the skeleton mixed `Bool` (`||`)
    with `Prop` (`∧`), which does not typecheck. Rewritten as a decidable
    `Bool` predicate using `decide`. Intent is unchanged: 1000-1011 minus
    {1004,1005,1006} (1015 is already outside 1000-1011 so excluded), plus
    the app range 3000-4999. -/
def closeCodeSendable (c : Nat) : Bool :=
  (decide (1000 ≤ c ∧ c ≤ 1011) && c ≠ 1004 && c ≠ 1005 && c ≠ 1006) ||
  decide (3000 ≤ c ∧ c ≤ 4999)

theorem close_1005_forbidden : closeCodeSendable 1005 = false := by decide
theorem close_1006_forbidden : closeCodeSendable 1006 = false := by decide
theorem close_1015_forbidden : closeCodeSendable 1015 = false := by decide
theorem close_1000_ok : closeCodeSendable 1000 = true := by decide
theorem close_app_ok : closeCodeSendable 3000 = true := by decide

end WsProto
