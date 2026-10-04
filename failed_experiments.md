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
| 7 | Cost-aware VQ entry selection + top-table ablation (plan stages 1+2) | round-trips, best +0.9% stream, Rice-k cliffs eat the gain | 1.4230:1 (best of 6 variants) | reverted, target ≥1.50 not reached |
| 8 | Byte-count-first top-table reference (`find_reference`: min differing bytes, popcount tie-break) | round-trips, difference −1.9% but mask +9.1% / reference +2.3% | 1.4055:1 (−0.33%) | reverted |
| 8b | Exp 8 re-run on top of per-strip mask k (the rice-k-cliff enabler) | round-trips, mask increase is *true distribution cost*, not a k cliff — still a loss | 1.4058:1 (−0.31%) | reverted (per-strip mask k kept, it is independently +0.03%) |

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

## 7. Cost-aware VQ entry selection + top-table ablation (plan stages 1+2)

**Idea:** the residual is the whale: difference ~21.2 bits/block, reference ~8.3, incompressible).
- **Stage 1 (selection):** after the SIMD `nearest32` scan, among the entries within 4 popcount of the nearest, pick the one minimizing the expected coded cost = mask bits + differing-byte bits under the static models; ties broken by fewest differing bytes, then cheapest reference, then lower index. The `init_static_models` pass uses a flat proxy (8 bits/byte, 1 bit per set mask bit) because the models do not exist yet, and the reference/mask/difference histograms get +1 Laplace smoothing so every symbol the compress pass can pick is present in the saved model (zero-count symbols are not stored in the stream).
- **Stage 2 (table):** ablate the `vq_top_table` refined (chimera) centroids against the exact top-256 patterns, plus a top-256-minus-singletons variant.

**Result:** 74-image suite, six completed variants, 11/11 round-trip on all of them:

| top table | entry selection | avg ratio |
|---|---|---|
| vq refined (current) | cost-aware + reference tie-break | 1.3999 |
| vq refined (current) | cost-aware | 1.3985 |
| vq refined (current) | cost-aware + two-pass init | **1.4230** |
| exact top-256 | cost-aware + reference tie-break | 1.3953 |
| exact top-256 | cost-aware | 1.3931 |
| exact top-256 | cost-aware + two-pass init | 1.4178 |

Baseline 1.4102. Target ≥ 1.50 (expected 1.52–1.57). Best combo = +0.9%. Per-image guard: 0 of 74 regressed > 1%.

**Why it lost:**
- The difference payload only dropped ~4% (metal.png: 165961 → 159559 B) vs the plan's expected 21.2 → 16–17 bits/block (−22%): the ±4-popcount window mostly re-orders same-byte-count candidates, and these textures rarely have a byte-sparse neighbor within 4 flips.
- **Rice-k cliff:** the selection shifts the emitted symbol distributions and flips the globally-minimized `k` (metal.png: reference_k 6→5, mask_k 1→2) → reference +11% (64006 → 71147 B) and mask +8% (23891 → 25800 B), eating the difference gain. A two-pass init (model fitted to the actually-emitted distribution) recovers most of it (1.4230), but the plan's ~10% expectation was built on a difference drop ~5× bigger than what the window can deliver.
- **Stage 2 has no lever:** the exact table covers more blocks exactly (zero-residual 4.1% → 6.3% suite-wide) but the refined centroids are closer on average for the rest (avg nearest popcount 5.36 → 5.59, difference bytes 86685 → 87851 with two-pass init) — the exact variant is slightly worse in every selection row (−0.3 to −0.5%). The refinement stays as-is, and the plan's "gated" variant is vacuous: a majority-vote centroid move always decreases total Hamming distance by construction.

**Open bug found (not fixed — work stopped here):** the top table can come out **empty**. `vq_top_table` drops clusters with sampled count ≤ 1; on small random images (512 blocks; reproduced on 1 of 20 splitmix32 seeds, e.g. seed `0x296969A4`) no cluster survives → `top_table_size = 0` → compress writes header byte `(uint8_t)(0−1) = 255` ("256 entries") but zero entries, corrupting the stream layout; `bc1_packed_decompress` then asserts (`stream.length > strip_offset`) in test builds and reads past the end of the stream with asserts off. The minus-singletons ablation triggers it deterministically (≥ 256 unique patterns → all top-256 are singletons → all dropped). Needs a 1-entry floor in `build_top_table` plus a regression test.

