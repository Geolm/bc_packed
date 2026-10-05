# Failed Experiments

Trace of the encoding experiments that were tried and **reverted** because they did not beat the
baseline. Kept here so we don't re-litigate them.

**Baseline (the bar to beat):** per-endpoint color deltas (6 `le_model`s: one set of red/green/blue
per endpoint), per-strip color predictor selection (exp 11, 8 header bytes), raw top-table reference
(1 `le_model`), index residual mask + per-byte difference (2 `le_model`s). Measured on the
`suite_image` set (74 images): **1.43:1** average compression ratio (1.4323, byte-weighted 1.3822),
13/13 tests pass. (Before exp 11 the baseline was 1.4102 / 1.3688.)

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
| 8b | Exp 8 re-run on top of per-strip mask k (the rice-k-cliff enabler) | round-trips, mask increase is *true distribution cost*, not a k cliff — still a loss | 1.4058:1 (−0.31%) | reverted (per-strip mask k not committed, the +0.03% did not justify the code) |
| 9 | Top-block color prediction (vertical predictor after the first scanline) | round-trips, `colors` +5.4% (zigzag-previous is the better predictor for 49/74) | 1.3933:1 (−1.19%) | reverted (format-compatible; kept as a speed lever, see open thread) |
| 10 | Cascading masked top-table, VQ removed (exact ≥ n → 1-byte → 2-byte rounds, mode-resolved) | round-trips; n 2..8 byte-identical; difference +2.15% | 1.4025:1 (−0.54%) | reverted (1-entry floor + regression test kept — fixes the open bug) |
| 11 | Per-strip color predictor: row-above (same zigzag position) vs zigzag-previous, 1 bit/strip in the header, decided by delta energy | round-trips; colors −2.72%; 58/74 improved, worst −0.05% | **1.4323:1** (+1.57%) | **kept — this is the new baseline** |
| 12 | YCoCg-R color encoding (color-space / delta-space / 565-upscaled), measured offline (exp 5 style) | round-trips exact; loses on 71-74/74 images | 1.5298 / 1.5421 / 1.5449 vs 1.5784 baseline | not adopted — the current ΔG/2 decorrelation *is* YCoCg-R depth-matched for 565 |
| 13 | Spatially-smooth VQ codebook + zigzag reference delta (the open-thread "better codebook") | offline study; reference +0.33 bits/block, 85.7% of index transitions are genuinely-different centroids | 1.43 → 1.42 (−0.7%), 73/74 images regress | not adopted — the index surface is inherently wide; lever closed |

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

**Open bug found (fixed in the exp 10 cleanup — see below):** the top table can come out **empty**. `vq_top_table` drops clusters with sampled count ≤ 1; on small random images (512 blocks; reproduced on 1 of 20 splitmix32 seeds, e.g. seed `0x296969A4`) no cluster survives → `top_table_size = 0` → compress writes header byte `(uint8_t)(0−1) = 255` ("256 entries") but zero entries, corrupting the stream layout; `bc1_packed_decompress` then asserts (`stream.length > strip_offset`) in test builds and reads past the end of the stream with asserts off. The minus-singletons ablation triggers it deterministically (≥ 256 unique patterns → all top-256 are singletons → all dropped). Fixed: a 1-entry floor in `build_top_table` plus the `top_table_empty_floor` regression test (kept from exp 10).

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

**Context:** between 8 and 8b the stream gained a change, later **not committed** (the +0.03% gain did not justify the code): a per-strip Rice `k` for the mask
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

**Kept:** (1) the per-strip mask k was measured net-positive (+0.033%) but was **not committed** — the gain
did not justify the added stream format + decoder state; (2) the generalization of the exp 8 lesson: a selection rule
that reshapes a value-based Rice distribution pays for the *shape*, not just the scale — per-strip
k removes the scale cliff but cannot remove a flatter shape. The selection frontier (exp 7 cost-aware,
exp 8 byte-sparse, 8b) is now closed for structural keys: only the actual model cost is monotone
with the actual cost, and that variant (exp 7) was shelved for process reasons, not measured ones.

