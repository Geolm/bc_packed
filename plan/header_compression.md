# Plan: header compression

Status: proposal, no code written into the library. **Revised 2026-11-10 after measuring the actual
header on the 74-image suite.** The measurement removed most of the original plan; the scope is now
two small, measured, payload-neutral items.
Scope: BC1 reference-model alphabet (H1) and raw-mode model alphabets (H2, BC4/BC5/BC3). Nothing else
in the header changes.
Baseline: 74-image BC1 suite **1.4907** (median stream 83.7 KB at 512^2, 132.8 KB at 768x512, 377.5
KB at 1024^2), BC4 **1.1352**, BC5 **1.0985**, BC3 **3.9180**
([stats_2026-11-08_18_04.csv](../logs/stats_2026-11-08_18_04.csv)).

Companion plan: [tile_layout_and_local_reference.md](tile_layout_and_local_reference.md). Both
change the stream format; ship them in one major version (see section 9). This plan is independent of
the tile search and can be developed first.

---

## 1. Measured header inventory (this replaces the earlier estimates)

Measured by instrumenting a throwaway copy of `src/bc_packed.c` (`save_static_model` + the BC1/BC4/BC5
headers) and running the full suite; **no library source was modified** (see section 10 for the
method).

Per BC1 image, 74 images:

| section | measured | notes |
|---|---:|---|
| 18 color model alphabets | **360 B** | mean per context: red 34.6, green 22 (2 of 3 endpoint-0 contexts empty), blue 34, each x endpoint 0/1; green endpoint 1 is the outlier at 158 B |
| reference model alphabet | **256 B** | `num_symbols` = 253-256, median 256; the table has 252-256 entries and every one is hit |
| difference model alphabet | **243 B** | 242.7 mean, up to 255 present symbols; never raw |
| mask model alphabet | **16 B** | always all 16 symbols present |
| 21 model headers (`count, k, q`) | **63 B** | 18 color + reference + mask + difference |
| **models total** | **937 B** | range 551-1299 |
| top table | **1008-1024 B** | always 252-256 entries x 4 B |
| fixed fields + offsets + per-tile padding | **~272 B** | flat/size/seed/predictor/dict 22, 63 offsets 126, tile flush/align ~124 |
| **header total** | **~2.2 KB** | 2.7% of a 512^2 stream, 1.7% at 768x512, 0.6% at 1024^2 |

Consequences that decide this plan:

- **The top table (≈1 KB) is the biggest single item, and there is nothing to win there.** It is 4 B
  x 252-256 entries with **0 duplicate patterns and 0 zero-hit entries** across all 74 images, so
  there is nothing to prune, and reordering it to make it compressible costs more than it saves
  (section 4.3). It stays raw.
- **The reference alphabet is 256 B on every image and is the one item that can be deleted outright**
  (H1). 11.5% of the header, zero payload change.
- **Raw-mode models always store a full alphabet that nobody needs** (H2): every raw model on the
  suite has all of its symbols present (BC4: 109 raw index models x 8.0 B, BC5: 110 x 8.0 B). That is
  exactly the case where the alphabet is derivable.
- Everything else is small: ~360 B of color alphabets, ~20 B of packing, ~30 B of offsets/flags, 6 B
  of empty green contexts. Each is priced in section 6 and dropped - below ~0.05% each.
- The headline saving is worth **+0.31% ratio at 512^2, +0.19% at 768x512 / 512x768, +0.07% at
  1024^2, a suite average of +0.176%** (1.4907 -> ~1.4933).

---

## 2. Objectives and acceptance

**Objective:** cut the measured **256 B** out of every BC1 header (H1) and the raw-model alphabets out
of BC4/BC5 (H2), with byte-identical payloads, no new modes, and no GPU decode cost. Leave the rest of
the header alone.

**Acceptance:**

- H1: header **-256 B** on every BC1 image; reference/mask/difference/color payload bytes
  **identical** to the baseline; suite average **>= +0.15%**; 512^2 class >= +0.25%.
- H2: raw-mode models carry no alphabet; payload bit-for-bit identical (the fixed-width field is
  order-independent); **-218 B per BC4 image, -220 B per BC5 image**, ~-16 B on BC1.
- CPU decode within 2%, GPU decode within noise (both should be marginally faster: fewer header bytes
  to parse, one alphabet copy fewer).
- All tests, ASan, GPU validation clean; deterministic streams.
- Encoder time: one extra sort of a 256-entry table; no meaningful multiplier.

**Hard stop:** if H1 does not reproduce -256 B with byte-identical payloads, or the suite gain is
below +0.10%, record the measurement in `failed_experiments.md` and do not ship the format change.