**Kept:** (1) the Rice-k cliff is a standing constraint on any future selection experiment: static Rice `k` is a global cliff, so any selection that skews the emitted reference/mask/byte distribution must re-fit the models to the emitted distribution (two-pass init) or accept the cliff; (2) refined centroids ≥ exact patterns, `vq_top_table` stays; (3) the empty-table bug above.

**Outcome:** reverted; the 1.4102 baseline stands. The plan's stages 1–2 are shelved, stage 3 (spatially adaptive codebook, format v2) remains gated pending approval. The incompressible reference (~16% of the 1024-class stream) is still the biggest untapped component, and a better (spatially aware) codebook remains the most promising lever.

---

## 8. Byte-count-first top-table reference

**Idea:** in the histogram and compress passes, replace the `nearest32` (min popcount) selection with
`find_reference`: traverse all top-table entries and pick the one with the fewest differing **bytes**
(popcount of the 4-bit mask), tie-broken by popcount, then by the larger entry value, then by the
lower index (nearest32's final tie-breaks). Rationale: the stream writes one full byte per
differing byte, so a low-popcount difference spread across 4 bytes (4 mask bits + 4 bytes) is
pricier than a higher popcount concentrated in 1 byte (1 mask bit + 1 byte). Structurally simpler
than exp 7: no model cost in the selection, no window, no two-pass init. The decoder is untouched
(it decodes the reference from the stream), and both encoder passes share one deterministic rule, so
the static models fit exactly the emitted distribution.

**Result:** 74-image suite, 11/11 round-trip, average ratio **1.4055:1** (baseline 1.4102, **−0.33%**).
52/74 images regressed > 0.05%, 15 improved, 7 flat. Worst: kodim10 −1.61%, woodplanks2 −1.38%,
kodim07 −1.33%. Best: wall3 +0.75%, rustywall2 +0.55%, grass1 +0.50%. Decoder throughput unchanged
(≈2760 → ≈2702 MB/s, run noise). Encoder suite wall time 8.1 → 10.1 s (scalar 256-entry scan;
immaterial).

Component payloads, whole suite (static header excluded):

| component | baseline | new | Δ | Rice k (mean) |
|---|---|---|---|---|
| difference | 6694405 B | 6565684 B | **−1.92%** | 3.88 → 4.81 |
| mask | 1124320 B | 1226779 B | **+9.11%** | 1.14 → 1.57 |
| reference | 2721337 B | 2783438 B | **+2.28%** | 5.65 → 5.80 |
| colors | 5888190 B | 5888190 B | +0.00% | untouched |

**What the hypothesis got right:** the difference component *did* drop, exactly as intended (fewer,
cheaper differing bytes; `difference_k` rose but the byte count fell more). On texture images
(grass1, ground, wall3, rustywall2) the difference drop (−2.2 to −3.8%) outweighed the other
increases and the ratio improved.

**Why it still lost:** the selection also moves the *other two* components, whose Rice cost is
**value-based**, not structure-based:
- **Mask +9.1%:** the mask symbol is Rice-coded by its *value* 0..15, not by its popcount. A
  1-byte difference in the MSB byte is mask 8 — structurally sparse but value-large. The shift to
  byte-sparse winners flattens the mask value distribution, so `mask_k` rose on most images
  (e.g. 1→2 on kodim10/09, wall2) and the whole component got pricier *despite* fewer bytes
  following the mask.
- **Reference +2.3%:** byte-sparse winners are systematically *rarer* table entries (the table is
  ordered by frequency rank, which is uncorrelated with byte sparsity), so emitted reference values
  shifted upward and `reference_k` rose 5→6 on 11 images. Near-uniform 8-bit symbols cost ≈1 bit
  more per raised `k` (cf. exp 5).
- On photograph-like images (kodim*, walls, wood) the mask + reference increases (≈165 KB
  suite-wide) outweigh the difference gain (≈129 KB) → 52/74 regress.

**Kept:** the standing lesson sharpens: a structural proxy for the per-block cost (popcount, byte
count) is only monotone with the *difference* component. Because the mask and reference costs are
value-based Rice, any selection rule that is not the actual model cost moves those two
distributions away from where the popcount rule left them. No free lunch from a single scalar key;
the model-cost-aware selection of exp 7 remains the only variant that was net-positive. (See 8b:
the per-strip mask k enabler did not change the verdict.)

**Outcome:** reverted; the 1.4102 baseline stood until the per-strip mask k of 8b was kept (1.4106).

---

## 8b. Exp 8 re-run with per-strip mask k

**Context:** between 8 and 8b the stream gained a kept change: a per-strip Rice `k` for the mask
model (the alphabet stays image-wide; the k is stored RLE'd against the image-level k, 1 B for a
stationary image; the strip stays 4-aligned). This kills the *k-cliff* failure mode for the mask:
each strip's k is the exact argmin of its own emitted mask distribution. Measured alone on the
popcount selection: **1.4102 → 1.4106 (+0.033%)**, mask payload −0.45%, 13 images improved, 0
regressed. Kept.

**Idea:** re-run exp 8 (byte-sparse `find_reference` selection) on top of that. Exp 8's mask loss
was +9.11%, partly attributed to the image-wide `mask_k` flipping 1→2; with per-strip k, only the
strips that genuinely need a different k should pay, so the hope was that the mask increase would
collapse and the net would flip positive (difference was already −1.92%).

**Result:** 74-image suite, 11/11 round-trip, average ratio **1.4058:1** (−0.31% vs 1.4102).
51/74 regressed > 0.05%, 15 improved, 8 flat — the same loss profile as exp 8. Components:

| component | exp 8 | 8b | Δ | k (mean, image-level) |
|---|---|---|---|---|
| difference | −1.92% | −1.92% | unchanged | 3.88 → 4.81 |
| mask | +9.11% | **+8.84%** | only −16 KB recovered | 1.14 → 1.57 |
| reference | +2.28% | +2.28% | unchanged (its k is still image-wide) | 5.65 → 5.80 |

**Why it still lost — the mask increase was never a k cliff:** per-strip k recovered only ~16 KB
of the ~102 KB mask increase. The Rice bit cost of a *mid-range* mask value is nearly k-invariant
(value 1: k=1 costs 3 bits, k=2 costs 3; value 8: k=1 costs 5, k=2 costs 4) — the 1→2 flip
roughly cancels itself. The +8.8% is **true distribution cost**: byte-sparse selection
systematically scatters the mask *value* distribution (a 1-byte difference in any of the 4 bytes
is one of values 1/2/4/8, a flatter mid-range shape than the popcount rule produced), and no k
choice, per image or per strip, prices that shape cheaper. The reference +2.3% (rarer entries →
larger values) is untouched by mask-only per-strip k, and extending per-strip k to the reference
could at best recover the small k-cliff slice of it — the arithmetic still nets a loss
(−129 KB difference gain vs ~+145 KB mask + reference) on any variant of this selection.

**Kept:** (1) the per-strip mask k (independently net-positive, and it remains the right shape for
any future per-strip model work); (2) the generalization of the exp 8 lesson: a selection rule
that reshapes a value-based Rice distribution pays for the *shape*, not just the scale — per-strip
k removes the scale cliff but cannot remove a flatter shape. The selection frontier (exp 7 cost-aware,
exp 8 byte-sparse, 8b) is now closed for structural keys: only the actual model cost is monotone
with the actual cost, and that variant (exp 7) was shelved for process reasons, not measured ones.

**Outcome:** the `find_reference` selection is reverted; the per-strip mask k stays. Current
baseline: **1.4106**.

---

## Open thread (not a failed experiment, just a lead)

The reference is the largest byte component (~22% of a typical image). Since it's a VQ index that is
not coherent, neither codebook reordering nor delta-coding moves it. The lever that *would* is a
**better VQ codebook** (more/smarter centroids, or spatially-aware quantization) so the index surface
is inherently smoother and the residual smaller. Larger change, separate investigation — not started.
