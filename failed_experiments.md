# Failed Experiments

Trace of the encoding experiments that were tried and **reverted** because they did not beat the
baseline. Kept here so we don't re-litigate them.

**Baseline (the bar to beat):** per-endpoint color deltas (3 `le_model`s: red/green/blue), raw
top-table reference (1 `le_model`), index residual mask + per-byte difference (2 `le_model`s).
Measured on the `suite_image` set (74 images): **1.41:1** average compression ratio, 10/10 tests pass.

## Summary

| # | Experiment | Result | Ratio | Outcome |
|---|-----------|--------|-------|---------|
| 1 | Average/spread color encoding (6 models) | round-trips, but slower | 1.34:1 (abs) / 1.32:1 (delta) | reverted |
| 2 | Reference zigzag-delta, static Rice | round-trips, but slower | 1.39:1 | reverted |
| 3 | Reference zigzag-delta, dynamic Rice | breaks round-trip | n/a | reverted |
| 4 | Hit-count-sorted top-table + reference delta | round-trips, `reference_bytes` drops but net slower | 1.39:1 | reverted |

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

## Open thread (not a failed experiment, just a lead)

The reference is the largest byte component (~22% of a typical image). Since it's a VQ index that is
not coherent, neither codebook reordering nor delta-coding moves it. The lever that *would* is a
**better VQ codebook** (more/smarter centroids, or spatially-aware quantization) so the index surface
is inherently smoother and the residual smaller. Larger change, separate investigation — not started.
