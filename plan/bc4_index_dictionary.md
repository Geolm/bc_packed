# Plan: BC4/BC5 per-tile index-word dictionary (MTF, exact match)

Status: design approved by the user, no code written.

Target: `font_atlas.png`, the only BC4 content class where bc_packed trails `zip -9` (596,663 B vs 682,144 B).
65 % of its non-flat blocks share their 48-bit index word with another block, which is what this change exploits.

## 1. Mechanism

- The unit is the 48-bit **index word** (the 6 index bytes of a block; the endpoints are coded as today).
- Per tile and per channel, a **255-entry move-to-front list** of exact index words, cleared per tile
  (a GPU thread decodes one tile, so no cross-tile state is possible).
- Hit: emit the MTF rank, the whole index payload (first index + 15 contextual symbols) is skipped, and the
  entry moves to the front. Miss: emit the escape, then the usual payload, insert at the front, evict the tail.
- Exact match only: no residual, no near match, no transmitted table, no seed list.

## 2. Coding

- One symbol of a 256-symbol static model per block that emits an index payload:
  `rank 0..254` = dictionary entry, `255` = escape. The alphabet is full, so the Rice / raw fallback of
  the fitter is a plain 8-bit symbol with no special case.
- **No per-block flag**: the escape *is* the flag. Measured 3.5-5 KB cheaper than a separate 2-symbol flag
  model plus a rank model (a 2-symbol static Rice model is exactly 1 bit/symbol and never goes below).
- Applies to every block that emits an index payload, so flat blocks in mode 0 participate too; flat blocks
  in mode 1 keep their free payload and never touch the list.

## 3. Enabling (gating)

- One plain `uint64_t` per channel, always written after the per-channel header bytes, as a contiguous block
  (the header bytes of every channel, then the tile word of every channel — not interleaved): bit `i` = tile
  `i` uses its dictionary. Same shape as BC1's `strip_dict_enable`, no flag, no conditional field. 8 bytes per
  channel (16 for BC5), and 0 payload cost for an image where the gate disables every tile.
- The rank model slot is always in the model region, like the endpoint MTF-8 slot: a channel whose gate
  disables every tile writes a dummy 1-symbol model (4 bytes, the decoder never reads from it), so a
  non-qualifying channel pays exactly the 8-byte tile word + 4-byte dummy — never the full 259-byte model.
- Gate decision, per tile and channel, with both sides priced by the models the stream actually carries:
  - The pass-2 simulation (§4) accumulates the global per-channel rank histogram (256 bins); the
    end-of-pass-2 fit builds `bc4_dict_rank_model[c]` from it, next to the endpoint / index models.
  - A dedicated pricing pass (the third read of the image; it cannot merge into pass 2 because the index
    models are only fitted once that pass is done) re-simulates the dictionary per tile and prices each
    tile: `saved` = payload bits of the tile's hit blocks (first index + 15 contextual symbols) under the
    fitted index models; `paid` = the tile's own 256-bin rank histogram (a reused temp, discarded at the
    tile end) priced under the fitted rank model, escape bin included. Enable when `saved > paid`.
  - No estimate constants: the gate is priced with exactly the models that get written (the written rank
    model is the pass-2 global fit; a channel with no enabled tile writes the dummy and no rank symbol
    exists), so the simulator's exact per-tile pricing only cross-checks the expected size (all 6 BC4
    images, 64 tiles each, agree).

## 4. Implementation

- Dictionary state, new type next to `mtf_list`: `bc4_dict` = `uint8_t word[255][6]` (fixed slots) +
  `uint8_t order[255]` (recency permutation, `order[i]` = slot of rank `i`) + `uint8_t count`. A hit copies
  6 bytes and shifts the permutation only (`memmove` of at most 255 B); the words never move, so a slot is
  stable for the lifetime of the tile.
- Encoder lookup reuses BC1's hashmap: same `hashmap_entry` buffer (`ctx->hashmap`), same `hash32`, and the
  linear-probe loop factored into one shared inline helper used by both `build_top_table` and the dictionary
  (no second table implementation). The dictionary stores `key = hash32(word_lo) ^ hash32(word_hi)` and
  `count = slot + 1`; the 6 bytes are compared before a hit is accepted, so a 32-bit hash collision only
  costs a probe. Slot to rank is a scan of `order`, 1-byte compares, at most 255 B.
