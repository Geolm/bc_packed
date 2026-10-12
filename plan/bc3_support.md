# BC3 support — implementation plan

Status: **done 2026-10-12** (approved same day: Q1 stats removed from the whole API, Q2 CPU-only, Q3 ≥ 1.0, Q4 no benchmark, Q5 worked on the existing `bc5` branch). Implemented, validated (31/31 tests, BC3 average **2.7149:1**), documented in the README.

Extends `bc_packed` with lossless asymmetric compression of BC3 (color + alpha, 16 bytes per
4×4 block) textures. Source images are in `images/bc3/` (`leak-stain_albedo.png`, `leaves.png`,
both 1024×1024 RGBA).

The whole point of this design: **BC3 reuses the two already-validated pipelines instead of
duplicating any of their logic.** BC3 is exactly "a BC1 block + a BC4 block" per 4×4 cell, and
both the BC1 and the single-channel BC4 pipeline (the `num_channels = 1` half of the existing
BC4/BC5 code) already exist. A BC3 stream is the composition of one BC1 stream and one BC4
stream; a BC3 decompress is the composition of the two existing decompressors. No new payload
code, no new models, no new context fields.

---

## 1. Key design decision: composition of two substreams (not a merged stream)

A BC3 block is a `bc1_block` (8 B) followed by a `bc4_block` (8 B, alpha). Two ways to pack a
BC3 stream:

**A (chosen) — two independent substreams concatenated:**

```text
[u32 bc1_stream_size (LE)] [BC1 stream] [BC4 stream]
```

Each substream is byte-identical to what `bcp_compress_bc1` / `bcp_compress_bc4` produce today:
own models, own strip seed, own 63 strip offsets, own 64 strips. Both end 4-byte aligned (the
existing per-strip `stream_align`), so the second substream always starts 4-aligned and both
can be decoded standalone. The u32 prefix makes the split self-describing.

**B (rejected) — one merged stream:** single header, per-strip interleaved color + alpha
bitstreams in one offset chain. Tighter by a few hundred bytes, but it requires refactoring
the validated BC1/BC4/BC5 paths into a shared strip scaffold + extracted per-format payload
functions (≈ 400 lines of churn in the flagship BC1 path, function-pointer dispatch). The
gain is a couple of KB on a 1 KiB texture; the risk is regressions in the paths the 74-image
BC1 suite and the GPU tests pin down. Rejected.

Cost of A: a 4-byte prefix + a duplicated model header/offset array (a few hundred bytes).
Both substreams' models are fitted on their own data exactly as for standalone BC1 / BC4, so
the entropy is *identical* to the standalone formats — nothing is lost, and parallel
decomposition stays 64 strips, same as BC1.

## 2. Block layout (I/O contract)

Standard D3D BC3, exactly as you described: the 8-byte BC1 color block followed by the 8-byte
BC4 alpha block, 16 bytes per 4×4 cell. `bcp_compress_bc3` consumes and `bcp_decompress_bc3`
reproduces these bytes byte-exact. No `bc3_block` struct is added to the public header: the
grid is documented as "a `bc1_block` followed by a `bc4_block`", the same way the BC5 docs say
"red `bc4_block` followed by the green one".

**stb pitfall (tests only):** `stb_compress_dxt_block` with `alpha = 1` writes the **alpha
block first, color second** — the opposite of the D3D order. The tests build BC3 blocks with
`stb_compress_dxt_block(dest, rgba, 0, STB_DXT_HIGHQUAL)` (color, bytes 0–7) +
`stb_compress_bc4_block(dest + 8, alpha16)` (alpha, bytes 8–15). The library itself never
depends on stb.

## 3. Stream format

```text
* u32 little-endian: the size in bytes of the BC1 substream that follows.
* The BC1 stream, exactly as documented in the README "Compressed stream" section
  (17 models, top table, seed, flags, 63 offsets, 64 strips).
* The BC4 stream, exactly as documented in the README "BC4 / BC5 → Compressed stream"
  section with 1 channel (2 endpoint models, 24 index models, 2-byte seed, 63 offsets, 64 strips).
```

No width / height / format in the stream, as every other format: `bcp_decompress_bc3` is a
separate entry point that knows its own prefix + the two substream headers.

