# Plan: per-image tile-layout search + per-tile local reference alphabet

Status: proposal, no code written yet.
Scope: BC1 reference/tile machinery; tile-layout search also applies to BC4 / BC5 / BC3.
Baseline: 74-image BC1 suite **1.4907** (the byte-weighted ratio is lower; see
[failed_experiments.md](../failed_experiments.md)), BC4 **1.1352**, BC5 **1.0985**, BC3 **3.9180**
([stats_2026-11-08_18_04.csv](../logs/stats_2026-11-08_18_04.csv)).

This merges the two ideas from the review:

1. **Per-image tile-layout search** (exp 22 showed 8 x 8 is +1.6% on average, but the per-image
   spread is +9% to -2.9%; choosing the layout per image should beat both).
2. **Per-tile local reference alphabet** (the reference is ~17% of the stream and its *global*
   marginal is near-uniform at ~7.49 bits; the per-tile conditional has never been measured.
   This is not exp 13/16: it does not reorder/delta the global sequence and does not add
   centroids; it changes the per-tile alphabet of the reference code).

Both are decode-cheap: the hot per-block loop is unchanged. The layout is read once per tile,
the local reference model is built once per tile, and the block payload uses the same
`le_encode_symbol` / `le_decode_symbol` path as today.

---

## 1. Objective and success criteria

**Primary objective:** raise the BC1 suite average with **zero meaningful decompression slowdown**
(CPU and GPU within run noise), and no image regressing (other than the tiny header cost).

**Targets:**

- Combined feature: BC1 suite average **>= +1.0%** (1.4907 -> >= 1.5056), byte-weighted and total
  stream bytes also down.
- Layout search alone: >= +0.4% (it is the low-risk half).
- Local reference alphabet alone: >= +0.3% on the reference component; the combined gate is what
  matters.
- BC4 / BC5 / BC3: layout search must not regress; ideally +0.2-0.5% on BC4/BC5.
- CPU decompression (ground.png benchmark) within 2% of baseline; GPU benchmark within noise.
- All tests pass, ASan clean, GPU validation layers clean.

**Hard stop:** if the combined gain is < +0.5% after the offline study and a prototype, record the
measurement in `failed_experiments.md` and do not ship the format change.

---

## 2. Why these two, and why merged

- Exp 22 measured the layout effect: 62/74 images improve, best +9%, worst -2.9% (horizontal-grain
  wood). A per-image choice is the obvious way to capture the variance; the compressor is allowed
  to be much slower, so trial encoding is acceptable.
- The reference model is unique in the stream: one 256-symbol model, a large global alphabet, and a
  near-uniform global distribution. Exp 13 showed the *global* index surface is not smooth and
  reordering/delta cannot fix it; exp 16 showed more global centroids lose. Neither measured
  `H(reference | tile)`. If a tile uses a small subset of centroids, a per-tile alphabet can cut the
  reference code substantially while the global table and the per-block `nearest32` result stay
  identical.
- The two features share the same evaluation machinery: per-tile histograms, a per-image exact-size
  gate, a scratch output buffer, and header plumbing. They also interact: the layout determines
  which blocks form a tile, hence the tile histogram and the local-alphabet decision. Evaluating
  them together avoids a layout chosen under one reference model and then re-optimized under
  another.

**No-regression by construction:** the current fixed 8 x 8 layout and the current global reference
model are both candidates in the search. The selected stream is the smallest actual stream, so the
result can only be smaller than the baseline (BC1 layout byte is zero net header; BC5 pays at most
2 bytes; see section 5).

---

## 3. Design overview

### 3.1 Tile layout

- Keep 64 independent tiles (GPU dispatch unchanged).
- Store a per-stream layout index in the header: `layout_log2` in 0..6, `gx = 1 << layout_log2`,
  `gy = 64 / gx = 1 << (6 - layout_log2)`.