---

## 3. Design principles

1. **Measured, then shipped.** Every item is priced with the section 1 numbers; anything under ~50 B
   of header is not in the plan (section 6).
2. **Derive what can be derived.** The reference alphabet follows from the table order; a raw model's
   alphabet follows from its symbol space. Neither needs to be transmitted.
3. **One convention, no modes.** Rank order is *the* table order; raw mode is identity-ordered. No
   order flags, no delta codecs, no encoder search, no per-group gates. One deterministic pass, same
   stream shape on every image.
4. **The GPU path stays a flat parse.** The decoder loads a fixed sequence of models with known types
   and sizes. Nothing in this plan adds a self-describing section, a per-group flag, or per-tile work:
   both items only *remove* bytes from the header.
5. **No public API change.** Stream format version bump only.

---

## 4. H1 - rank-ordered top table + identity reference alphabet (the main win)

### 4.1 Observation

The reference model header today is `[count-1][k][q_escape][alphabet: count bytes]`, and the
alphabet is the table-index frequency order. The table itself is emitted in VQ cluster order. If the
**table is stored in that same hit-count order**, the reference alphabet is the identity `0..count-1`
and does not need to be transmitted, while the reference payload cost is unchanged (the encoded rank
of the n-th most frequent centroid is n either way).

Measured: the reference alphabet is 253-256 B on every image (every entry is hit), so this is a flat
**-256 B** with no payload movement - 11.5% of the header.

This is not a re-litigation of exp 4/5 (reference delta / hit-count sort with delta): the payload is
not delta-coded and the rank sequence is unchanged. Exp 4 showed reordering the raw reference is a
wash, which is exactly what makes the 256 B free.

### 4.2 What it requires

- **Sort the table by reference hit count**, descending, ties by pattern value ascending (the
  existing `nearest32` tie-break already prefers the largest entry value; sorting by it keeps pattern
  selection order-independent).
- **Assert uniqueness instead of deduplicating.** Measured: 0 duplicate patterns among 252-256
  entries on the whole suite, so dedup has no yield. Keep a debug-build assert (and, if a future VQ
  change produces duplicates, fall back to the current behavior) rather than a dedup pass with a
  format consequence.
- **Prune is not needed either.** Measured: 0 zero-hit entries. If a stream ever has
  `table_size != reference count`, that is an invariant violation, so the decoder rejects it.
- **Reference model header becomes 3 bytes**: `count-1`, `k`, `q_escape`, no alphabet, no flag. The
  decoder sets `alphabet[i] = i` and `index[i] = i` for `i < count`.
- **Table bytes are written unchanged**: `4 * top_table_size`, raw, just in rank order. No mode bits,
  no size field, no chain.

### 4.3 Why the table itself stays raw (decision record, 2026-11-10)

An earlier draft carried table delta coding (XOR mask / numeric ascending + subtraction) plus a
three-order search. Dropped, and now priced with the measurement:

- **The coupling kills it.** Only a rank-ordered table can use the identity alphabet for free. Any
  other order must store the frequency permutation (256 B - exactly what H1 removes) or pay a
  reference payload penalty. The delta code would have to beat 256 B *plus* that penalty.
- **The table is only 256 entries over a large index space**, so sorted gaps average ~2^32/256 (~24
  bits/entry, ~600-765 B for 255 deltas) and the per-byte delta models come out of that too. The
  realistic net was marginal, on top of a new bit-reader and chained reconstruction on CPU and GPU.
- **Table size is already at its floor** (252-256 entries, all used), so there is no prune/dedup
  upside to combine with a codec.

If the table is ever revisited, the honest target is "keep rank order and shrink 4 B/entry", not
"re-order the table so it compresses".

### 4.4 Validation

H1 must be payload-neutral. Compare per-component byte counts against the baseline: reference, mask,
difference, and color payload bytes must be identical; the table byte *multiset* must be identical
(only the order changes); the header must shrink by exactly 256 B. If the reference payload moves,
the sort/tie-break is wrong (the exp 13 caution: a measurement that scrambles the pipeline looks like
a win).

---

## 5. H2 - omit the alphabet in raw mode (identity ranks)

### 5.1 Observation

Raw mode writes the **rank** (`model->index[value]`) as a fixed-width field of
`ceil(log2(num_symbols))` bits, and the decoder maps it back through `alphabet[index]`. The alphabet
is only needed because the rank order is the frequency order - and in raw mode the field width does
not depend on the order at all. So a raw model can be **re-ordered to the identity** for free:

