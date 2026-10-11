# BC4 / BC5 support — implementation plan

Status: **done 2026-10-11** (approved same day, all open questions answered; implemented, validated, documented in the README).

Extends `bc_packed` with lossless asymmetric compression of BC4 (single channel, 8 bytes per
4×4 block) and BC5 (two channels, 16 bytes per 4×4 block) textures, mirroring the BC1 pipeline:
static Rice-Golomb models fitted from a histogram pre-pass, 64 independent strips for parallel
decompression, and a byte-stream format the decoder can parse standalone.

Design source: the BC4 / BC5 section of `README.md` (the repo-root note was merged in and deleted, Q9).
Entropy expectations (indices ≈ 2.97 bits raw, endpoints save ≈ 1.8 bits/symbol → expected
≈ 1.05–1.15:1) came from the study tool `tools/bc5_analysis.c`, which was removed before the API
was finalized; the image suite in §6.2 plays the role of the end-to-end check.

---

## 1. Block layout (I/O contract)

Standard D3D layout, exactly as `stb_compress_bc4_block` / `stb_compress_bc5_block` write it:

```c
typedef struct bc4_block
{
    uint8_t endpoint[2]; // [0] = max endpoint (stb mx), [1] = min endpoint (stb mn)
    uint8_t indices[6];  // 16 x 3-bit indices, little-endian 48-bit field, raster order
} bc4_block; // 8 bytes

// BC5 block = 16 bytes: the red bc4_block followed by the green bc4_block
```

- Index `i` (raster order, index 0 = top-left pixel) occupies bits `[3i, 3i+3)` of the 48-bit
  little-endian field in `indices[0..5]`.
- BC5 block `bi` = red channel at bytes `[bi*16, bi*16+7]`, green at `[bi*16+8, bi*16+15]`.
- Verified against the committed study tool: its 3-bit repack of stb output is byte-identical to
  the stb 6-byte field, so `bcp_compress_bc4/bc5` consume stb output directly (open question Q1).

## 2. Compression scheme

### 2.1 Endpoints (one channel)

Per-endpoint wrapped 8-bit delta (`curr - pred` mod 256, `prev + delta` on decode — the
`delta_encode_wrap` / `delta_decode_wrap` of the design doc), with one static 256-symbol model
per endpoint ("1 context per endpoint" → 2 models per channel).

Prediction per block of a strip (same zigzag chain as BC1:
`zigzag_x = (y & 1) ? x : width_blocks - 1 - x`, `previous` initialized to the strip seed):

```text
strip first row (y == start_y):  pred = previous            (seed for the strip's first block)
row start (x == 0, later rows):  pred = top                 (block of the row above, same column)
other blocks:                    pred = previous + top - top_previous   (mod 256, per endpoint)
```

- `top` = block at `(y-1, same physical x)`, `top_previous` = block above the chain-previous.
  All references are inside the strip (or the seed), so strips stay independent.