## 4. Public API (src/bc_packed.h)

```c
// Maximum size of the compressed stream for a given BC3 texture, in bytes:
// packed_bc1_maxsize + packed_bc4_maxsize + 4 (the prefix).
size_t packed_bc3_maxsize(uint32_t width, uint32_t height);

// [bc3_image] holds (width/4) * (height/4) standard 16-byte BC3 blocks: the bc1_block
// (color) followed by the bc4_block (alpha). Same constraints and return conventions
// as bcp_compress_bc1.
size_t bcp_compress_bc3(bcp_context* ctx, const void* bc3_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

// Decompresses one strip of the 16-byte block grid; call it num_strips times for the
// full image. Returns false on error, as bcp_decompress_bc1.
bool bcp_decompress_bc3(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);
```

No new `bcp_context` fields: the BC1 models (17) and the BC4 models (26 for 1 channel) already
coexist in the context; the two substreams never touch the same fields.

## 5. Core implementation (src/bc_packed.c)

The one real piece of work: **strided block views.** The BC1 color half of a BC3 grid is a
`bc1_block` at byte offset 0 with a 2-block stride; the alpha half is a `bc4_block` at byte
offset 8 with the same stride. Every internal function that indexes blocks therefore gains a
`uint32_t block_stride` parameter (in units of its 8-byte base block):

- BC1: `vq_refine_round`, `vq_top_table` / `build_top_table`, `init_static_models` (seed pass +
  predictor pass + dictionary pass), the `bcp_compress_bc1` strip loop (`current`, `top`), the
  `bcp_decompress_bc1` loop (`current`, `color_reference`) — ~12 pointer expressions, all of the
  form `+ i` becoming `+ i * block_stride`.
- BC4/5: `bc4_init_models` (both passes), the `bcp_compress_bc45` / `bcp_decompress_bc45` loops
  — the existing `* num_channels + c` index becomes `* block_stride + c`; BC4/BC5 callers pass
  `block_stride = num_channels` (1 / 2, unchanged layout), the BC3 alpha view passes
  `block_stride = 2, num_channels = 1`.
- The public `bcp_compress_bc1` / `bcp_decompress_bc1` bodies move verbatim into new static
  `*_impl` functions carrying the extra parameter; the public wrappers pass 1. BC1 / BC4 / BC5
  **stream formats are byte-identical before and after** (stride 1 is the existing layout); the
  existing tests pin that down — no test changes allowed for the stride refactor commit.

Then the two new public entry points are small:

- `bcp_compress_bc3`: guard (width ≥ 16, height ≥ 256, pointers) → reset the BC1 and BC4 bit
  counters → reserve 4 bytes for the prefix → call the BC1 impl on the color view (stride 2,
  `output + 4`, length − 4) → write the prefix → call the BC4/5 impl (1 channel) on the alpha
  view (`(bc4_block*)(image + 8)`, stride 2) → return `4 + bc1_size + bc4_size`.
- `bcp_decompress_bc3`: read the prefix, sanity-check it against `input_length`, then call the
  two impls on the two views of `output` and return their AND.

Estimated new code: ≈ 60–80 lines of genuinely new logic (wrappers + prefix + stats if (b));
the stride threading is a mechanical parameter, not new logic. Nothing is duplicated: the BC3
payload is the BC1 payload + the BC4 payload, executed by the existing code.

GPU / Metal: **out of scope for this change** (Q2). CPU-only mirrors the BC4/BC5 plan's Q6
answer; a `bcp_decompress_bc3` kernel (one thread per strip, the existing BC1 + BC4 bodies with
a strided output buffer) is a clean follow-up.

## 6. Tests (all in the existing suites, no new CMake entry, no new files)

- **Synthetic** (`tests/synthetic_tests.c`): a `bc3_roundtrip(image, width, height)` helper
  mirroring `bc45_roundtrip` (16-byte grid, all 64 strips, byte-exact), exercised on the four
  existing 256×256 patterns — flat, checkerboard, gradient, SplitMix32 random — with the BC1
  pattern values as the color half and the BC45 pattern values as the alpha half (the random
  pattern forces the alpha raw-mode fallback and full 24-context usage, as it does for BC4).
