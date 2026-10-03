# Proposal: beat the 1.41:1 baseline by attacking the VQ residual

## Evidence (from `logs/stats_2026-10-03_09_18.csv`, 74 images)

Per-block stream cost, 1024x1024 class: `difference` ~21.2 bits (41%), `colors` ~18.2 (36%),
`reference` ~8.3 (16%), `mask` ~3.3 (7%).

- `reference_k` is 5-6 on every image: the reference is encoded at ~8 bits/symbol, i.e. incompressible
  (flat histogram, as exp. 2/4 concluded). All four failed experiments attacked the *reference*; none
  attacked the *residual*, which is the actual whale.
- ~2.4 differing bytes/block at ~8.8 bits/byte => the nearest table entry is ~19 Hamming flips away,
  i.e. no better than a random entry (16 flips). The codebook is not predicting.
- Mechanism: `build_top_table` seeds from top-256 exact patterns, then `vq_top_table` flips each
  centroid bit to the cluster majority (chimeras that match no real pattern), and drops count<=1 slots.
  `nearest32` then tie-breaks equal popcount by "larger value" - arbitrary with respect to what costs
  bits (number of differing bytes, and their values under the static difference model).

## Proposal (3 stages, public API unchanged, decompress path symbol-count unchanged)

### Stage 1 - Cost-aware table-entry selection (no stream format change)
In the compress pass and `init_static_models` pass, after the SIMD `nearest32` scan, pick the entry
that minimizes *expected coded cost* = mask bits + sum of differing-byte bits (both models are static
and known at selection time; reference cost used as a tie-break proxy), restricted to candidates with
popcount score <= best + 4 to keep CPU near current. Also replace the "larger value" tie-break with
"fewest differing bytes".
Expect: `difference` 21.2 -> ~16-17 bits/block, `mask` 3.3 -> ~3.0. ~10% stream reduction.

### Stage 2 - Table construction ablation + fix (no stream format change)
A/B the current `vq_top_table` against "keep the exact top-256 patterns, no centroid moves" (and, if
the refinement still wins for dedup, a gated variant that only accepts a moved centroid when it
reduces summed sample distance). First instrument: % zero-residual blocks, avg differing bytes/block,
avg nearest popcount, per image.
Hypothesis: exact patterns cover a large fraction of blocks with residual = 0 (BC1 index planes are
deterministic and repeat per texture); the chimeras inflate the residual of otherwise-exact blocks.
Expect combined Stages 1+2: `difference` -> ~13-15 bits/block.

### Stage 3 - (GATED, needs approval) Spatially adaptive codebook, stream format v2
The flat reference (16% of stream) and the residual both suffer from one global 256-entry table for
heterogeneous textures. Add: 1-byte version flag; region count in {1, 4} derived from block count
(1 if < 40000 blocks, 4 contiguous 16-strip bands otherwise - strips are the decompress unit, so
regions must be whole strips); per-region table + per-region reference model (4 models in header,
~4 KiB tables). Decoder selects region by strip index. 1024x1024 gets 4 regions; 512x512/768x512
stay single-region so their small streams pay no header penalty.
Expect: `reference` 8.3 -> ~5-6 bits/block on the 1024 class, residual slightly tighter.
This breaks compatibility with v1 streams (hence the version byte) - do only if approved.

## Target

- Suite (74 images) average ratio: **1.41 -> >= 1.50 floor, expect 1.52-1.57** with Stages 1+2;
  up to ~1.60 with Stage 3.
- Guards: 10/10 round-trip; no image regresses > 1%; decompression bench within 10%; compress time
  within +25% (exps 1/2/4 were reverted partly on speed - must stay competitive).

## Todo

- [ ] 1. Instrument residual stats (% zero-residual blocks, avg differing bytes/block, avg nearest
      popcount) per image in the suite run; confirm "table ~ random" hypothesis.
- [ ] 2. Ablate table construction: exact top-256 patterns vs current refined centroids; adopt winner.
- [ ] 3. Implement cost-aware entry selection (windowed argmin over mask+byte model cost;
      min-differing-bytes tie-break) in compress + model-init passes.
- [ ] 4. Re-baseline: full 74-image suite, record new ratio bar in `failed_experiments.md`, add ratio
      regression guards to the image suite, verify round-trip + benches.
- [ ] 5. (Gated, needs approval) Spatially adaptive codebook, format v2 with version byte + region
      tables/models; re-measure; adopt only if suite >= 1.55 and header <= 2% of small-image streams.

## Considered and set aside

- Reference delta / reordering: exps 2-4, dead (reference is a VQ index, not coherent).
- Avg/spread colors: exp 1, dead (second-order differences Rice-code worse).
- Per-strip tables: 64 x ~1.6 KiB headers = ~20% of a 512 KiB stream. Fatal.
- Bigger global table (>256 entries): `le_model` alphabet is 256 symbols (`LE_ALPHABET_SIZE`),
  reference is 8-bit - structural cap in `lite_encoding.h`.
- Nibble-granularity residual (16 nibbles, 4-symbol model): mask balloons (~12 of 16 set),
  ~28 bits vs current ~24.5. Worse on paper.