- `bcp_context` gains `hashmap_size` and a `hashmap_ensure(ctx, size)` helper: BC1 asks for `HASHMAP_SIZE`,
  the BC4/BC5 dictionary for `BC4_DICT_MAP_SIZE` (1024 entries = 8 KB, load factor at most 0.25 against 255
  live entries), and an existing buffer is reused when it is already large enough. The table is cleared per
  tile (8 KB) and an evicted word is removed with backward-shift deletion (~12 lines), so the map always
  holds exactly the live dictionary.
- Encoder plumbing: the dictionary state lives in `bcp_compress_bc45` next to `mtf_list mtf[...]`; the
  pass-2 simulation in `bc4_init_models` (dictionary cleared per tile, same zigzag walk as the histogram)
  fills the global `dict` rank histogram of `bc4_hists` (256 bins per channel) and the end-of-pass-2 fit
  builds `bc4_dict_rank_model[c]` from it; the pricing pass (third read of the image) fills
  `bc4_dict_enable[c]` (one `uint64_t` per channel) and afterwards a channel whose gate disabled every
  tile has its model replaced by the dummy 1-symbol one.
- Decoder: same state next to the endpoint MTF list in `bcp_decompress_bc45`, no map: `order[rank]` gives the
  slot, the hit is a 6-byte copy plus the permutation shift.
- Stream: the rank model slot goes right after the first-index model of each channel, the tile word after the
  per-channel header bytes (contiguous block, see §3). `packed_bc4_maxsize` grows by one channel's worth,
  `8 + 3 + 256` = 267 bytes; `packed_bc5_maxsize` already doubles it, which covers both channels.

## 5. Expected results (simulator, current encoder)

Replica baseline 683,223 B against the real 682,144 B; the BC4 suite has 6 images.

| font_atlas | bytes | ratio | change |
|---|---:|---:|---:|
| current encoder | 682,144 | 2.2396 | |
| with the dictionary, K=255 | ~634,900 | 2.406 | -48.3 KB, -7.1 % |

- The other five BC4 images keep byte-identical tile data: the gate disables all 64 tiles on the four 1K AO
  maps and on the 4K roughness map (measured 0-17 hits over 65k blocks, so 0 B of tile payload), leaving only
  the 8-byte tile word + 4-byte dummy rank model = 12 bytes per channel as overhead.
- For reference, `zip -9` is 596,663 B (2.5605) on this image; the dictionary alone lands at 2.406.

## 6. Gates

- Round-trip CPU + GPU on the BC4 and BC5 suites, byte-exact over all 64 tiles (GPU kernel mirrors the list).
- Synthetic: a periodic-pattern BC4/BC5 image (dictionary must engage and round-trip), an all-distinct-index
  image (must stay gated off and round-trip), a flat-mode-0 image with repeated flat words.
- ASan build clean; GPU validation layers clean.
- CPU decode speed within 5 % of the current 14540 MiB/s BC4 / 7868 MiB/s BC5 (one `memmove` per hit).
- Encode time within 15 %: the image is read three times (seed; histogram + dictionary simulation; gate
  pricing) with an O(1) lookup; the only new buffers are the 8 KB map inside the existing hashmap
  allocation and the reused 1 KB pricing temp, both stay in cache.
- GPU: per-tile state in the Metal kernel is 1.79 KB/channel (3.6 KB for BC5) of thread-local memory. If the
  standalone GPU profile shows a regression, the fallback is a per-tile dictionary scratch in device memory,
  which needs a new buffer in the GPU API: ask before doing it.
- `logs/bc4_zip.md` re-baselined, README BC4/BC5 Indices + Compressed stream updated.

## 7. Design decisions and version

- Rejected for this mechanism: seed table, near-match residuals, static per-image table, whole-block
  dictionary, separate per-block flag, K > 255, transpose and half-promotion update policies.
- Version: 2.1.0 (new feature). The stream layout changes — breaking for a 2.0.0 stream — but v2 has never
  been merged to main and is being prepared for main, so the change stays on the 2.x line and takes the
  minor bump, not a major one.