**Outcome:** the `find_reference` selection is reverted; the per-strip mask k was not committed. Current
baseline: **1.4102**.

---

## 9. Top-block color prediction (vertical predictor after the first scanline)

**Idea:** after the first scanline of each strip, predict each block's endpoint colors from the
spatial block *above* (`y−1`, same `x`) instead of the zigzag-previous block. The first scanline
keeps the current behavior (zigzag-previous, strip seed at the start). The row above is already
decoded when a row is processed (rows go top to bottom within a strip), so the change is a
3-site one-liner (`(y == start_y) ? &previous : block_above` in the histogram, compress, and
decompress passes): no stream-format change, no header change, no inter-strip dependency.

**Motivation:** with the zigzag-previous predictor, each block depends on the *preceding* block —
a serial chain of `width_blocks × strip_width` per strip. With the top-block predictor every block
depends only on the row above, so a whole row decodes in parallel: the per-strip serial depth
falls from `width×rows` to `rows` (1024 → 4 for a 1024² strip, 256× shallower), a structural win
for the GPU compute-shader decompression.

**Result:** 74-image suite, 11/11 round-trip, average ratio **1.3933:1** (baseline 1.4102,
**−1.19%** simple; byte-weighted 1.3688 → 1.3431, **−1.88%**). 49/74 images regressed, 25 improved.
The `colors` payload is the whole story: **5,888,190 → 6,203,938 B (+5.36%)**; reference/mask/
difference are untouched. Mean color-model k: green 2.15→2.35 / 2.34→2.53 (slightly wider
delta shape), red/blue flat.

By image class (ratio, colors_bytes):

| class | n | ratio | Δ | colors Δ |
|---|---|---|---|---|
| Wood scans | 20 | 1.4846 → 1.5146 | **+2.0%** | −4.2% |
| kodim photos | 24 | 1.4191 → 1.3796 | **−2.8%** | +8.8% |
| other textures | 30 | 1.3534 → 1.3234 | **−2.2%** | +6.0% |

Decompression (ground.png 1024², 18 threads, means of repeated runs): ≈2733 → ≈2765 MiB/s
(**+1.2%** — the multithreaded CPU path mostly hides the shorter serial chain; the structural
win is the 256× shallower per-strip dependency, which matters for the GPU shader).

**Why it lost:** for this suite the horizontal-neighbor delta is the better color predictor on
average. Adjacent 4×4 blocks share more of their 565 endpoint color along the row than along the
column (kodim photos, walls, roofs: +6–9% colors payload), and only textures whose features run
*vertically* (the Wood scans) benefit enough to pay it back (−4.2% colors). Colors is ≈22–33% of
a stream, so a +5.4% payload increase is a net −1.2% ratio — the same distribution-cost logic as
exp 1: changing the predictor reshapes what the Rice models carry, and here the shape got wider.

**Kept:** nothing in the stream. The change was fully format-compatible and is preserved as a
patch (`plans/exp9_topblock_predictor.patch`) for re-application if decompression throughput
becomes the objective (known cost: −1.2% size). See also the open thread on the per-row variant.

**Correction (2026-10-04, found while implementing exp 11):** the "spatial block above (y−1, same x)"
in this experiment was actually the block of the row above at the **same zigzag position** — visually,
the horizontally *mirrored* column of the previous row — not the directly-above block. The patch was
never committed (the `plans/` folder was cleaned up), so this was proven by measurement instead: with
the documented class partition (20 `Wood_0x` scans) the same-zigzag-position reference reproduces the
documented **−4.2%** wood class delta exactly (−4.21%), and the class baselines (1.4846 / 1.4191 /
1.3534 / 1.4102) and the 5,888,190-byte colors payload all match the current tree. The truly
spatial directly-above reference is *far* worse on this suite (all-top: kodim +45.7%, wood +7.6%,
+20.7% overall) — it is the zigzag-column reference that is cheap on the plank scans, because the
grain follows the zigzag columns. Consequence: exp 9's "−1.2% size" price tag applies to the
zigzag-column reference, not to the spatially-vertical one.