- Strip seed = integer average over the 64 strips of the first block in chain order of each strip
  (`x0 = (start_y & 1) ? 0 : width_blocks - 1`), per channel, per endpoint — same convention as
  the BC1 color seed. (Open question Q2: the study tool's "exact copy" predictor uses the 2D
  formula at row starts with an `above → seed` fallback; we implement the doc's top-only.)

### 2.2 Indices (one channel)

16 3-bit indices in raster order:

```text
index 0:   3 raw bits (le_write_bits / le_read_bits, no model)
index i:   symbol = prediction XOR index[i], encoded with a static 8-symbol model
           prediction = index[i-4]  (directly above)   for i in {4, 8, 12}
                      = index[i-1]  (left)            otherwise
           context  = family * 8 + prediction
           family   = |endpoint[0] - endpoint[1]| < 8  ? 0
                       |endpoint[0] - endpoint[1]| < 16 ? 1 : 2   (unsigned range, Q3)
→ 3 families x 8 prediction values = 24 models per channel
```

The family is derived from the (already decoded) endpoints, so it costs no stream bits and is
identical on encoder and decoder. No top table, no dictionary for the indices (Q5).

### 2.3 BC5

Two channels, each with its own 2 endpoint models + 24 index models + 2-byte seed
(52 models + 4 seed bytes in the header for BC5, 26 + 2 for BC4). One implementation
parameterized by `num_channels` (1 or 2): channel `c` of block `bi` is
`((const bc4_block*)image + bi * num_channels + c)`. Per-block payload order: channel 0
(red) complete, then channel 1 (green); per channel: endpoint 0, endpoint 1, index 0, indices 1..15.

## 3. Stream format

```text
* [num_channels * 2] endpoint models — per channel, per endpoint: 256-symbol static model,
  3 + num_symbols bytes each, no padding (save_static_model / load_static_model, as BC1).
* [num_channels * 24] index models — per channel, family-major: 8-symbol static model each.
* Strip seed: 2 bytes per channel (endpoint 0, endpoint 1).
* Padded to a 2-byte boundary.
* 63 strip offsets (uint16_t), relative deltas in dword units (strip 0 implicit zero), as BC1.
* Padded to a 4-byte boundary.
* 64 strip bitstreams, each 4-byte aligned, each flushed independently (le_stream), as BC1.
```

No per-strip flags: the endpoint predictor is fixed by §2.1 (BC1's per-strip predictor flag and
color-pair dictionary do not carry over). No format magic: the bc4/bc5 decompressors are
separate entry points and know their own header (model count) and block size.

## 4. Public API (src/bc_packed.h)

```c
// Maximum size of the compressed stream for a given BC4 / BC5 texture, in bytes:
// 2x the standard block size (16 / 32 bytes per 4x4 block), the BC1 convention.
size_t packed_bc4_maxsize(uint32_t width, uint32_t height);
size_t packed_bc5_maxsize(uint32_t width, uint32_t height);

// [bc4_image] / [bc5_image]: standard D3D BC4 / BC5 blocks, (width/4) * (height/4) of them,
// width and height multiples of 4, width >= 16, height >= 256 (64 strips x >= 4 block rows).
// [num_strips] receives the number of strips (64). Returns the stream size, or 0 on failure.
size_t bcp_compress_bc4(bcp_context* ctx, const void* bc4_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);
size_t bcp_compress_bc5(bcp_context* ctx, const void* bc5_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

// Extracts statistics after compression (valid after a successful bcp_compress_bc5).
// pair_bytes = the whole index payload (name kept for the study tool, Q5); dict_bytes is
// reserved for a future inter-block index dictionary, currently always 0.
typedef struct bcp_stats_bc5
{
    uint32_t endpoint_bytes; // 4 endpoint delta symbols per block (2 endpoints x 2 channels)
    uint32_t pair_bytes;     // index payload: 3 raw bits + 15 contextual symbols per channel block
    uint32_t dict_bytes;     // reserved, always 0 for now
} bcp_stats_bc5;
void bcp_get_stats_bc5(bcp_context* ctx, bcp_stats_bc5* stats);

// Decompresses one strip into [output] (the full standard BC4/BC5 block grid); call it
// num_strips times for a complete image. Returns false on error, as bcp_decompress_bc1.
bool bcp_decompress_bc4(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);
bool bcp_decompress_bc5(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);
```

- `bcp_context` gains (opaque, no ABI surface): `le_model bc4_endpoint_model[2][2]`,
  `le_model bc4_index_model[2][24]`, `uint8_t bc4_strip_seed[2][2]`, and per-component bit
  counters for the stats. ≈ 27 KB added to the malloc'd context — kept static (like the BC1
  models), no extra allocation path.
- 2× standard maxsize is provably sufficient: every model is fitted as min(Rice, raw), and the
  full-raw fallback costs ≤ 2·8 + 3 + 15·3 = 53 bits per 8-byte channel block, so the payload
  never exceeds the standard texture size (plus the small static header).
- GPU/Metal decoder: **out of scope** (Q6). No `bc_decompress.metal` change.

## 5. Core implementation (src/bc_packed.c)

Appended after the BC1 section, same file, same conventions (Allman, `//` comments, `le_*`
reuse, no new dependencies, no global state):

