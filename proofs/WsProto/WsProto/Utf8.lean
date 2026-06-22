/-
WsProto.Utf8 — formal model of UTF-8 validation (RFC 3629 / RFC 6455 §8.1).
Property P8: the validator core.

The decoder reads one code point from the front of a byte list. The shape
(lead-byte classify -> continuation range guards -> code-point assembly) maps
1:1 onto the C implementation so the bridge tests can pin proof <-> code.

We prove the *critical core lemmas* (not full validator soundness, by the
YAGNI scope agreed with the caller):
  - P8.1 decidability/exhaustiveness of the per-step classification,
  - P8.2 range soundness: any accepted code point is a valid Unicode scalar
         value (<= 0x10FFFF and not a surrogate) — the main result,
  - P8.3 boundary rejection/acceptance on concrete vectors (overlong,
         surrogate, out-of-range) for the C bridge.

Proofs are filled by the proof-repair loop; no `sorry` may remain.
-/

namespace WsProto

/-! ## P8 — UTF-8 decode step (RFC 3629) -/

/-- Is `b` a UTF-8 continuation byte (0x80..0xBF)? -/
def isCont (b : UInt8) : Bool := 0x80 ≤ b && b ≤ 0xBF

/-- Low 6 bits of a continuation byte, as a `Nat`. -/
def contBits (b : UInt8) : Nat := b.toNat % 0x40

/-- Decode one code point from the front of `bytes`.
    Returns `(codepoint, rest)` or `none` if the prefix is not well-formed
    UTF-8 per RFC 3629 (overlong forms, surrogates, and > U+10FFFF rejected).

    Lead-byte ranges and the special continuation guards on the first
    continuation byte (the rows the C side must replicate):
      1-byte  0x00..0x7F
      2-byte  0xC2..0xDF, cont                     (0xC0/0xC1 overlong rejected)
      3-byte  0xE0,       cont in 0xA0..0xBF, cont  (0xE0 overlong guard)
              0xED,       cont in 0x80..0x9F, cont  (0xED surrogate guard)
              0xE1..0xEF (except 0xED), cont, cont
      4-byte  0xF0,       cont in 0x90..0xBF, cont, cont (0xF0 overlong guard)
              0xF4,       cont in 0x80..0x8F, cont, cont (0xF4 <= U+10FFFF guard)
              0xF1..0xF3, cont, cont, cont -/
def utf8DecodeStep (bytes : List UInt8) : Option (Nat × List UInt8) :=
  match bytes with
  | [] => none
  | b0 :: rest =>
    if b0 ≤ 0x7F then
      some (b0.toNat, rest)
    else if 0xC2 ≤ b0 && b0 ≤ 0xDF then
      match rest with
      | b1 :: rest1 =>
        if isCont b1 then
          some ((b0.toNat % 0x20) * 0x40 + contBits b1, rest1)
        else none
      | [] => none
    else if 0xE0 ≤ b0 && b0 ≤ 0xEF then
      match rest with
      | b1 :: b2 :: rest2 =>
        -- first-continuation guard: 0xE0 -> 0xA0..0xBF, 0xED -> 0x80..0x9F
        let g1 : Bool :=
          if b0 = 0xE0 then 0xA0 ≤ b1 && b1 ≤ 0xBF
          else if b0 = 0xED then 0x80 ≤ b1 && b1 ≤ 0x9F
          else isCont b1
        if g1 && isCont b2 then
          some ((b0.toNat % 0x10) * 0x1000 + contBits b1 * 0x40 + contBits b2, rest2)
        else none
      | _ => none
    else if 0xF0 ≤ b0 && b0 ≤ 0xF4 then
      match rest with
      | b1 :: b2 :: b3 :: rest3 =>
        -- first-continuation guard: 0xF0 -> 0x90..0xBF, 0xF4 -> 0x80..0x8F
        let g1 : Bool :=
          if b0 = 0xF0 then 0x90 ≤ b1 && b1 ≤ 0xBF
          else if b0 = 0xF4 then 0x80 ≤ b1 && b1 ≤ 0x8F
          else isCont b1
        if g1 && isCont b2 && isCont b3 then
          some ((b0.toNat % 0x08) * 0x40000 + contBits b1 * 0x1000
                  + contBits b2 * 0x40 + contBits b3, rest3)
        else none
      | _ => none
    else
      none

/-- Validate a full byte string: fold `utf8DecodeStep` to the end. -/
def utf8Valid (bytes : List UInt8) : Bool :=
  match bytes with
  | [] => true
  | b :: bs =>
    match utf8DecodeStep (b :: bs) with
    | none => false
    | some (_, rest) =>
      if h : rest.length < (b :: bs).length then utf8Valid rest else false
termination_by bytes.length
decreasing_by exact h

/-! ### P8.3 — boundary rejection / acceptance on concrete vectors

These pin the decisive RFC 3629 boundaries and double as the bridge test
vectors for the C implementation. All by `decide` (closed computation). -/