- `alphabet[i] = i`, `index[s] = s` keeps the payload bit count identical (fixed width, order
  independent) and makes the alphabet reconstructible, so it can be omitted from the header.
- This is only valid when the present symbol set is exactly `{0..num_symbols-1}` (a prefix of the
  symbol space); otherwise `index[s] = s` would encode a symbol the field cannot represent. The
  encoder checks with a <=256-entry scan and falls back to writing the alphabet when the set is not a
  prefix.
- The decoder needs one bit to know: **bit 6 of the `k` byte, raw mode only** (bit 7 is the raw flag,
  bits 0-3 the width; bits 4-5 stay free).

Header-only change: no payload bits move, no per-block work, and the decoder builds the identity model
without copying an alphabet. On the reference model this is the same trick H1 applies unconditionally
for Rice mode.

### 5.2 Measured payoff

From the section 1 dump, **every raw model on the suite has a full alphabet**, so the rule applies to
all of them:

| stream | raw models | alphabet each | saving per image | share of stream |
|---|---:|---:|---:|---:|
| BC4 1024^2 | 27.25 of 64 per image | 8 B | ~218 B | ~0.048% |
| BC5 1024^2 | 27.5 of 128 per image | 8 B | ~220 B | ~0.023% |
| BC1 | 0.01 per image (one raw mask model in 1 of 74) | 16 B | ~0 B (16 B on that image) | - |

If H1 does not ship, this also frees the 256 B reference alphabet on the 14 of 74 images where the
reference model is raw; with H1 those models are identity anyway.

### 5.3 Implementation notes

- `load_static_model` currently clamps `k & 0x7f` to `LE_MAX_K`, which would **destroy a bit-6 flag**.
  Extract the flag first and clamp only the width field (`k & 0x0f`).
- Force the identity in `le_static_model_init` right after the raw decision. Nothing else reads a raw
  model's rank order: `le_symbol_cost` is width-only in raw mode, and the decision that picked raw mode
  is a cost comparison that does not change.
- GPU: skip the alphabet read and build the identity model (`alphabet[i] = i`), ~10 lines in the BC1
  and BC4/BC5 model loaders.
- `packed_bc*_maxsize` bounds only shrink; no constant change is needed beyond the format bump.
- The rule is uniform across model types (8-symbol index, 16-symbol mask, 256-symbol
  endpoint/reference): no per-type special case.

### 5.4 Tests

- `header_raw_identity_alphabet`: near-uniform BC4/BC5 index models go raw -> no alphabet in the
  stream; round-trip CPU + GPU; the stream is `alphabet bytes x raw models` smaller than the baseline
  and the payload bytes are unchanged.
- `header_raw_alphabet_fallback`: a raw model whose present set has a gap (e.g. symbols `{0, 2, 3}`)
  -> the flag is clear, the alphabet is written, round-trip.
- `header_raw_width`: raw models at the 2/4/8-symbol width boundaries, and a 256-symbol raw model
  (width 8) if one can be constructed.

---

## 6. Measured and rejected (do not re-propose without new numbers)

| item | measured ceiling | verdict |
|---|---:|---|
| Shared color alphabets (union alphabet per red/green/blue group + per-bucket `k/q`, `header_flags` preamble, per-group gate) | union == largest bucket: 184 B/image; realistic ~110 B | **dropped (2026-11-10).** ~0.04% suite average for a self-describing header section, a per-group payload gate, and a bigger test matrix. The GPU parse would have to branch per group; not worth it for 110 B |
| Drop the 2 empty endpoint-0 green context headers | 6 B, and it needs a structural proof that those contexts can never carry symbols (true on the suite, not obviously true for synthetic inputs) | **dropped.** Fix the README's "19 models" wording instead (the stream legitimately carries 21 headers: 18 color = 16 used + 2 empty dummies, + reference + mask + difference) |
| Raw-mode **bitmap/list alphabet codes** (selector in the raw `q_escape` byte, ascending-symbol lists) | same bytes as H2, but only for the models whose present set is not a prefix | **dropped as a codec.** Every measured raw model has a full alphabet, so the identity rule (H2) covers all real cases with one flag bit |
| Model header packing (bit-packed `count/k/q`) | 18-20 B on BC1 (18 color models), ~128 B per BC4/BC5 channel | **dropped.** 0.02% at 512^2, invisible in the suite average |
| Offsets packing / flag RLE / bit-packed tile streams | 24-35 B (offsets), 5-10 B (flags), 60-120 B (tile bit packing, needs bit-level tile starts) | **dropped.** Below the noise floor of the tile plan |
| Table delta coding (XOR / numeric subtraction / similarity order) | negative net (section 4.3) | **dropped** |
| Reference alphabet + table order | **256 B** | **shipped (H1)** |
| Raw-mode alphabet omission via identity ranks | BC4 ~218 B, BC5 ~220 B | **shipped (H2)** |