**Outcome:** reverted as an *all-or-nothing* predictor; its reference (same zigzag position) is the
winner of the per-strip decision of exp 11, which is kept.

---

## 10. Cascading masked top-table (VQ pass removed)

**Idea (yours):** drop the `vq_top_table` refinement pass (it doesn't bring anything) and instead fill the
top table with a cascading series of hashmap rounds, each keeping only patterns used at least n times
(n an encoder-only knob, tested 2..8):
1. **Exact round:** hash all block indices, keep the most frequent full patterns with count ≥ n (top 256 by count).
2. **If the table is not full** (likely for sparse textures): 4 rounds with one byte masked off of the
   hash key (`0x00FFFFFF`, `0xFF00FFFF`, `0xFFFF00FF`, `0xFFFFFF00`), same threshold, most frequent first.
3. **If still not full:** 6 rounds with two bytes masked off (all byte-pair combinations, `0x00FF00FF`-style).
   The cascade stops as soon as the table is full; a table with fewer than 256 entries is valid.
Each masked pattern is resolved to its **most frequent concrete index** (per-pattern mode, via a second
rehash of the same hashmap: the mode's blocks get a zero residual instead of one differing byte; tie-break
by the higher value, same convention as `nearest32`), and candidates are deduped against the current
table. Rounds are factored into one `collect_top_table_round` function. A **1-entry floor** (the single
most frequent exact pattern) guards the documented empty-table corruption. Histogram and decompression
passes untouched; stream format and `bc1_packed_maxsize` unchanged.

**n sweep (2..8):** all values produced **byte-identical streams** on the 74-image suite (the pattern
histograms are heavy-tailed enough that the top-256 selection is stable for any threshold below the
256th pattern's count) — n has no measurable effect on this suite, only small/sparse images would feel it.
Default shipped at n = 2 (closest to the old "top 256 whatever the count" behavior).

**Result:** 74-image suite, 13/13 round-trip, average ratio **1.4025:1** (baseline 1.4102, **−0.54%**).
70/74 images regressed, 4 improved (Wood_03 +4.95%, Wood_04 +4.21%, sky +0.07%, kodim07 +0.05%), 0 flat.
Worst: wall2 −4.56%, bigsand −2.94%, Wood_15 −2.86%, Wood_14 −2.85%, roof_tiles −2.68%.

Component payloads, whole suite (static header excluded):

| component | baseline | new | Δ | Rice k (mean) |
|---|---|---|---|---|
| colors | 5888190 B | 5888190 B | +0.00% | untouched |
| reference | 2721337 B | 2703547 B | −0.65% | 5.65 → 5.62 |
| mask | 1124320 B | 1100670 B | −2.10% | 1.14 → 1.04 |
| difference | 6694405 B | 6838463 B | **+2.15%** | 3.88 → 3.96 |

Top table: mean size 226 → 255 (VQ used to prune clusters; the cascade fills to 256 almost always).
Ground.png stream: 397616 → 399092 B. Decoder throughput ≈2767 → ≈2650 MiB/s (run noise; the decode
path is untouched). Encoder: slower, as expected (up to 11 masked hashmap passes + 10 mode-resolution
passes over the blocks, vs the VQ's 4 jittered-sampled nearest32 rounds).

**Why it lost:** the same mechanism exp 7's stage-2 ablation measured: the VQ majority-vote centroids are
*strictly closer* than the exact patterns to the blocks that don't match a pattern exactly (average
nearest popcount 5.36 refined vs 5.59 exact). The cascade's entries are real, existing patterns: the
blocks that equal one get a zero residual, but that population is already well served by the current
top-256, and the *residual* population — the majority of blocks, `difference` ≈ 40% of the payload — is
systematically farther from a real pattern than from a bit-majority centroid. The +2.15% difference
increase (≈ +144 KB suite-wide) outweighs the mask −2.10% (−24 KB) and reference −0.65% (−18 KB)
improvements. The coarse-fill slots (mean table 226 → 255) also replace VQ's chimeras, which were the
closer match for exactly those blocks. One implementation note: the survivors bubble (carried over from
the old top-table code) packs entries at the *back* of the array; the first implementation read them from
the front, so every cascade round silently produced 0 survivors and the table collapsed to the floor —
caught by the `top_table_masked_patterns` regression test, fixed with a full array reversal.

**Kept:** the 1-entry floor + the `top_table_empty_floor` regression test — this is the fix for the open
empty-table bug from exp 7 (independent of the cascade), re-applied to the reverted `build_top_table`.
The `top_table_masked_patterns` test went with the cascade.

**Outcome:** reverted; the 1.4102 baseline (VQ top table + 1-entry floor) stands. The 3-byte-off round
was never shipped (left out on purpose, per the doubt raised before implementing). The open "better
codebook" thread stands unchanged: this experiment rules out *coarser exact patterns* as the lever; the
measured lever remains smarter (spatially aware) centroids.

---

## 11. Per-strip color predictor (row-above at the same zigzag position)

**Idea (yours, the per-strip refinement of exp 9 / the open thread):** in the pre-histogram pass, per
strip, decide which color predictor is cheaper — zigzag-previous (the baseline) or the block of the row
above at the **same zigzag position** after the first scanline (the reference exp 9 actually used, see
its correction note) — and store **one bit per strip** (8 bytes, little-endian, in the header after the
strip seed). Histogram, compress, and decompress all use the chosen predictor per strip. The first
scanline of every strip is always predicted from the strip seed via the zigzag chain (identical for both
predictors, it cancels in the comparison).

**Decision rule:** total absolute magnitude of the six color deltas each predictor would emit for the
strip (decorrelated green/red/blue, both endpoints) — the `color_delta_energy` helper. A branchless
`abs32` here shipped with a signed/unsigned shift bug (`u >> 31` on a `uint32_t` is a *logical* shift;
the trick needs the all-ones mask, so it was rewritten with an arithmetic shift) that wrapped every
negative delta to ~2³² and made the decision garbage (measured 1.34:1, k-cliffs everywhere) before the
fix. With correct magnitudes the rule is a good proxy for the exact Rice cost: the fixed-point optimum
(per-strip argmin under the fitted mixture model, iterated) lands within 0.01% of the energy rule on the
suite, so the cheap rule is what ships.

**Result:** 74-image suite, 13/13 round-trip, average ratio **1.4323:1** (baseline 1.4102, **+1.57%**;
byte-weighted 1.3688 → 1.3822, total stream bytes −0.96%). Colors payload **5,888,190 → 5,728,137 B
(−2.72%)**; reference/mask/difference untouched. Per image: **58 improved, 15 flat, 1 regressed**
(worst: kodim14 −0.052% — the 8-byte header overhead on a stream where no strip switches; best:
Wood_05 +6.93%). Wood scans +2.7…+6.9% each, kodim photos mostly +0.1…+2%, roof_tiles +4.6%, metal
+1.5%, wall2 +2.3%. Decompression throughput unchanged (≈2730 → ≈2729 MiB/s, run noise); encoder
gains one extra pre-pass over the blocks.

**Why it won where exp 9 lost:** exp 9 applied the zigzag-column reference to *every* strip; here only
the strips where it is actually cheaper pay for it. The gain comes almost entirely from the plank/wood
textures, whose grain follows the zigzag columns (the same-zigzag-position row-above is an exact or near
exact match there); photo-like images keep zigzag-previous on (nearly) every strip, so their distribution
— and the fitted Rice `k` — barely moves (no exp-8b-style shape cost). A model fitted to the mixture
still fits the *emitted* distribution exactly, the standing lesson of exp 7/8b, so there is no k cliff.

**Also measured, same machinery:** the truly spatial *directly-above* reference (parity-remapped) has
≈zero per-strip value on this suite — its per-strip best-of under the mixture model is ratio-neutral
(wood +7.6% all-top; the decision only ever flips a handful of strips, e.g. wall2 −0.18%). The
zigzag-column reference is the one that carries the gain.

**Kept:** this is the **new baseline** (1.4323). Stream format: 8 extra header bytes after the strip
seed, nothing else. Decoder: flagged strips decode row-parallel (no serial zigzag chain) — the exp 9
GPU motivation, now paid for only on the strips that benefit. Regression test: `strip_predictor`
(two synthetic patterns that force the all-ones and the all-zeros flag words, byte-exact round-trip).

---

## 12. YCoCg-R color encoding

**Idea (yours):** encode the endpoint colors in reversible YCoCg-R —
`Co = R−B; tmp = B + Co/2; Cg = G − tmp; Y = tmp + Cg/2` (inverse: `tmp = Y − Cg/2; B = tmp − Co/2;`
`G = Cg + tmp; R = B + Co`) — and delta the YCoCg components against the reference block, since the
transform "already" decorrelates green from red/blue, making the current explicit decorrelation
(`ΔR − ΔG/2`, `ΔB − ΔG/2`) redundant.

**Why the premise is wrong for 565:** YCoCg-R is the decorrelated basis for *equal-depth* channels.
In 565, green has twice the precision of red/blue, so a pure luminance step is
`(ΔR, ΔG, ΔB) = (x, 2x, x)`, and:
- current scheme: `(ΔG, ΔR − ΔG/2, ΔB − ΔG/2) = (2x, 0, 0)` — luminance in **one** 6-bit component
- YCoCg-R: `(Y, Co, Cg) = (1.5x, 0, x)` — luminance in **two** components: `Cg = ΔG − (ΔR+ΔB)/2 =
  2x − x = x ≠ 0`, and `Y` carries 1.5x on top. The "green↔blue/red" correlation is only removed in
  the 8-8-8 sense; on the 565 grid the 5-bit channels move at half green's rate, so `G − (R+B)/2` does
  **not** collapse under a brightness change. The current ΔG/2 decorrelation is precisely YCoCg-R with
  the one depth correction the 565 grid needs; standard YCoCg-R undoes it.

**Method:** offline cost study (exp 5 style, no code change): real stb_image → stb_dxt(HIGHQUAL) →
packed BC1 on the 74-image suite, real `le_static_model_init` models, exact Rice bit cost per symbol,
4-byte strip alignment and the full header mirrored. A replica of the shipped scheme (v0) reproduced
the real `bc1_packed_compress` stream **byte-exact on 74/74 images** (200k fuzz round-trips of both
transforms, 0 decode mismatches, 0 out-of-alphabet symbols). Three variants measured, each with its
best-case per-strip predictor flag:

| variant | symbols | avg ratio | wins/74 | worst | colors payload |
|---|---|---|---|---|---|
| baseline (current) | (ΔG, ΔR−ΔG/2, ΔB−ΔG/2), 7-bit | **1.5784** / 1.5201 bw | — | — | 4182 KB |
| ycocg_abs (the literal proposal) | (Y, Co, Cg) of the color, deltaed, 8/7/8-bit | 1.5298 / 1.4692 bw (−3.17%) | **0** | −7.95% (drygrass) | +12.0% |
| ycocg_dlt | YCoCg of the delta, 8/7/8-bit | 1.5421 / 1.4806 bw (−2.39%) | 3 (≤ +0.40%) | −7.45% | +9.3% |
| ycocg_565 (depth-correct: upscale R,B to the 6-bit grid first) | (Y, Co, Cg), 8/8/8-bit | 1.5449 / 1.4869 bw (−2.15%) | **0** | −4.84% | +7.8% |

(baseline measured on the current code: 1.5784 avg / 1.5201 byte-weighted; the 1.4323 logged at the
top of this file predates later code/suite changes — the A/B comparison is same-code, same-suite.)

**Why it lost:** the luminance leak above. Two of the three YCoCg components carry the same luminance
signal, and the independent 1-D static Rice models cannot exploit their correlation — luminance-
dominant content (i.e. all of it) pays for it, worst on high-frequency grass (−8%). Even the
depth-correct 565-upscaled form — which for a luminance step emits exactly the same `(2x, 0, 0)`
structure as the current scheme — still loses on 74/74: for chroma it is a rotation of the same
information to a *wider* integer scale (`Co = 2(ΔR−ΔB)` is 2× wider than the current chroma symbols,
and the 8-bit symbols widen the rank tables). The 3 ycocg_dlt wins (Wood_03, wall3, rustywall3, all
≤ +0.40%) are within the predictor-flag re-selection noise. Note also that adopting any variant is a
stream format change (7→8-bit color symbols, 128→256 rank alphabets) requiring a GPU decoder update —
a cost the loss already rules out.

**Outcome:** not adopted. The current `(ΔG, ΔR − ΔG/2, ΔB − ΔG/2)` basis stands: it is YCoCg-R done
for 565, and its equal-depth cousin loses by 2-3% on this suite. Do not re-litigate.

---

## 13. Spatially-smooth VQ codebook + zigzag reference delta (the open thread)

**Idea (the open thread, now measured):** the reference is the largest byte component and stays
near-8-bit incompressible because it is a VQ code index that is not spatially coherent, and neither
codebook reordering nor delta-coding moves it (exp. 2/4/5). The lever that *would* is a **better VQ
codebook** — more/smarter centroids, or spatially-aware quantization — so the index surface is
inherently smoother and the residual smaller. Two halves:

1. **Spatially-aware codebook order:** search the permutation of the 256 table slots that minimizes
   the expected zigzag adjacency of the reference bitmaps — greedy construction plus steepest-ascent
   swap local search on Σ W·|posdiff| (W = joint block-pair frequency) — then zigzag-delta the
   reference under that order. This is the *maximum* smoothness any ordering of the current centroids
   can buy: if best-order + delta does not beat raw, no ordering can.
2. **Smarter centroids (the residual half):** covered by the already-measured exp. 7 (stage 2) and
   10 ablations of the `vq_top_table` refinement (see below).

**Method:** faithful offline cost study (exp. 5/12 style, no code change). The scratch tool
`#include "bc1_packed.c"` and mirrors the unit-test pipeline exactly (stb_image → stb_dxt
HIGHQUAL → `extract_4x4_rgba_block` at pixel coords `x*4, y*4` → `build_top_table` → `nearest32`
selection), then prices the reference component with the real `le_static_model_init` models. The
replica reproduces the unit test byte-exact (metal.png 1.405311, stream 373,076 B, table 256) and
its simple-average baseline 1.4341 matches the current suite stats (the 1.4323 logged at the top of
this file predates the later suite changes, cf. exp. 12's note) before any number is trusted.

**Result:** 74-image suite. The reference under candidate codebooks/orders (bits/block):

| reference encoding | bits/block |
|---|---|
| raw (current) | 7.778 |
| zigzag-delta, no reorder | 8.353 |
| zigzag-delta + best spatial reorder | 8.107 |

**Transition census** of the reference map over all adjacent zigzag block pairs: **85.7%** of
transitions are between *genuinely different* centroids (> 8-bit Hamming in the 32-bit index);
only **2.4%** are the same centroid and **2.8%** are brittle flips (≤ 4 bits).

Per-image new ratio (delta + best reorder replaces the raw reference; all other components
identical): **1 improved** (Wood_16 +0.24%), **73 regressed** (all > 0.5%; worst Wood_03
−2.88%). Simple average **1.4341 → 1.4239 (−0.72%)**, byte-weighted 1.3830 → 1.3732. Suite-wide
reference payload 2,755,618 → 2,872,383 B (**+4.24%** on the 16.8%-of-stream component ≈ −0.7%
overall).

**Why it lost:** the index surface is **inherently wide**, so the lever's premise fails at its root.
Adjacent DXT 4×4 blocks mostly have genuinely different index bitmaps — 85.7% of neighbor pairs jump
between centroids that differ in more than 8 of 16 texel indices. The incoherence exp. 2/4/5 measured
is **content, not codebook**: no permutation of the table can co-locate centroids that the content
genuinely jumps between, so even the *best* reorder + delta (8.107) sits +0.33 bits/block **above**
raw (7.778) — the same sign and magnitude class as exp. 2's un-reordered delta, and the 2.8% brittle
flips are the only part a smarter codebook could smooth (≈3% of the surface). The smarter-centroid
half is likewise closed by the measurements exp. 13 was supposed to refine: exp. 7 (stage 2) and
10 showed the VQ majority-vote centroids are *strictly closer* than the exact patterns to the
non-exact blocks (avg nearest popcount 5.36 refined vs 5.59 exact) — a bitwise majority vote over
the sample *is* the Hamming-optimal centroid for each cluster, so "smarter centroids" cannot beat
it, and the residual is at the Hamming limit of a 256-entry table for this content.

The one untested variant is a **spatially adaptive codebook** (multiple per-region tables, format
v2): it would let *different* regions use different centroid sets, but the same 85.7% wall applies
*within* each region's surface, and it adds per-block table-ID overhead plus the GPU decoder rework
exp. 12 flagged for any format change. Not pursued.

**Caution (measurement):** the first cut of this study ran on a 4× horizontally *shifted* RGBA
extraction (the scratch tool passed the block's column `x` where the test passes the pixel coord
`x*4`), which scrambled the generated BC1 and produced a phantom 1.5784 "baseline" that looked like
a win. The faithful replica (pixel coords, the test's flat-buffer layout) reproduces the unit test
exactly. Any future study of this pipeline must mirror the test's pixel extraction or it measures a
different encoder.

**Kept:** the closed verdict on the open thread. Both halves of "better VQ codebook" are measured
dead ends for this content class: reordering + delta cannot harvest smoothness the content does not
have (85.7% real transitions), and the majority-vote centroid is already the Hamming-optimal
256-entry codebook (exp. 7/10). The reference component is incompressible as a VQ index; the only
remaining structural lever is the format-v2 per-region table, gated behind the same wall.

**Outcome:** not adopted; no code change. The repo stays at the 1.43 baseline (13/13 tests pass).
This resolves the first lead of the open thread below.

---

## Open thread (not a failed experiment, just a lead)

The better-VQ-codebook lead is **resolved by exp. 13 and closed**: the reference surface is 85.7%
genuinely-different-centroid transitions, so neither a spatially-smooth codebook order + delta nor
smarter centroids can move it, and the format-v2 per-region table is gated behind the same wall. Do
not re-litigate.

A second lead from exp 9: a **per-row color predictor**. For each scanline, emit one flag bit and
predict from whichever of row-above / zigzag-previous was cheaper for that row (decided in the
histogram pass by delta energy, so the static models fit the emitted distribution exactly). The
flag costs 1 bit/row (≈32 B for a 1024² image) and could subdivide the per-strip choice of exp 11
(a strip is only height/64 rows: 4 rows for a 1024² texture, so per-row can split a strip exp 11
assigned to a single predictor). Caveat: a row flagged zigzag is a serial chain again, so the GPU
row-parallel win only applies to the rows flagged row-above. (Exp 11 implemented the per-*strip*
coarsening of this lead and kept it.)
