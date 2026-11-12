# Plan: header compression

Two payload-neutral header reductions, both riding along with the v2.0.0 format bump the tile plan already requires.

## H1: rank-ordered top table, identity reference alphabet (-256 B, BC1)

Store the table in reference hit-count order (descending; ties by pattern value ascending, preserving `nearest32` tie-break). The reference alphabet then becomes identity `0..count-1` and is never transmitted.

- Measured: the alphabet is 253-256 B on every image (every entry hit) -> flat **-256 B**, no payload movement (the rank of the n-th most frequent centroid is n either way; exp 4 showed reordering the raw reference is a wash, which is what makes the alphabet free).
- Measured: 0 duplicate patterns and 0 zero-hit entries on the whole suite -> debug-build assert on uniqueness instead of a dedup pass; decoder rejects `table_size != reference count`.
- Reference model header shrinks to **3 bytes**: `count-1`, `k`, `q_escape`. Decoder sets `alphabet[i] = index[i] = i`. Table bytes unchanged: `4 * top_table_size`, raw, in rank order (no mode bits, no size field).

**Validation (H1 must be payload-neutral):** per-component byte counts (reference, mask, difference, color) identical to baseline; table byte *multiset* identical (only order may change); header shrinks by exactly 256 B. If the reference payload moves, the sort/tie-break is wrong.

## H2: omit the alphabet in raw mode when the set is a prefix (-218 B BC4 / -220 B BC5)

Raw mode writes the rank at a fixed width `ceil(log2(num_symbols))`, so alphabet order never affects bit counts. If the present symbols are exactly `{0..num_symbols-1}`, force the identity (`alphabet[i] = i`, `index[s] = s`) and omit the alphabet.

- Encoder: prefix check is a <=256-entry scan; otherwise fall back to writing the alphabet. Flag in **bit 6 of `k` (raw mode only)**; bit 7 is the raw flag, bits 0-3 the width, bits 4-5 stay free.
- `load_static_model` currently clamps `k & 0x7f`, which would destroy bit 6: extract the flag first, clamp only `k & 0x0f`.
- Force the identity in `le_static_model_init` right after the raw decision; nothing else reads a raw model's rank order (`le_symbol_cost` is width-only; the raw/Rice decision is unchanged).
- GPU (BC1 and BC4/BC5 loaders): skip the alphabet read, build the identity (~10 lines).
- Uniform across model types (8-symbol index, 16-symbol mask, 256-symbol endpoint/reference). `packed_bc*_maxsize` bounds only shrink; no constant change beyond the format bump.

## Encoder / decoder flow

- Encoder (BC1): `build_top_table` (unchanged) -> final reference pass (unchanged) -> sort by hit count (assert unique) -> fit reference model, identity alphabet, 3-byte header -> write table raw in rank order -> H2 identity rewrite for each raw model.
- Encoder (BC4 / BC5 / BC3): unchanged except H2. BC3 inherits both: H1 on the color half, H2 on both halves.
- Decoder (CPU / GPU): read model headers (raw with k bit 6 set => identity, no alphabet); load the reference model with `alphabet[i] = i`, require `table_size == reference count`; payload decode unchanged (marginally faster, one alphabet copy fewer).

## Payoff

- **H1**: -256 B per BC1 image -> +0.31% at 512^2, +0.19% at 768x512 / 512x768, +0.07% at 1024^2, **+0.176% suite average**.
- **H2**: -218 B per BC4 image, -220 B per BC5 -> +0.05% BC4, +0.02% BC5, ~0 BC1.
- Rejected (not worth a format path or GPU parse complexity, 0 to -20 B): shared color alphabets, packing, offsets, the 2 empty green headers.

Total: **~+0.18% BC1, +0.05% BC4, +0.02% BC5**. Small but unconditional, payload-neutral, decoder-cheaper. The larger ratio work (tile-layout search, local reference alphabet) is in the companion tile plan.