-- The decode step is closed computation -> `decide`.
-- `utf8Valid` is well-founded recursive (does not reduce by `rfl`), so we
-- unfold it via `simp` (its equation lemma) down to `utf8DecodeStep` facts.

-- reject
theorem reject_overlong2 : utf8Valid [0xC0, 0x80] = false := by
  simp [utf8Valid, utf8DecodeStep]
theorem reject_overlong3 : utf8Valid [0xE0, 0x80, 0x80] = false := by
  simp [utf8Valid, utf8DecodeStep, isCont]
theorem reject_surrogate : utf8Valid [0xED, 0xA0, 0x80] = false := by
  simp [utf8Valid, utf8DecodeStep, isCont]
theorem reject_above_max : utf8Valid [0xF4, 0x90, 0x80, 0x80] = false := by
  simp [utf8Valid, utf8DecodeStep, isCont]

-- accept (with their code points)
theorem accept_dollar : utf8DecodeStep [0x24] = some (0x24, []) := by decide
theorem accept_cent   : utf8DecodeStep [0xC2, 0xA2] = some (0xA2, []) := by decide
theorem accept_euro   : utf8DecodeStep [0xE2, 0x82, 0xAC] = some (0x20AC, []) := by decide
theorem accept_clef   : utf8DecodeStep [0xF0, 0x9D, 0x84, 0x9E] = some (0x1D11E, []) := by decide

theorem accept_dollar_valid : utf8Valid [0x24] = true := by
  simp [utf8Valid, utf8DecodeStep]
theorem accept_cent_valid   : utf8Valid [0xC2, 0xA2] = true := by
  simp [utf8Valid, utf8DecodeStep, isCont]
theorem accept_euro_valid   : utf8Valid [0xE2, 0x82, 0xAC] = true := by
  simp [utf8Valid, utf8DecodeStep, isCont]
theorem accept_clef_valid   : utf8Valid [0xF0, 0x9D, 0x84, 0x9E] = true := by
  simp [utf8Valid, utf8DecodeStep, isCont]

/-! ### P8.1 — decidability / exhaustiveness

`utf8Valid` is `Bool`-valued, hence total and decidable by construction. We
state that a single decode step classifies every input as `none` or `some`
(exhaustive, no third outcome), which is what the C side must mirror. -/

theorem step_decidable (bytes : List UInt8) :
    utf8DecodeStep bytes = none ∨ ∃ r, utf8DecodeStep bytes = some r := by
  cases h : utf8DecodeStep bytes with
  | none => exact Or.inl rfl
  | some r => exact Or.inr ⟨r, rfl⟩

theorem valid_decidable (bytes : List UInt8) :
    utf8Valid bytes = true ∨ utf8Valid bytes = false := by
  cases utf8Valid bytes
  · exact Or.inr rfl
  · exact Or.inl rfl

/-! ### P8.2 — range soundness (the main result)

Any code point that `utf8DecodeStep` accepts is a valid Unicode scalar value:
in range `0..0x10FFFF` and not a UTF-16 surrogate (`0xD800..0xDFFF`). This is
the property that justifies the overlong/surrogate/upper-bound guards in the
decoder. RFC 3629 §3, RFC 6455 §8.1.

Proof: unfold one step, split into every lead-byte/guard branch, discharge the
impossible (`none`-producing) branches, then in each accepting branch convert
the `UInt8` byte-range guards to `Nat` facts and let `omega` bound the assembled
code point. The 0xE0/0xF0 overlong guards give the lower bounds, the 0xED
surrogate guard and 0xF4 guard give the surrogate/upper bounds; for the generic
3-byte branch the `b0 ≠ 0xED` fact (converted via `UInt8.toNat_inj`) is what
keeps the code point out of the surrogate window. -/
theorem utf8_range_sound (bytes : List UInt8) (cp : Nat) (rest : List UInt8)
    (h : utf8DecodeStep bytes = some (cp, rest)) :
    cp ≤ 0x10FFFF ∧ ¬(0xD800 ≤ cp ∧ cp ≤ 0xDFFF) := by
  unfold utf8DecodeStep at h
  repeat' split at h
  all_goals (try (simp at h))
  all_goals
    simp_all only [isCont, contBits, Bool.and_eq_true, decide_eq_true_eq,
      UInt8.le_iff_toNat_le, UInt8.toNat_ofNat, ← UInt8.toNat_inj]
  all_goals omega

/-- Corollary: accepted code points never fall in the surrogate range. -/
theorem utf8_no_surrogate (bytes : List UInt8) (cp : Nat) (rest : List UInt8)
    (h : utf8DecodeStep bytes = some (cp, rest)) :
    cp < 0xD800 ∨ 0xDFFF < cp := by
  have := (utf8_range_sound bytes cp rest h).2
  omega

/-- Corollary: accepted code points are within the Unicode upper bound. -/
theorem utf8_max (bytes : List UInt8) (cp : Nat) (rest : List UInt8)
    (h : utf8DecodeStep bytes = some (cp, rest)) :
    cp ≤ 0x10FFFF :=
  (utf8_range_sound bytes cp rest h).1

end WsProto