```c
// constants
#define BC4_INDEX_FAMILIES (3)
#define BC4_INDEX_CONTEXTS (BC4_INDEX_FAMILIES * 8) // 24
#define BCP_NUM_CHANNELS   (2)                      // max channels; BC4 uses 1, BC5 uses 2

// helpers
static inline uint8_t bc4_endpoint_predict(bool first_row, bool row_start, uint8_t prev, uint8_t top, uint8_t top_prev);
static inline uint8_t bc4_index_family(uint8_t e0, uint8_t e1);
static inline void bc4_unpack_indices(const bc4_block* b, uint8_t idx[16]); // 48-bit LE field -> 16 x 3-bit
static inline void bc4_pack_indices(const uint8_t idx[16], bc4_block* b);

// per-channel histogram: 2 x 256 endpoint + 24 x 8 index symbols (~2.8 KB)
typedef struct bc4_hists
{
    uint32_t endpoint[2][256];
    uint32_t index[BC4_INDEX_CONTEXTS][8];
} bc4_hists;

// model fit: pass 1 computes the per-channel seeds, pass 2 accumulates the histograms with the
// exact strip predictors of §2 (one pass suffices: there is no per-strip mode decision)
static void bc4_init_models(bcp_context* ctx, const void* input, uint32_t width_blocks, uint32_t strip_width, uint32_t num_strips, uint32_t num_channels);

// shared implementation, num_channels 1 or 2
static size_t bcp_compress_bc45(bcp_context* ctx, const void* image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips, uint32_t num_channels);
static bool   bcp_decompress_bc45(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index, uint32_t num_channels);
```

- Encoder: per strip, walk the BC1 zigzag chain (`previous` seeded from `bc4_strip_seed`, plus
  `top` / `top_previous` bookkeeping); per block, per channel: endpoint symbols, 3 raw bits,
  15 contextual index symbols — the same three call sites (hist / encode / decode) share the
  `bc4_endpoint_predict` helper; each per-channel loop stays well under the 50-line
  duplication threshold (same pattern as the BC1 color path).
- Stream header save/load reuses `save_static_model` / `load_static_model` / `stream_align` /
  `read_stream_align`; strip offsets and per-strip `le_stream` handling mirror `bcp_compress_bc1`
  verbatim (u16 dword deltas, 4-byte strip alignment, `assert(strip_size < 262144)`).
- Guards: `width < 16 || height < 256 || !pointers` → 0 / false, as BC1; `height_blocks % 64 != 0`
  silently drops trailing block rows, as BC1.

## 6. Tests

### 6.1 Synthetic roundtrips (tests/synthetic_tests.c, no new CMake entry)

A `bc45_roundtrip(image, width, height, num_channels)` helper mirroring the BC1 one (compress
into `packed_bc?_maxsize`, decompress all strips, byte-exact compare), exercised per pattern
with both 1 and 2 channels at 256×256: flat, checkerboard, gradient, SplitMix32 random. The
random pattern forces wide endpoint ranges (family 2), near-uniform deltas (raw-mode fallback),
and full 24-context usage — the edge cases of the format.

### 6.2 Image suite (tests/image_tests.c, existing HAS_IMAGE_TESTS guard, stb already included)

`test_bc4_images` + `test_bc5_images`, added to `suite_image` (no main_test.c change):

- Hardcoded filenames (no folder scan, per design doc):
  `images/bc4/{grey_roof_tiles_02, patterned_cobblestone, red_brick, rough_wood}_ao_1k.png` and
  `images/bc5/{...}_nor_dx_1k.png` (all 1024×1024).
- Per image: stb_image load (RGBA) → per 4×4 block: BC4 = 16 red-channel bytes through
  `stb_compress_bc4_block`; BC5 = 32 interleaved R,G bytes through `stb_compress_bc5_block` →
  `bcp_compress_bc?` (assert success) → ratio = standard size / stream length, printed to stdout
  per image + average (no CSV, per design doc) → assert **suite-average ratio ≥ 1.0** (Q8) →
  decompress all 64 strips, byte-exact compare.

### 6.3 ~~Study tool link check~~

Dropped: `tools/bc5_analysis.c` was removed (the API was finalized without it). The image
suite of §6.2 exercises the full compress → stats → decompress path instead.

## 7. Validation checklist

1. ✅ Build clean under `-Werror -Wall -Wextra -Wpedantic`.
2. ✅ `./build/unit_tests` — 24/24 pass; BC1 unchanged (74-image average 1.4654:1, all
   synthetic/benchmark/gpu tests green).
3. ✅ 8 synthetic roundtrips + 8 image roundtrips, suite averages above 1:1 (bc4 1.1244:1,
   bc5 1.0756:1; all per-image ratios also above 1:1).
4. ~~`bc5_analysis` tool builds~~ — dropped, the tool was removed (see §6.3).
5. ✅ README: BC4/BC5 sections (blocks, endpoints, indices, stream, status, validation);
   design note folded in and deleted (Q9).