- Tile bounds use shifts instead of division:
  `x0 = (tx * width_blocks) >> layout_log2`, `x1 = ((tx + 1) * width_blocks) >> layout_log2`,
  `y0 = (ty * height_blocks) >> (6 - layout_log2)`, `y1 = ((ty + 1) * height_blocks) >> (6 - layout_log2)`.
  The even split still covers every row/column (exp 22's `odd_dimensions` property).
- Candidate set: `gx in {1, 2, 4, 8, 16, 32, 64}`, filtered by `gx <= width_blocks`
  (height is >= 64 blocks by the API's `height >= 256` requirement, so `gy <= height_blocks`
  always holds). Default / tie-break: 8 x 8 (log2 = 3).
- The encoder tries candidates and keeps the smallest stream. Start with full trial encoding of
  every valid candidate; compression is offline and the suite is small. Add proxy pruning only if
  the encoder time becomes a problem.
- BC4 / BC5: same layout field and search; no local reference alphabet.
- BC3: each substream stores its own layout; the halves are independent streams, so per-half layouts
  are valid (the 64-call decoder covers every block exactly once per half). Update the README note
  that the halves "share the decomposition"; the format supports either, and the encoder can use
  per-half optima. If a shared layout is preferred for clarity, sum the two halves' sizes per
  candidate and pick the best shared layout instead.

### 3.2 Per-tile reference alphabet (BC1 only)

For each tile:

1. Accumulate the tile's reference histogram (top-table indices) during the existing histogram
   pass. `nearest32` is per block and layout-independent; only the grouping changes. Count only
   blocks that actually emit a reference: blocks collapsed by flat-index mode 1 are excluded (they
   have no reference symbol). The global model's alphabet may remain the all-blocks superset; only
   the per-tile cost/decision uses the emitted counts.
2. Fit the global reference model from the all-blocks histogram, exactly as today. Its alphabet is
   a *superset* of every tile's references, which guarantees a lossless local alphabet.
3. For each tile, evaluate candidate promotion sizes `S in {8, 16, 32}` (only sizes <= the global
   model's `num_symbols`):
   - `list` = the S most frequent references in the tile, ties by lower table index; if the tile has
     fewer than S distinct references, fill with the least frequent global alphabet symbols not
     already in the list (they are never emitted; this only shifts tail ranks minimally).
   - Local alphabet = `list[0..S-1]` followed by the global alphabet in its existing order, minus the
     promoted symbols. This is a **permutation of the same symbol set**, so `num_symbols` is
     unchanged and the raw-mode width is unchanged.
   - Fit `(k, q_escape)` for the local alphabet from the tile histogram (new internal helper; see
     section 6).
   - Cost = 2-bit size code + `S * 8` list bits + 2 bytes `(k, q_escape)` + exact tile payload cost
     under the local model.
   - Compare with the no-local cost: 2-bit size code + exact tile payload cost under the global
     model.
4. Global option decision:
   - Option A (stream-level local flag = 0): every tile uses the global model, no per-tile header
     bits. This is exactly today's reference behavior.
   - Option B (flag = 1): every tile writes the 2-bit size code; tiles that do not pay choose size 0
     and use the global model.
   - Compute both totals and pick the smaller. The flag is stored in the layout byte.
5. Emit the chosen per-tile headers at the start of each tile bitstream; encode the reference symbol
   with the tile's local model (or the global model for size 0).

The decoder:

- Loads the global 19 models as today.
- For each tile, if the stream-level flag is set, reads the size code, the list, `k`, `q_escape`,
  and builds a local `le_model` from the global alphabet (one 256-entry pass per tile).
- Uses that model for the tile's reference symbols. The per-block loop is unchanged.
- If the global reference model has zero symbols (all-flat mode 1 image), there are no reference
  symbols; the local machinery is skipped.

### 3.3 What this does *not* do

- No per-tile `k` for colors, mask, or difference (separate experiment; keep this plan focused).
- No local alphabet for the difference / mask models (promising but a separate risk profile).
- No Huffman/range coder, no new color contexts, no dictionary changes.
- No change to the top table, `nearest32`, the VQ refinement, or the residual structure.

---

## 4. Formats

### 4.1 Stream version

Breaking stream-format change: library version becomes **2.0.0** at commit time (major bump per
`AGENTS.md`). No public API change: `bcp_compress_*`, `bcp_decompress_*`, `num_strips = 64` and the
signatures stay identical.

### 4.2 Header bytes

**BC1** (current order: models, top-table size u8, flat mode u8, 4-align, seed u32, predictor u64,
dict enable u64, table u32[], 2-align, offsets u16[], 4-align, tiles):

- Insert `layout u8` after the flat-mode byte and before the 4-align.
- Header size delta: top-table size + flat + layout = 3 bytes, then 1 pad to 4. Before: top-table
  size + flat = 2 bytes, then 2 pad. **Zero net growth.**
- `layout` bits:
  - bits 0..2: `layout_log2` (0..6; 3 = 8 x 8 default).
  - bit 3: `ref_local_enabled` (stream-level Option B flag).
  - bits 4..7: reserved (write 0; decoder rejects non-zero for forward compatibility, or ignores).
- **Alternative (preferred if both plans ship):** [header_compression.md](header_compression.md) no
  longer needs table-mode bits in the flat-mode byte (table delta coding is scrapped), so bits 3-7
  of that byte are free. Put `layout_log2` (bits 3-5) and `ref_local_enabled` (bit 6) there and drop
  the separate `layout u8` entirely: the BC1 header stays 2 bytes + 2 pad (zero net growth) with no
  new field, and BC4/BC5 can use spare high bits of their flat bytes the same way (BC5 then needs no
  +2 B). Verified in the current code: the writer emits `mode | (value << 1)` and the reader masks
  only those bits, so BC1 has bits 3-7 free and BC4/BC5 have bits 4-7 free per channel.

**BC4 / BC5** (models, seeds, flat byte per channel, 2-align, offsets, 4-align, tiles):

- Insert `layout u8` after the per-channel flat bytes, before the 2-align.
- BC4: seed 2 + flat 1 + layout 1 = 4, already 2-aligned; **zero net growth**.
- BC5: seed 4 + flat 2 + layout 1 = 7 -> 8 (one pad); **+2 bytes**.
- Increase `packed_bc4_maxsize` / `packed_bc5_maxsize` by the worst-case layout-byte growth (2 bytes)
  and add a maxsize test. `packed_bc3_maxsize` inherits it.

**BC3**: the BC1 substream and the BC4 substream each carry their own `layout` byte, exactly as
above. The BC3 size prefix is unchanged.

### 4.3 Per-tile reference header (inside each BC1 tile bitstream, when `ref_local_enabled`)

Written with `le_write_bits` / `le_write_byte` at the start of the tile bitstream, before the first
block:

| field | bits | notes |
|---|---:|---|
| size code | 2 | 0 = none, 1 = 8, 2 = 16, 3 = 32 |
| list | `size * 8` | top-table indices in local-frequency order; 0 bits if size code 0 |
| `k` | 8 | same encoding as the model header (bit 7 = raw mode; reference is normally Rice) |
| `q_escape` | 8 | 0..31 |

When `ref_local_enabled = 0`, the tile bitstream has **zero** extra bits and is byte-identical to
the layout-only format.

### 4.4 Local alphabet construction (encoder and decoder must match)

```text
promoted[256] = false
local_alphabet[0..S-1] = list[0..S-1]
for i in 0..S-1: promoted[list[i]] = true
rank = S
for r in global_alphabet[0..global_num_symbols-1]:
    if !promoted[global_alphabet[r]]:
        local_alphabet[rank++] = global_alphabet[r]
assert(rank == global_num_symbols)   // the list is a subset of the global alphabet
```

- The local model's `num_symbols` equals the global model's.
- The encoder rebuilds `index[symbol] = rank` for the local model; the CPU/GPU decoders only need
  `alphabet`.
- Validation (CPU decoder, corrupted streams): each list entry must be `< top_table_size` and must
  be present in the global alphabet, and `S <= global_num_symbols`; otherwise return false. The GPU
  contract remains "garbage on corrupt streams", as today.

---

## 5. Encoder algorithm

```text
build_top_table()                              // layout-independent, once per image
flat mode scan                                 // layout-independent result

best_size = SIZE_MAX
for each valid layout L in {1,2,4,8,16,32,64} filtered by width_blocks:
    init_static_models(L)                      // seed, predictor flags, dict flags,
                                               // color models, reference/mask/diff models,
                                               // per-tile reference histograms + local decisions
    size = compress(L)
    if size < best_size:
        best = (L, models, stream)
        best_size = size

return best stream in the caller's output buffer
```

Details:

- `init_static_models` gains a `layout_log2` parameter and the per-tile reference histogram array
  (`uint32_t ref_hist[64][256]`, 64 KiB).
- The global reference model is fitted first (all-blocks histogram, as today). Then the per-tile
  decisions (Option A vs B, per-tile size/list/k/q) are computed and stored in the context:
  `uint8_t ref_size[64]`, `uint8_t ref_list[64][32]`, `uint8_t ref_k[64]`, `uint8_t ref_q[64]`.
- The compress pass uses `ref_local_enabled` and the per-tile decision; the block loop changes only
  the model pointer for the reference symbol.
- **Scratch buffer:** trial encoding needs a copy of the best stream. Allocate one scratch buffer of
  `output_length` bytes through the existing `bcp_mem_interface` on the first compress, free it in
  `bcp_terminate`, and return 0 on allocation failure. Document the extra working memory. (This is
  the same mechanism already used by the 1 MiB hashmap.)
- **Tie-break:** smaller size wins; on equality prefer the default 8 x 8 layout, then the lower
  layout index, then Option A over Option B. Deterministic streams are required.
- **BC3:** the BC1 and BC4 compressors each run their own layout search. If a shared layout is
  desired, run the search at the BC3 level over the combined sizes instead.
- **Encoder time:** expect a 3-7x multiplier for BC1 (7 candidates), similar for BC4/BC5. The README
  already says compression is a lot slower; document the multiplier. If needed, add a two-stage
  search: rank candidates with a cheap proxy (color delta energy + reference local-alphabet ideal
  cost + dictionary match count), full-encode only the best 2-3 plus 8 x 8. Do not trust the proxy
  alone (exp 7/8b lesson); it only prunes.

---

## 6. Internal helpers (no public API change)

1. `tile_bounds_get(strip_index, width_blocks, height_blocks, layout_log2)` — replace the
   width-derived `gx` with the stored `layout_log2`; shifts instead of divisions.
2. `le_model_fit_kq(le_model* model, const uint32_t* rank_histogram, uint32_t num_symbols)` —
   factor the existing `(k, q_escape)` search out of `le_static_model_init`. The global init keeps
   its frequency sort and calls this; the local model builds its own alphabet order and calls this
   with `rank_histogram[rank] = tile_hist[local_alphabet[rank]]`.
3. `ref_local_model_build(le_model* local, const le_model* global, const uint8_t* list, uint32_t size,
   uint8_t k, uint8_t q_escape)` — shared encoder/decoder alphabet permutation.
4. `ref_local_cost(...)` — exact bit cost of a tile under the global or a local model, including
   header bits. Used only by the encoder.

Keep these in `src/bc_packed.c` with header documentation only where it describes a public contract;
no descriptive comments in the .c.

---

## 7. Decoder changes

### CPU (`bcp_decompress_bc1_impl`)

- Read the `layout` byte after the flat-mode byte; validate `layout_log2 <= 6` and
  `(1 << layout_log2) <= width_blocks`; compute bounds from it.
- After `le_begin_decode` for the current tile:
  - If `ref_local_enabled`, read the 2-bit size code; if non-zero, read the list, `k`, `q_escape`,
    build the local `le_model` (a stack copy), and set a pointer to it; otherwise use the global
    model.
- The block loop uses `reference_model` for `le_decode_symbol`; everything else is unchanged.
- All-flat / zero-symbol models: skip the local machinery when the global reference model has zero
  symbols.
- Keep the existing overrun/status behavior; add list validation as described in 4.4.

### GPU (`bcp_decompress.metal`, BC1 kernel)

- Read the `layout` byte; `tile_grid_x` becomes `1 << layout_log2`; bounds use shifts.
- Keep a `model_t` copy of the global reference model; at tile start, if the flag is set, read the
  size/list/k/q and overwrite a local `model_t` with the permuted alphabet, `k`, `q_escape`
  (a 32-byte promotion bitmap in local memory is enough).
- The per-block code is unchanged (`decode_symbol(&reference_model)`).
- Extra thread-local memory: ~260 bytes (one model copy) + 32 bytes bitmap. No extra per-block work.
- Mirror the layout read in the BC4 / BC5 kernels (no local reference there).

### BC3

No shader. The CPU BC3 decompressor calls the BC1/BC4 implementations; each reads its own
substream's layout. Confirm the 64-call loop still covers every block exactly once when the two
halves have different layouts (the writes are disjoint per half).

---

## 8. Implementation stages

Each stage ends with a build, the unit tests, and a measurement note.

### Stage 0 — offline study (no library change)

Replicate the pipeline (stb -> DXT -> `build_top_table` -> `nearest32`) and measure:

- Full stream size per candidate layout under the current code (temporary compile-time layout
  override), vs the 8 x 8 baseline.
- Per-tile reference support size, entropy, top-S coverage, and the ideal cost of the local
  alphabet (including header bits) vs the global model.
- Per-image best layout and the local-alphabet adoption rate.
- For BC4 / BC5: per-image best layout.

Decide: keep the full candidate set or prune; keep promotion sizes `{8,16,32}` or add 4; confirm the
local alphabet's upside before touching the format.

**Gate:** if per-tile reference entropy is ~7.4 bits (no concentration), drop point 2 and ship only
the layout search (which is still worthwhile). Record the study in the plan/experiment log.

### Stage 1 — layout plumbing (no search yet)

- Add `layout_log2` to the context; thread it through `tile_bounds_get`, all encoder/decoder passes,
  BC1/BC4/BC5/BC3, and the Metal kernels.
- Add the header byte at the zero-net positions; update `packed_bc4_maxsize` / `packed_bc5_maxsize`.
- Fixed `layout_log2 = 3` (8 x 8) for now; streams must round-trip CPU + GPU and the suite results
  must match the baseline (modulo the BC5 +2 bytes).

Tests: existing suite; a synthetic image compressed with an internal forced layout (test-only hook)
for every candidate, CPU + GPU round-trip; `odd_dimensions` for each layout.

### Stage 2 — layout search

- Build the top table once; loop over valid candidates; `init_static_models(L)` + compress; keep the
  best via the scratch buffer.
- Deterministic tie-breaks; return the best stream.
- Tests: round-trips; the chosen stream size is <= the size of a forced-8 x 8 encode (no-regression
  property); encoder time reported.

### Stage 3 — per-tile reference histograms + decision, no emission

- Accumulate `ref_hist[64][256]` in the histogram pass.
- Fit the global model; compute Option A/B totals and the per-tile decisions; store them.
- Do **not** write the local headers yet: emit the global model and compare the predicted
  global-only reference cost with the actual reference bytes. This validates the cost model against
  the real stream (exp 13's caution); the local half is validated in Stage 4 by comparing the
  predicted local total with the actual stream size.
- Measure the predicted reference savings per image.

### Stage 4 — emit and decode local alphabets

- Write the stream-level flag and the per-tile headers; encode the reference with the local model.
- Decoder + GPU: read and build the local model; CPU/GPU byte-identical output.
- Tests: a synthetic image with one half of the tiles repeating a few reference patterns and the
  other half using unique patterns (exercises local + global tiles); an image where every tile uses
  a local alphabet; an all-flat image; a 1-entry top table image.

### Stage 5 — combined gating

- The layout search evaluates the full stream including the local option; the best layout + option
  wins.
- Run the full image suite; compare BC1/BC3/BC4/BC5 ratios, per-image deltas, total bytes, CPU/GPU
  decode throughput, encoder time.

### Stage 6 — tuning (only if Stage 5 is positive)

- Promotion sizes / list fill order.
- Per-tile `k/q` packing (2 bytes -> 9 bits) if the header matters on 512^2.
- Global reference model `k/q` refit on the non-local blocks (keep the all-blocks alphabet),
  then re-evaluate the decisions once (exp 7's two-pass lesson).
- Local list delta-coding vs the previous tile (adjacent tiles may overlap), if the list is a
  measurable share on 512^2.
- Two-stage layout proxy pruning if encoder time is unacceptable.
- Optional cleanup (measure separately, not required for this CL): the current reference/mask/
  difference histograms include blocks that flat-index mode 1 collapses, even though those blocks
  emit no index payload. Subtracting the flat blocks' counts (the `color_hists_sub` pattern) fits
  the models to the emitted distribution and can shrink the headers on flat-heavy images.
- Optional: per-tile local alphabet for the **difference** model (separate experiment; do not fold
  into this CL).

### Stage 7 — cleanup / docs / version

- README: stream format, tile layout search, per-tile reference alphabet, decoder/GPU notes,
  encoder-time note.
- `bc_packed.h`: version 2.0.0 (major, breaking stream format); no API change.
- `failed_experiments.md`: exp 24 entry with the measurements (kept or reverted), per-image deltas,
  component payloads, CPU/GPU speed, encoder-time multiplier.
- `logs/`: new stats CSV.

---

## 9. Measurement and ablation matrix

Run on the full 74-image BC1 suite, plus the BC4/BC5/BC3 suites.

| variant | layout search | local ref | expected |
|---|---|---|---|
| baseline (current) | fixed 8 x 8 | off | 1.4907 |
| A | search | off | +0.4% or more; no per-image regressions |
| B | fixed 8 x 8 | on | reference component down; total +0.3% or more |
| C | search | on | >= max(A, B), target +1.0% |
| C1 | search | on, Option A only | confirms the local option's marginal value |
| C2 | search | on, sizes {8} / {8,16} / {8,16,32} | pick the default |
| C3 | search | on, global k/q refit | measure the two-pass gain |
| C4 | search | on, difference model local alphabet | separate follow-up, not in this CL |

Record per image: ratio, stream bytes, best layout, local-tile count, reference bytes,
header bytes, CPU/GPU decode rate. Compare against the baseline with the same suite and build
configuration (the exp 12/13 caution: same code, same suite).

Also measure the *pure* layout effect on BC4 / BC5 / BC3 (the local reference does not apply there).

---

## 10. Risks and mitigations

| risk | mitigation |
|---|---|
| Per-tile reference support is not concentrated (entropy ~7.4 bits) | Stage 0 study first; drop point 2 if it does not clear +0.3% |
| Local list header eats the gain on 512^2 tiles | Per-tile exact-cost gate; promote sizes include 8; Option A avoids all 2-bit codes; gate the whole option per image |
| Layout search multiplies encoder time | Offline compression is allowed to be slow; start with all candidates; add proxy pruning or a subset if needed; document the multiplier |
| Scratch output buffer memory | Allocate through `bcp_mem_interface`, free on terminate, return 0 on failure; the context already allocates the 1 MiB hashmap; document |
| Distribution reshape / k cliff (exp 7/8b/15) | The decision is exact per tile against the *actual* model; the local model's k/q is fitted to the local histogram; the global model is fitted to all blocks; optional refit on non-local blocks with a second decision pass |
| Losslessness with a promoted symbol not in the global alphabet | The global model is fitted on all blocks, so its alphabet is a superset; the local alphabet is a permutation of the same symbol set; validate on decode |
| CPU/GPU divergence | Both build the local alphabet from the same global model and list; the GPU tests compare byte-exact output |
| Corrupt-stream OOB from an invalid list | CPU validation (list entry < top_table_size, present in the global alphabet, S <= num_symbols); keep the current corrupt-stream contract |
| Format break | Version 2.0.0; README update; no API change |
| BC3 halves with different layouts | Per-half layouts are valid because the halves are independent; the 64-call loop covers each half exactly once. If this is undesirable, use a shared layout chosen by the combined size |
| Maxsize bound overflow | The layout byte is zero net on BC1/BC4 and +2 on BC5; update the BC4/BC5 maxsize constants and add a maxsize test |

---

## 11. Test plan

**New tests (behavior, not implementation):**

1. `tile_layout_roundtrip`: synthetic images that favor each layout (horizontal stripes, vertical
   stripes, checkerboard, random, uniform) -> CPU + GPU round-trip byte-exact; the stream size is
   never larger than the fixed-8 x 8 baseline (no-regression property).
2. `tile_layout_odd_dimensions`: 36 x 70 blocks for each valid layout; every block is written
   exactly once (the exp 22 trailing-row fix).
3. `ref_local_mixed`: left half of the tiles repeat a few index patterns, right half unique ->
   local + global tiles in one stream; CPU + GPU round-trip.
4. `ref_local_all_tiles`: every tile has a small reference set -> all tiles local; round-trip.
5. `ref_local_flat_image`: all-flat image (zero reference symbols) + local flag on; round-trip.
6. `ref_local_small_table`: 1-entry top table (the exp 7 empty-table floor case) + local flag;
   round-trip.
7. BC4 / BC5 / BC3 layout round-trips: one synthetic image per format, CPU + GPU where a shader
   exists.

**Regression / validation:**

- Full existing suite (74 BC1, 4 BC4, 4 BC5, 2 BC3) + GPU round-trips.
- ASan build: no reports. GPU validation layers: no messages.
- `packed_bc*_maxsize` bound test with a worst-case synthetic image.
- CPU benchmark (`ground.png`, 1000 runs, 18 threads) within 2% of baseline; GPU benchmark within
  noise.
- Deterministic streams: two compressor runs produce identical bytes.

**Do not weaken existing tests.** If a threshold needs updating, justify it with the suite log.

---

## 12. Acceptance criteria

- Combined feature: BC1 suite average >= **+1.0%**; zero images regress by more than 0.1%; total
  stream bytes down; byte-weighted ratio down.
- Layout-only fallback: if point 2 fails, ship the layout search alone if it clears **+0.4%** and
  keeps BC4/BC5/BC3 non-regressing.
- CPU/GPU decompression within noise (CPU <= 2% slower); no new per-block work.
- All tests pass; ASan clean; GPU validation clean; deterministic streams.
- Encoder time multiplier documented; scratch memory documented.

If the combined gain is below +0.5%, write the experiment (method, numbers, why it lost) in
`failed_experiments.md` and do not ship the format change.

---

## 13. Open decisions (resolve in Stage 0/5)

1. Is per-tile reference entropy actually lower than the global 7.49 bits? (decisive for point 2)
2. Which promotion sizes pay on 512^2 vs 1024^2? Is 4 worth adding?
3. Is the per-tile `k/q` (beyond the alphabet permutation) worth the 2 header bytes?
4. Does the global `k/q` refit on non-local blocks help enough to justify the second pass?
5. Which layout candidates can be pruned without losing the best per image?
6. BC3: per-half layouts or a shared layout? Measure both on the 2-image suite.
7. Is the layout win coming from the layout itself or from re-deciding the per-tile predictor /
   dictionary flags? Ablate (layout search with the flags forced to the 8 x 8 choices).

---

## 14. Files expected to change

| file | change |
|---|---|
| `src/bc_packed.c` | layout plumbing, per-tile reference histograms, local model helpers, encoder trial loop, CPU decoder, scratch buffer |
| `src/bc_packed.h` | version 2.0.0 only (no API change) |
| `src/bc_decompress.metal` | layout read, tile bounds, local reference model build (BC1); layout read in BC4/BC5 |
| `README.md` | stream format, layout search, local reference alphabet, encoder time, GPU notes |
| `tests/synthetic_tests.c` | new layout / local-reference tests |
| `tests/gpu_tests.cpp` | GPU round-trips for the new paths |
| `tests/image_tests.c` | suite thresholds / stats |
| `tests/benchmark_test.c` | no functional change; use for the speed gate |
| `failed_experiments.md` | exp 24 entry (kept or reverted) |
| `plan/tile_layout_and_local_reference.md` | this plan |

---

## 15. Estimated payoff

- Layout search: **+0.4-1.0%** BC1 (exp 22's per-image spread is the evidence); ~0 on BC4/BC5
  (their 1024^2 maps already have enough block rows per tile), BC3 inherits the BC1 color-half win.
- Local reference alphabet: **+0.3-1.5%** BC1 if the per-tile support is concentrated; zero if not
  (the gate disables it). The reference is ~17% of the stream, so even a 1-bit/block reduction is
  ~1% of the stream.
- Combined target: **+1.0-2.0%** BC1 (1.4907 -> ~1.506-1.520) with no meaningful decode slowdown,
  plus the layout search's BC3 gain. If the reference concentration is strong, the upper end is
  plausible; if not, the layout search alone is the deliverable.
