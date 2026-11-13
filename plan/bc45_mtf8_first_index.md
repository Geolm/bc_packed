# Plan: BC4/BC5 endpoint MTF-8 + first-index model

Status: proposal, no code written.

## 1. Endpoint MTF-8 (flag per texture for BC4, per channel for BC5)

- Keep an 8-entry move-to-front list of the last distinct (e0, e1) pairs of the tile chain, cleared per tile.
- Per block: one symbol of a new 9-symbol model, rank 0..7 = pair at that MTF position, 8 = escape to the
  current endpoint deltas (predictor unchanged). Endpoint models are fitted on escaped blocks only.
- Flag: bit 4 of the existing per-channel flat-index byte (`mode | value << 1 | mtf << 4`), 0 header bytes.
- Decision: price the endpoint histogram pass with and without MTF, keep the cheaper. No heuristic.

## 2. First-index model

- Replace the raw 3-bit first index of each non-flat block with one 8-symbol static model per channel.
- Self-gating: the model falls back to raw 3-bit mode when modeling doesn't pay off, so no flag.
- Atlas: 52,998 → 36,941 B (**−16 KB, −2% stream**). Natural textures: +11 B (model header), red brick AO −256 B.

## 3. Gates

- Round-trip CPU + GPU (Metal kernel mirrors both), synthetic tests for MTF on/off and escape-heavy tiles.
- Decode speed should be the same +- 5% noise variance
- Stream format change: should ship with the other format changes in one major version
