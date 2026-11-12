# Plan: per-image tile-layout search + per-tile local reference alphabet

Status: proposal, no code written yet.
Scope: BC1/BC4/BC5/BC3 (layout); local reference alphabet is BC1 only.
Baseline: 74-image BC1 suite **1.4907**, BC4 **1.1352**, BC5 **1.0985**, BC3 **3.9180**
([stats_2026-11-08_18_04.csv](../logs/stats_2026-11-08_18_04.csv)).
Format break: library version **2.0.0**; no public API change (`bcp_compress_*` /
`bcp_decompress_*`, `num_strips = 64` unchanged).

Two decode-cheap features sharing one evaluation mechanism (per-tile histograms, per-image
exact-size gate, scratch buffer, header plumbing). The per-block loop is unchanged: layout is
read once per tile, the local reference model is built once per tile, and the block payload uses
the existing `le_encode_symbol` / `le_decode_symbol` path.

1. **Per-image tile-layout search** — exp 22: 8x8 is +1.6% on average but per-image spread is
   +9%..-2.9%; a per-image choice captures the variance (offline compression, trial encoding OK).
2. **Per-tile local reference alphabet** — the reference is ~17% of the stream with a
   near-uniform global marginal (~7.49 bits); if a tile uses a concentrated subset of
   centroids, a local alphabet cuts its reference code. Not exp 13/16 (no reorder/delta/centroids).

**No regression by construction:** the fixed 8x8 layout and the global reference model are both
candidates; the emitted stream is the smallest actually-encoded one.

---

## 1. Design

### 1.1 Tile layout

- Keep 64 independent tiles (GPU dispatch unchanged); store `layout_log2` in 0..6 in the header:
  `gx = 1 << layout_log2`, `gy = 1 << (6 - layout_log2)`.
- Tile bounds use shifts, not division:
  `x0 = (tx * width_blocks) >> layout_log2`, `x1 = ((tx + 1) * width_blocks) >> layout_log2`,
  `y0 = (ty * height_blocks) >> (6 - layout_log2)`, `y1 = ...` (exp 22: the even split still
  covers every row/column).
