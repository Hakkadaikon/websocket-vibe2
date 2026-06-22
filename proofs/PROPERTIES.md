# Lean 形式検証 — 性質カタログ

役割分担(疎結合): 設計の状態機械は TLA+(spec/WsLifecycle.tla)で網羅検査済み。
Lean はそこに届かない **バイト列・ビット演算レベルの数学的性質** と **decode/encode の
決定性・網羅性・往復** を担当する。両者を機械変換で結線しない(YAGNI)。証明済み性質は
そのまま TDD のテストリスト述語になる。

| ID  | 性質 | 出典(RFC 6455) | 型 | 優先度 |
|-----|------|------|----|--------|
| P1  | マスキング involution: `unmask(mask(p,k),k) = p`、かつ mask=unmask(同一演算) | 5.3 | involution | 高(セキュリティ核) |
| P2  | マスキングは長さ保存: `length(mask(p,k)) = length(p)` | 5.3 | invariant | 高 |
| P3  | ペイロード長エンコード往復: `decodeLen(encodeLen(n)) = n`(全 n) | 5.2 | roundtrip | 高 |
| P4  | 長さエンコードの最小性/網羅性: n<126→1byte, 126≤n≤0xFFFF→2byte, それ超→8byte。境界が決定的に分類される | 5.2 | 網羅性/MUST | 高 |
| P5  | opcode 分類の網羅性・排他性: 全 4bit 値が data/control/reserved のどれか一つに決定的に分類 | 5.2/11.8 | 網羅性 | 中 |
| P6  | control フレーム制約: opcode が control なら payload長 ≤ 125 を要求する述語が、>125 を必ず reject | 5.5 | MUST NOT | 高 |
| P7  | close コード分類: 1005/1006/1015 は wire 禁止、1000-1011 と 3000-4999 のみ送出可、を決定的に判定 | 7.4.1 | MUST NOT | 中 |
| P8  | UTF-8 検証の健全性: validator が accept する列は妥当な UTF-8(過長・サロゲート・範囲外を reject)。全バイト列が accept/reject に決定的二分(網羅性) | 8.1 | 健全性/網羅性 | 高 |

## P1-P2 マスキング(最重要)
RFC 5.3: `transformed[i] = original[i] XOR key[i mod 4]`。XOR は自己逆元 → mask は involution。
これが破れるとデータ破壊。Lean で `mask k p` を定義し `mask k (mask k p) = p` を証明。

## P3-P4 長さエンコード
RFC 5.2: 7bit が 0-125 なら即値、126 なら続く 16bit、127 なら続く 64bit(MSB=0)。
`encodeLen : Nat → EncodedLen`、`decodeLen : EncodedLen → Nat`。roundtrip と境界分類。

## P5-P7 opcode / close コード分類
全値域での網羅性・排他性。`classifyOpcode : Fin 16 → OpClass` が total かつ各値一意。
P6: control なら ≤125。P7: close コードの送出可否述語。

証明都合の等価変形: `closeCodeSendable` はスケルトンが `||`(Bool)と `∧`(Prop)を混在させ型不整合だったため、`decide (...)` を使う純 Bool 述語へ書き直した(意図不変: 1000-1011 から {1004,1005,1006} を除外、1015 は元々 1000-1011 外なので除外済み、加えて 3000-4999)。

## P8 UTF-8(重い・後回し可)
RFC 8.1 + RFC 3629: text フレーム/close reason は妥当な UTF-8 MUST。
validator の健全性(accept⇒妥当)と決定性(全列が二分)を証明。重ければ核の補題だけ。

## 橋渡し(test-first)
各 Pn → C 実装の対応分岐を1対1でテスト化し、先に落としてから実装をモデルに合わせる。
特に P1(mask)・P4(長さ境界 126/127)・P6(control≤125)は実装で外しやすい所。
