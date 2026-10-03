# Failed Experiments

Trace of the encoding experiments that were tried and **reverted** because they did not beat the
baseline. Kept here so we don't re-litigate them.

**Baseline (the bar to beat):** per-endpoint color deltas (6 `le_model`s: one set of red/green/blue
per endpoint), raw top-table reference (1 `le_model`), index residual mask + per-byte difference
(2 `le_model`s). Measured on the `suite_image` set (74 images): **1.41:1** average compression
ratio (1.4102, byte-weighted 1.3688), 11/11 tests pass.

Splitting the 3 shared color models into one set per endpoint (6 models, +3 model headers in the
stream) beat the 3-model baseline: colors payload −0.41%, total stream −0.105%, average ratio
1.4087 → 1.4102. The endpoint-1 deltas have a different distribution (often wider), so a dedicated
Rice `k` and rank table per endpoint fits better. Kept.

## Summary

| # | Experiment | Result | Ratio | Outcome |
|---|-----------|--------|-------|---------|
| 1 | Average/spread color encoding (6 models) | round-trips, but slower | 1.34:1 (abs) / 1.32:1 (delta) | reverted |
| 2 | Reference zigzag-delta, static Rice | round-trips, but slower | 1.39:1 | reverted |
| 3 | Reference zigzag-delta, dynamic Rice | breaks round-trip | n/a | reverted |
| 4 | Hit-count-sorted top-table + reference delta | round-trips, `reference_bytes` drops but net slower | 1.39:1 | reverted |
| 5 | Reference-delta hypothesis, offline entropy study (exp. 2 revisited, no code change) | no near-0 peak, delta near-uniform | n/a (est. +3.0% on reference payload) | not implemented |
| 6 | Four difference models (per fixed byte / per ordinal of the set mask bits) | round-trips, payload −0.2% but 3 extra rank tables in the header | 1.4056:1 (fixed) / 1.4049:1 (ordinal) | reverted |

---

## 1. Average/spread color encoding

**Idea:** replace the 3 per-endpoint color models with 6 models per channel pair:
`red_avg`/`red_spread`, `green_avg`/`green_spread`, `blue_avg`/`blue_spread`.
- average = `(c0 + c1) / 2`, deltaed against the previous block's average (decorrelate red/blue against the halved green, mirroring the existing green decorrelation).
- spread = `c1 - c0`, deltaed against the previous block's spread.