The lesson worth recording: **the header is mostly one table and one 256-byte alphabet**. After H1
and H2 there is no cheap 0.5-1 KB left - the earlier draft's "-0.8 to -1.3 KB" was built on a table
codec and a shared-alphabet scheme that do not survive their own complexity.

---

## 7. Encoder / decoder summary

Encoder (BC1):

```text
build_top_table()                                  // unchanged VQ
final reference pass                               // unchanged
sort table by hit count desc, ties by pattern value ascending (assert unique)
fit the reference model in that order, identity alphabet, 3-byte header
write the table raw in that order
each raw model: force identity ranks, set k bit 6, omit the alphabet  (H2)
```

Encoder (BC4 / BC5 / BC3): unchanged except the raw-model identity rewrite (H2).

Decoder (CPU / GPU):

```text
read the 21 (BC1) / 130 or 260 (BC4/BC5) model headers; raw models with k bit 6 are identity, no alphabet
read top_table_size, flat mode, table (raw), offsets
load the reference model with alphabet[i] = i, and require table_size == reference count
tile payload decode is unchanged (H1/H2 only remove header bytes, so decode is marginally faster)
```

BC3 inherits both: the BC1 color half gets H1, both halves get H2.

---

## 8. Stages

### Stage 1 - H1 rank order + identity reference (the deliverable)

- Sort the table by hit count, assert uniqueness, fit the reference model in table order with the
  identity alphabet, write the 3-byte reference header.
- Validate: per-component payload bytes identical to baseline, header exactly -256 B, `top_table` byte
  multiset identical.
- Tests: tied hit counts (deterministic tie-break), 1-entry table, all-flat image (zero reference
  symbols), GPU round-trip, a hand-built stream with `table_size != reference count` rejected.

### Stage 2 - H2 raw-mode identity alphabet (small; same CL)

- Force identity ranks when raw mode is chosen and the present set is a prefix; set `k` bit 6; skip
  the alphabet on write/read (CPU + GPU); fix the `load_static_model` clamp so the flag survives.
- Validate: payload bytes identical to baseline on BC4/BC5/BC3; stream smaller by exactly
  `8 B x raw index models`; identity and fallback paths both round-trip; ASan clean.

### Stage 3 - cleanup / docs / version

- README: header layout, rank-order/identity convention, the model-header count (21 for BC1:
  16 used color contexts + 2 empty + reference + mask + difference).
- `bc_packed.h`: version bump (see section 9); no API change.
- `failed_experiments.md`: entry with the section 1 measurement, the H1/H2 results, and the rejected
  items of section 6 (shared alphabets, table delta, packing, offsets) so they stay closed.
- `logs/`: new stats CSV.

---

## 9. Versioning and interaction with the tile plan

- H1 and H2 are breaking stream-format changes (H2 adds a `k` meaning and removes alphabet bytes, so
  old and new streams cannot be mixed). Ship them with the tile plan under **one** major bump (2.0.0),
  no API change.
- **Bit budget:** the BC1 flat byte keeps bit 0 (flat mode) and bits 1-2 (flat value).
  **Bits 3-7 are free** (verified: the writer emits `bc1_index_mode | (bc1_index_value << 1)`, the
  reader masks only those bits; BC4/BC5 per-channel bytes use bits 0-3 and leave 4-7 free). Enough for
  the tile plan's `layout_log2` (bits 3-5) and `ref_local_enabled` (bit 6), so that plan's separate
  `layout u8` is unnecessary - zero net header growth, and BC5 loses its +2 B.
- H1 makes the tile plan's local reference alphabet simpler: the global alphabet is `0..N-1` in rank
  order, the per-tile promoted list is a list of table indices, and the local tail is `0..N-1` minus
  the promoted set.
- H2 and the tile plan do not interact: BC4/BC5 have no local reference alphabet, and BC1's raw
  models are (measured) nonexistent apart from one mask model.

---

## 10. Measurement method (for reproducibility)

- Copy `src/bc_packed.c` to a throwaway file; add a `printf` in `save_static_model` (image name,
  sequence, `num_symbols`, raw flag) and one after the BC1 model block (`stream.pos`,
  `top_table_size`, duplicate-entry count, zero-hit count).
- Build a standalone harness that mirrors `tests/image_tests.c` (stb_image + stb_dxt, `-DSTBI_ONLY_PNG`),
  compress all 74 BC1 images plus the 4 BC4 / 4 BC5 sets, and dump the model lines.