6. ✅ No test failures, no sub-1:1 image — nothing to document in `failed_experiments.md`.

## 8. Decisions (answered 2026-10-11)

Q1 yes (standard D3D, direct stb output). Q2 doc version: top-only at row starts. Q3 yes
(unsigned range). Q4 yes (per-channel models). Q5 keep the tool's `pair_bytes`/`dict_bytes`
names. Q6 CPU only, GPU is a future task. Q7 yes (2× standard). Q8 **average** ≥ 1:1 per suite
(not per-image). Q9 merge the design note into the README and delete it. Additionally, the
index models use a new compact `le_model8` (8-entry rank tables, 20 bytes retained) instead of
the full 516-byte `le_model`: context growth ≈ 3 KB, not 27 KB.

---

Original questions (kept for the record):

Q1. **I/O layout.** Confirm `bcp_compress_bc4/bc5` take standard D3D blocks (3-bit indices in the
    6-byte little-endian field, ep0 = max / ep1 = min) — i.e. direct stb output, and
    decompression reproduces standard D3D bytes. (The committed study tool implies exactly this;
    its repack is byte-identical to stb output.)

Q2. **Row-start endpoint predictor.** The doc says "first block of a row uses only top"; the
    study tool's "exact copy of bc5_predict_endpoints" uses the 2D formula `prev + top − above`
    at row starts (with `above → seed` when the chain-previous sits in the strip's first row).
    Plan: implement the doc (top-only — fewer special cases, no seed mixed into the 2D term).
    OK? If so, optionally sync the tool's histogram to match the shipped predictor.

Q3. **Family range signedness.** `abs(e1 − e0)` as an unsigned value in [0, 255] (the true
    palette contrast)? Study shows ~80% of normal-map blocks land in family 2 (range 16–63), so
    only narrow-contrast blocks are affected by the interpretation.

Q4. **Per-channel models.** Each channel gets its own endpoint + index models (52 in the BC5
    header, ≤ ~2.1 KB)? Study shows red/green midpoints and ranges are uncorrelated, so
    per-channel is the right call. Confirm.

Q5. **`bcp_stats_bc5` fields.** The committed tool pins the names `endpoint_bytes` /
    `pair_bytes` / `dict_bytes`. The doc's scheme has no pair model and no dictionary. Plan:
    keep the names untouched (tool stays as-is): `pair_bytes` = index payload bytes,
    `dict_bytes` = reserved, 0. Alternative: rename to `index_bytes`, drop `dict_bytes`, patch
    the tool's one printf. Which?

Q6. **GPU scope.** No Metal decoder for BC4/BC5 in this change (design doc's tests are
    CPU-only)? GPU support would be a follow-up.

Q7. **maxsize.** 2× standard (16 / 32 bytes per block), mirroring BC1 — provably sufficient
    (§4). Confirm.

Q8. **Threshold.** The study shows the 1:1 target is *thin* on these normal maps: index entropy
    ≈ 2.97 bits raw, the doc's predictor recovers only ≈ 0.12 bits/index, endpoints save
    ≈ 1.8 bits/symbol → expected ≈ 1.05–1.15:1 before Rice quantization. Do we assert
    per-image ≥ 1.0 (plan) or average ≥ 1.0? If an image misses, tune the models (study levers:
    8 range buckets instead of 3 families, 12-bit-row delta) or document the miss in
    `failed_experiments.md`?

Q9. **Docs.** README gets the full BC4/BC5 documentation; the repo-root design note
    (`# BC4 and BC5 support.md`) is merged into the README and deleted (or moved to `plan/`)?

## 9. Execution order

1. ✅ `bc_packed.h`: block docs + API (Q1–Q7 answers first) — compile check.
2. ✅ `bc_packed.c`: helpers + `bc4_init_models` + `bcp_compress_bc45` + `bcp_decompress_bc45` +
   public wrappers.
3. ✅ Synthetic tests (§6.1) — roundtrip correctness before anything else.
4. ✅ Image tests (§6.2) — real ratios above 1:1, no tuning needed (Q8).
5. ✅ README + design-note cleanup (§7.5) (tool link check dropped, §6.3).
6. ✅ Full `./build/unit_tests` run + BC1 regression check. Commit pending user permission (no
   push without permission).