- **Images** (`tests/image_tests.c`): `bc3_images` mirroring the BC4/BC5 suite — hardcoded
  `images/bc3/{leak-stain_albedo,leaves}.png`, stb load, per block stb color + stb alpha,
  per-image ratio to stdout, suite average ≥ 1.0 (Q3). Expect ≈ 1.1–1.3:1: the BC1 albedo half
  compresses like BC1 (≈ 1.4:1+) and the alpha half like BC4 (≈ 1.0–1.1:1).
- **Benchmark** (Q4): `benchmark_bc3_leaves_decompression` mirroring the BC5 one.
- **Regression guard**: the stride refactor must not move a single byte of the BC1/BC4/BC5
  output — verified by the unchanged test suite, in particular the 74-image BC1 suite whose
  average ratio (1.4654:1) must reproduce exactly, plus the GPU bc1/bc4/bc5 roundtrips.
- **failed_experiments.md**: per AGENTS.md, if the suite average misses the threshold the
  experiment (images, measured per-part ratios: color vs alpha) is recorded there.

## 7. Documentation

README: a "## BC3" section after BC4/BC5 — block layout (D3D 16 B), the composition scheme
(references the existing BC1 / BC4 sections instead of re-describing them), the stream format
(§3), status (CPU; GPU follow-up if Q2 says so), validation (measured ratios), and a line in
the top "Library" section. The "Images" section gains the `images/bc3` source note.

## 8. Decisions (answered 2026-10-12)

Q1. **Stats — removed from the entire API.** The user dropped `bcp_stats` / `bcp_get_stats`,
    `bcp_stats_bc5` / `bcp_get_stats_bc5`, the `stream_bits_since` helper, the `*_bits` context
    counters, and the `stats_constant_image` test (its roundtrip is covered by `flat`); the
    README stats mentions were deleted. No stats for BC3 either.

Q2. **GPU.** CPU-only, no Metal work — BC3 is niche.

Q3. **Image threshold.** Suite average ≥ 1.0 (the user expects much more; measured 2.7149:1).

Q4. **Benchmark.** None — the user considers BC3 already covered by the existing benchmark.

Q5. **Branch.** The existing `bc5` branch (already checked out, `main` merged into it).

## 9. Execution order (done)

1. ✅ Stats removal (Q1): header + `.c` + `stats_constant_image` test + image-test CSV columns
   + README — clean build, 26/26, all ratios byte-identical to the pre-change run.
2. ✅ Strided-views refactor (no API change, no stream-format change): `block_stride` threaded
   through `vq_refine_round` / `vq_top_table` / `build_top_table` / `init_static_models` / the
   BC1 + BC45 loops; the public `bcp_compress_bc1` / `bcp_decompress_bc1` bodies moved into
   static `*_impl` + 1-line public wrappers; BC4/BC5 wrappers pass stride = num_channels.
   26/26 green, 1.4654 / 1.1244 / 1.0756 unchanged.
3. ✅ `bc_packed.h`: `bc3_block` is NOT added to the public header (the tests mirror the block
   types in `tests/test_helpers.h`, where `bc3_block` was added instead); `packed_bc3_maxsize`,
   `bcp_compress_bc3`, `bcp_decompress_bc3` added.
4. ✅ `bc_packed.c`: the two public BC3 entry points (prefix + the two views) + maxsize.
5. ✅ Synthetic roundtrips: `bc3_roundtrip` helper + flat / checkerboard / gradient /
   SplitMix32 random, 256×256 (color half = the BC1 pattern values, alpha half = the BC45
   pattern values).
6. ✅ Image suite: `bc3_images` on `images/bc3/{leak-stain_albedo,leaves}.png` (stb color +
   stb alpha, D3D order), per-image ratio to stdout, average ≥ 1.0. No benchmark (Q4).
7. ✅ README: BC3 section (scheme, stream, status CPU-only, validation with the measured
   ratios) + images source note.
8. ✅ Clean rebuild from scratch + full `./build/unit_tests`: 31/31 pass, 47715371
   assertions; BC1 suite average 1.4654:1 unchanged. No test failures and no sub-1:1 image —
   nothing to document in `failed_experiments.md`. Commit pending user permission (no push
   without permission).