- Aggregate per model role: counts, raw share, alphabet bytes. Everything in section 1 comes from that
  dump; no library file was edited and nothing was committed.

If this measurement is repeated, keep the instrumented copy out of the build (`CMakeLists.txt` lists
`src/bc_packed.c` explicitly) and do not leave the print in the source tree.

---

## 11. Risks and mitigations

| risk | mitigation |
|---|---|
| Rank order changes pattern selection through the tie-break, so the payload moves | Ties already prefer the largest entry value; sorting by hit count keeps that tie-break meaningful. Validate per-component bytes against the baseline; treat any payload movement as a bug, not a win |
| A duplicate or zero-hit entry appears (synthetic images, future VQ change) | Measured 0 on the suite; keep a debug assert and a defined fallback. The decoder requires `table_size == reference count`, so a mismatched stream fails loudly rather than decoding wrong |
| Reference identity mis-parsed (rank vs symbol) | Tied-count test plus the identity mapping test; GPU mirror test |
| H2 forces identity on a raw model whose present set is not a prefix -> payload corruption | Prefix check before forcing; fallback writes the alphabet as today; test with a gapped histogram |
| H2 flag destroyed by the `load_static_model` width clamp | Extract bit 6 before clamping; clamp only `k & 0x0f`; test a raw model with the flag |
| GPU parse slows down or diverges from the CPU | Both items only remove header bytes and one alphabet copy; keep the fixed parse order (no new self-describing sections), mirror the flag handling, GPU validation layers + byte-exact GPU round-trips |
| Format churn / two bumps | Ship H1 + H2 with the tile plan as 2.0.0 |
| Maxsize bound | The header only shrinks; the existing bound is safe; test with a worst-case random image |

---

## 12. Test plan

1. `header_reference_identity`: tied hit counts -> the reference header is the 3-byte identity form,
   round-trip byte-exact, reference payload bytes identical to a stored-alphabet build.
2. `header_raw_identity_alphabet`, `header_raw_alphabet_fallback`, `header_raw_width` (H2, see 5.4).
3. `header_table_rank_order`: the transmitted table is in non-increasing hit-count order (ties by
   pattern value); the table byte multiset equals the VQ table's; `alphabet[i] == i`.
4. `header_table_floor`: 1-entry table, all-flat image (zero reference symbols), and a corrupt stream
   with `table_size != reference count` (rejected).
5. `header_model_count`: the stream carries 21 model headers on BC1 (130 for BC4, 260 for BC5) and the
   model boundaries match the declared counts.

Regression: full suite (74 BC1, 4 BC4, 4 BC5, 2 BC3) + GPU round-trips; ASan clean; GPU validation
clean; CPU benchmark within 2%; deterministic streams; per-component payload comparison against the
baseline.

---

## 13. Files expected to change

| file | change |
|---|---|
| `src/bc_packed.c` | rank sort + identity reference model, (H2) raw-mode identity/flag + loader clamp, CPU decoder |
| `src/bc_packed.h` | version bump only |
| `src/bc_decompress.metal` | identity reference model, (H2) raw-mode identity/flag |
| `README.md` | header layout, rank-order/identity convention, model-header count |
| `tests/synthetic_tests.c` | the new header tests |
| `tests/gpu_tests.cpp` | GPU round-trips for the new paths |
| `tests/image_tests.c` | suite thresholds / stats |
| `failed_experiments.md` | entry with the measurement and the rejected items |
| `plan/header_compression.md` | this plan |

---

## 14. Estimated payoff

- **H1: -256 B per BC1 image, payload-neutral** -> +0.31% at 512^2, +0.19% at 768x512 / 512x768,
  +0.07% at 1024^2, **+0.176% suite average**. Zero decode cost (one alphabet copy fewer).
- **H2: -218 B per BC4 image, -220 B per BC5 image, payload-neutral** -> +0.05% BC4, +0.02% BC5, ~0 on
  BC1. ~20 lines of encoder/decoder plus a flag bit: the cheapest bytes in the plan after H1.
- **Rejected items (section 6): 0 to -20 B** on BC1 (shared color alphabets, packing, offsets, the 2
  empty green headers), none of them worth a format path or GPU parse complexity.

Total realistic header win: **~+0.18%** on the BC1 suite plus +0.05% BC4 / +0.02% BC5. Small, but
unconditional, payload-neutral, decoder-cheaper, and it rides along with the v2.0.0 format change the
tile plan already requires. The larger ratio work (tile-layout search, local reference alphabet) is in
the companion plan.