**Result:** round-trip stays exact, but the ratio regressed.
- Spread as an **absolute** value: **1.34:1**
- Spread **deltaed** against the previous spread: **1.32:1** (marginally worse — the spread delta `Δ(c1−c0) = Δc1 − Δc0` is a *sum of two* independent endpoint deltas, ~2× the variance of the baseline's single endpoint delta, so it Rice-codes worse).

**Why it lost:** for the same 6 color symbols, per-endpoint deltas (each one delta, lower variance) are inherently more compact than one average + one spread (the spread is a second-order difference). You cannot get both the 1.41:1 ratio *and* the per-channel avg/spread split from a 6-symbol average/spread scheme.

**Kept:** the avg/spread split is a valid *derived* statistic — it can be computed from the per-endpoint models in `bc1_packed_get_stats` with zero stream cost. The encoding itself was reverted.

---

## 2. Reference zigzag-delta, static Rice

**Idea:** the top-table reference index is spatially coherent like colors, so encode a zigzag-8 delta
against the previous block's reference instead of the raw reference. Reference is 8-bit, so the
mod-256 zigzag delta fits the same 256-symbol static model (header layout unchanged). Trivial
`prev_reference` state, reset to 0 at the start of each strip (no inter-strip dependency).

**Result:** round-trips, but regressed to **1.39:1**. `reference_k` stuck at **6** on every image
(the same value the raw reference already got).

**Why it lost:** the reference is a **VQ code index** (the *nearest* centroid), not a smooth value.
A single flipped bit in a block's indices can flip *which* centroid is nearest, so the reference
index **jumps around** rather than staying flat. Its delta is not concentrated at 0, and a static
Rice model (one `k` per strip) picks the same `k` it already chose for the raw reference. No win.

---

## 3. Reference zigzag-delta, dynamic Rice

**Idea:** a dynamic (moving-alphabet) Rice model adapts `k` per symbol, which might adapt to the
delta better than a static model that must commit to one `k`.

**Result:** **broke the round-trip** (10/10 tests failed, indices corrupted).

**Why it lost:** the stream's model load path (`load_static_model`) always rebuilds a *static* model
on decode, but the encoder's *dynamic* model promotes its moving alphabet per symbol — the two
diverge and the decoded reference comes out wrong. Fixing it requires reworking the save/load path
for a component where the static version (exp. 2) already shows no benefit. Dead end without a much
larger change.

---

## 4. Hit-count-sorted top-table + reference delta

**Idea (yours):** the final table is emitted in *cluster* order by `vq_top_table`, not hit-count.
Sort the table most-used-first, then reuse the reference delta.
- **Confirmed:** reordering the *raw* reference is a wash — `le_static_model_init` builds its Rice
  alphabet sorted by *frequency*, so permuting the table only permutes identical histogram bins →
  identical bits. Only the *delta* can benefit.
- **Confirmed:** the sort *does* reduce `reference_bytes` (common centroids cluster at low, nearby
  indices → smoother index surface → sparser delta): `metal.png` 68967→68063, `brick2.png`
  68755→67594, `grass1.png` 69051→68645.

**Result:** round-trips, but still **1.39:1** — below the 1.41:1 baseline. `reference_k` still 6.

**Why it lost:** the reference is not spatially coherent (see exp. 2). Hit-count ordering smooths the
*frequency* layout, but the underlying *spatial* VQ discontinuities dominate, so the delta
distribution is still too broad for static Rice to beat the raw reference. No codebook ordering can
fix that.

---

## 5. Reference-delta hypothesis, offline entropy study

**Idea:** revisit of exp. 2's hypothesis, this time measured without touching the encoder:
"the reference is spatially coherent for the same reasons as colors — adjacent blocks usually
cluster on the same or a near centroid, so a zigzag-8 delta from the up-neighbor collapses the
rank histogram to a near-0 peak (reference_k 5–6 → 0–1, i.e. ~8 bits down to 2–4), with trivial
decoder state (last reference) and no inter-strip dependency."

**Method:** replicated the full pipeline (stb load → `STB_DXT_HIGHQUAL` → `build_top_table`)
on the 54-image / 2,834,432-block corpus, computed the per-block reference map, and measured
the entropy of three symbols: raw reference, zigzag-8 mod-256 delta against the scan-previous
reference (per-strip seed 0, encoder semantics), and zigzag-8 delta against the *spatially*
up-neighbor (the literal reading of the hypothesis).

**Result:**

| predictor | bits/block |
|---|---|
| raw reference (current) | 7.49 |
| scan-previous delta | 7.72 (+3.0% on the reference payload) |
| spatial-up delta | 7.92 |

- Only **7.1%** of deltas are zero (0.3–30% per image); **17.6%** have |Δ| > 127 and wrap in the
  mod-256 zigzag. The delta histogram is near-*uniform*, not a near-0 peak.
- The spatial-up variant is *worse* than the scan-previous one — the conclusion holds under
  either reading of "up-neighbor".
- Consistent with exp. 2's full-encode result (1.41:1 → 1.39:1): the reference is ~22% of the
  stream, so +3% on it is ≈ −0.6% overall.

**Why it lost:** the top table is ordered by *frequency rank*, not bitmap similarity. Adjacent
blocks do have similar index bitmaps — but that similarity is already captured by the
mask+difference pipeline. A 1–2 bit change in a bitmap moves the nearest centroid to a
*different rank*, and rank adjacency is uncorrelated with bitmap similarity, so the reference
*rank* sequence is near-uniform to begin with (7.49 of the 8-bit maximum: ≤0.5 bits of
redundancy) and differencing it only re-scatters it. Two additional constraints: `reference_k`
is the model *precision*, not the per-symbol cost (a near-uniform 256-symbol model costs
≈7.9 bits at any k, so "2–4 bits" was unreachable), and `LE_ALPHABET_SIZE = 256` caps the model
alphabet, so an exact 9-bit delta is impossible without modifying lite_encoding.

**Outcome:** not implemented; the raw reference stays.

---

## 6. Four difference models (one per non-zero byte)

**Idea:** replace the single `table_difference_model`, shared by all 4 bytes of the indices residual,
with 4 static models so each can specialize on a narrower byte distribution (and thus pick a better
`k`). Two variants were measured:
- **Fixed position:** model `j` encodes byte `j` of the indices (LSB→MSB).
- **Ordinal:** model `j` encodes the `(j+1)`-th *non-zero* byte — the first differing byte always
  uses model 0, the second model 1, etc. (the model index follows the set bits of the mask in
  LSB→MSB order, on both the histogram and the encode/decode side).

Both variants keep the same stream framing (the header grows from 6 to 9 static models) and
round-trip byte-exact, all 11 tests pass.

**Result:** loses on both ratio and speed (`suite_image`, 74 images).
- Fixed position: average ratio 1.4087 → 1.4056 (**−0.22%**); `difference_bytes` −0.27% (smaller on 74/74).
- Ordinal: average ratio 1.4087 → 1.4049 (**−0.27%**); `difference_bytes` −0.21%; **0/74 images improve**.
- Decompression (ground.png 1024², 18 threads): ≈2497 → ≈2447 MB/s (**≈−2%**).

**Why it lost:** the four bytes of the residual are near-identical in Rice terms. The single model
already picks `k=4` on 66/74 images, and the split models land on the same `k` for 59/74 (fixed
variant: bytes 0/3 are `k=4` on 69/74 each; only bytes 1/2 occasionally drop to `k=3`, one image
each to `k≤2`). Specialization therefore saves at most ≈0.2% of the `difference` payload, while the
stream header grows by 3 extra rank tables (`2 + num_symbols` bytes each, up to 258) — ≈+450 B per
image (ground.png: 397,536 → 397,988 B), which outweighs the gain. Small 512² images suffer most
(−0.5%): the header is a bigger share of their stream. Decompression loses ≈2% because the hot
decode loop now selects among 4 models with divergent `k` values, with no bitstream savings to pay
for it.

**Kept:** nothing. The baseline single `table_difference_model` stays.

---

## Open thread (not a failed experiment, just a lead)

The reference is the largest byte component (~22% of a typical image). Since it's a VQ index that is
not coherent, neither codebook reordering nor delta-coding moves it. The lever that *would* is a
**better VQ codebook** (more/smarter centroids, or spatially-aware quantization) so the index surface
is inherently smoother and the residual smaller. Larger change, separate investigation — not started.