- Candidates: `gx in {1, 2, 4, 8, 16, 32, 64}`, filtered by `gx <= width_blocks` (height is
  >= 64 blocks by the API's `height >= 256` requirement, so `gy <= height_blocks` always holds).
  Default / tie-break: 8x8 (`layout_log2 = 3`).
- Encoder trial-encodes every valid candidate and keeps the smallest stream. Expect a 3-7x
  encoder-time multiplier (document it); add proxy pruning only if that becomes a problem
  (exp 7/8b: a proxy may prune, never decide).
- BC4 / BC5: same layout field and search; no local reference.
- BC3: each substream carries its own layout (halves are independent streams; the 64-call loop
  covers each half exactly once), or a shared layout chosen by combined size — measure both, pick one.

### 1.2 Per-tile local reference alphabet (BC1)

1. Histogram pass accumulates `ref_hist[64][256]` (top-table indices, layout-independent
   `nearest32` result) **counting only blocks that emit a reference** (flat-index mode 1 blocks
   emit none).
2. Fit the global reference model from the all-blocks histogram, exactly as today. Its alphabet
   is a *superset* of every tile's references, guaranteeing a lossless local alphabet.
3. Per tile, evaluate promotion sizes `S in {8, 16, 32}` (only sizes <= the global
   `num_symbols`):
   - `list` = the S most frequent references in the tile (ties: lower table index); if the tile
     has fewer than S distinct references, fill with the least frequent global symbols not in the
     list (never emitted; shifts tail ranks only).
   - Local alphabet = `list[0..S-1]` followed by the global alphabet in existing order minus the
     promoted symbols — a **permutation of the same symbol set**, so `num_symbols` and the
     raw-mode width are unchanged.
   - Fit local `(k, q_escape)` from the tile histogram.
   - Cost = 2-bit size code + `8*S` list bits + 2 bytes `(k, q)` + exact tile payload under the
     local model, compared against the no-local cost (2-bit code + payload under the global model).
4. Stream-level decision: Option A (flag = 0: all tiles use the global model, zero extra bits —
   today's behavior) vs Option B (flag = 1: every tile writes the 2-bit size code; non-paying
   tiles choose 0 and use the global model). Compute both totals, emit the smaller; the flag
   lives in the header.
5. All-flat image (global reference model with zero symbols): no reference symbols, local
   machinery skipped entirely.

---

## 2. Stream format

**Header bytes** (preferred, if [header_compression.md](header_compression.md) ships: the
flat-mode byte is `mode | (value << 1)` and only those bits are read, so BC1 has bits 3-7 free
and BC4/BC5 have bits 4-7 free per channel):

- BC1: `layout_log2` in flat-byte bits 3-5, `ref_local_enabled` in bit 6 -> **zero net header
  growth**, no new field.
- BC4 / BC5: `layout_log2` in spare bits of the per-channel flat bytes -> **zero net growth**
  (BC5 avoids the +2 B of the fallback).

Fallback (if the header plan does not ship): insert a separate `layout u8` after the flat-mode
byte (BC1: 3 bytes + 1 pad, zero net; BC4: 4 bytes, 2-aligned, zero net; BC5: 7 -> 8 bytes,
**+2 bytes**). Raise `packed_bc4_maxsize` / `packed_bc5_maxsize` by 2 bytes in that case
(`packed_bc3_maxsize` inherits); add a maxsize bound test. BC3: each substream carries its own
layout, size prefix unchanged.

**Per-tile reference header** (BC1, at the start of each tile bitstream, only when
`ref_local_enabled`), written with `le_write_bits` / `le_write_byte` before the first block:

| field | bits |
|---|---:|
| size code | 2 (0 = none, 1 = 8, 2 = 16, 3 = 32) |
| list | `size * 8` (top-table indices in local-frequency order; 0 bits for size code 0) |
| `k` | 8 (same encoding as the model header; bit 7 = raw) |
| `q_escape` | 8 (0..31) |

When `ref_local_enabled = 0`, the tile bitstream has zero extra bits and is byte-identical to
the layout-only format.

**Local alphabet construction** (encoder and decoder must match):

```text
promoted[256] = false
local_alphabet[0..S-1] = list[0..S-1]
for i in 0..S-1: promoted[list[i]] = true
rank = S
for r in global_alphabet[0..global_num_symbols-1]:
    if !promoted[global_alphabet[r]]:
        local_alphabet[rank++] = global_alphabet[r]
assert(rank == global_num_symbols)
```

The encoder rebuilds `index[symbol] = rank`; CPU/GPU decoders only need `alphabet`. CPU decoder
validates each list entry (`< top_table_size`, present in the global alphabet, `S <= num_symbols`),
returns false on violation; the GPU keeps today's "garbage on corrupt streams" contract.

---

## 3. Encoder

```text
build_top_table()                     // layout-independent, once per image
flat mode scan                        // layout-independent

best_size = SIZE_MAX
for each valid layout L:
    init_static_models(L)             // + layout_log2, per-tile ref hists and decisions
    size = compress(L)
    if size < best_size: keep (L, models, stream) in the scratch buffer
return the best stream
```

- `init_static_models` gains `layout_log2` plus `uint32_t ref_hist[64][256]` (64 KiB); the
  global reference model is fitted first, then the Option A/B totals and per-tile decisions
  (`ref_size[64]`, `ref_list[64][32]`, `ref_k[64]`, `ref_q[64]`) are stored in the context.
- The compress block loop changes only the model pointer for the reference symbol.
- **Scratch buffer:** one `output_length`-byte copy of the best stream, allocated through
  `bcp_mem_interface` on first compress, freed in `bcp_terminate`, return 0 on allocation
  failure (same mechanism as the 1 MiB hashmap; document the working memory).
- **Tie-break (determinism):** smaller size wins; on equality prefer 8x8, then lower
  `layout_log2`, then Option A over Option B.
- BC3: each half runs its own search (or a BC3-level combined-size search if a shared layout is
  chosen).

## 4. New internal helpers (`src/bc_packed.c`, no public API)

1. `tile_bounds_get(strip_index, width_blocks, height_blocks, layout_log2)` — shifts; replaces
   the width-derived `gx` / division (CPU call sites and the Metal mirror).
2. `le_model_fit_kq(le_model*, const uint32_t* rank_histogram, uint32_t num_symbols)` — the
   existing `(k, q_escape)` search factored out of `le_static_model_init`; the global init keeps
   its frequency sort and calls it; the local model builds its alphabet order and calls it with
   `rank_histogram[rank] = tile_hist[local_alphabet[rank]]`.
3. `ref_local_model_build(le_model* local, const le_model* global, const uint8_t* list,
   uint32_t size, uint8_t k, uint8_t q_escape)` — the shared alphabet permutation.
4. `ref_local_cost(...)` — exact bit cost of a tile under the global or a local model, header
   bits included; encoder-only.

## 5. Decoders

**CPU** (`bcp_decompress_bc1_impl`): read the layout after the flat-mode byte; validate
`layout_log2 <= 6` and `(1 << layout_log2) <= width_blocks`. After `le_begin_decode` per tile: if
the flag is set, read the size code; if non-zero, read list/`k`/`q` and build the local `le_model`
(stack copy), else use the global model. The block loop uses `reference_model` for
`le_decode_symbol`; everything else unchanged. Skip the local machinery when the global reference
model has zero symbols; keep existing overrun/status behavior plus the list validation.

**GPU** (`src/bc_decompress.metal`, BC1 kernel): read the layout; `tile_grid_x = 1 << layout_log2`,
bounds via shifts. At tile start, if the flag is set, read size/list/`k`/`q` and overwrite a
thread-local `model_t` copy with the permuted alphabet (a 32-byte promotion bitmap in local
memory suffices). Per-block code unchanged. Extra thread memory ~260 B; no extra per-block work.
Mirror the layout read in the BC4 / BC5 kernels (no local reference there).

**BC3:** no shader; the CPU BC3 decompressor calls the BC1/BC4 implementations, each reading its
own substream's layout; confirm the 64-call loop still covers each half exactly once.

---

## 6. Implementation stages

Each stage ends with a build, unit tests, and a measurement note.

0. **Offline study** (no library change): full stream size per candidate layout vs 8x8; per-tile
   reference support / entropy / top-S coverage; ideal local-alphabet cost incl. header bits;
   per-image best layout and adoption rate; BC4/BC5 per-image best layout.
   **Gate:** if per-tile reference entropy is ~7.4 bits (no concentration), drop feature 2 and
   ship layout search only.
1. **Layout plumbing** (no search yet): `layout_log2` through context, `tile_bounds_get`,
   encoder/decoder passes, all formats, Metal kernels; header bits at the positions above;
   `packed_bc*_maxsize` bumps if the fallback byte is used. Fixed `layout_log2 = 3`; round-trips
   must match the baseline (modulo any BC5 header growth).
2. **Layout search**: candidate loop + scratch buffer + deterministic tie-breaks; assert chosen
   size <= forced-8x8 size; report encoder time.
3. **Local decisions, no emission**: accumulate `ref_hist`, compute Option A/B + per-tile
   decisions, but emit the global model; validate the predicted global reference cost against the
   actual reference bytes (exp 13's caution).
4. **Emit and decode local alphabets**: stream flag + per-tile headers; CPU/GPU byte-identical
   output.
5. **Combined**: the layout search evaluates the full stream including the local option; run the
   full suite (BC1/BC3/BC4/BC5 ratios, per-image deltas, total bytes, CPU/GPU throughput,
   encoder time) with ablations layout-only / local-only / combined.
6. **Tuning** (only if Stage 5 is positive): promotion sizes (add 4?), per-tile `k/q` packing
   (2 B -> 9 bits), global `k/q` refit on non-local blocks then one re-decision pass (exp 7),
   list delta-coding vs the previous tile, two-stage proxy pruning.
7. **Docs / version**: README (format, search, local alphabet, encoder time, GPU notes),
   `bc_packed.h` 2.0.0, [failed_experiments.md](../failed_experiments.md) entry (kept or
   reverted) with measurements, new stats CSV in `logs/`.

## 7. Tests

New (behavior-level): `tile_layout_roundtrip` (synthetic stripes / checkerboard / random /
uniform; CPU+GPU byte-exact; size never above the fixed-8x8 baseline), `tile_layout_odd_dimensions`
(36x70 blocks, every block written once, per layout), `ref_local_mixed` (half the tiles repeating
few patterns, half unique), `ref_local_all_tiles`, `ref_local_flat_image` (zero reference symbols,
flag on), `ref_local_small_table` (1-entry top table), BC4/BC5/BC3 layout round-trips.

Regression / validation: full existing suite (74 BC1, 4 BC4, 4 BC5, 2 BC3) + GPU round-trips;
ASan build clean; GPU validation layers clean; `packed_bc*_maxsize` worst-case bound test; CPU
decode benchmark (`ground.png`) within 2% of baseline, GPU within noise; two compressor runs
produce identical bytes. Do not weaken existing tests.

## 8. Acceptance criteria

- Combined: BC1 suite average >= **+1.0%**, no image regresses by > 0.1%, total stream bytes down.
- Layout-only fallback (if feature 2 fails its gate): >= **+0.4%** and BC4/BC5/BC3 non-regressing.
- CPU/GPU decompression within noise; no new per-block work; encoder-time multiplier and scratch
  memory documented.
- If the combined gain is < +0.5%: record method + numbers in
  [failed_experiments.md](../failed_experiments.md) and do **not** ship the format change.

## 9. Open decisions (resolve in Stage 0 / 5)

1. Is per-tile reference entropy actually below the global 7.49 bits? (decisive for feature 2)
2. Which promotion sizes pay (512^2 vs 1024^2)? Is 4 worth adding?
3. Is the per-tile `k/q` (beyond the alphabet permutation) worth its 2 header bytes?
4. BC3: per-half layouts or shared? Does the layout win come from the layout itself or from
   re-decided predictor / dictionary flags (ablate by forcing the 8x8 flag choices)?

## 10. Files expected to change

| file | change |
|---|---|
| `src/bc_packed.c` | layout plumbing, per-tile reference histograms/decisions, helpers, trial loop, scratch buffer, CPU decoder |
| `src/bc_packed.h` | version 2.0.0 only |
| `src/bc_decompress.metal` | layout read + shifted bounds; local reference model build (BC1); layout read in BC4/BC5 |
| `tests/synthetic_tests.c` | new layout / local-reference tests |
| `tests/gpu_tests.cpp` | GPU round-trips for the new paths |
| `tests/image_tests.c` | suite thresholds / stats |
| `README.md` | stream format, layout search, local reference alphabet, encoder time |
| `failed_experiments.md` | exp 24 entry (kept or reverted) |
