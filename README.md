# BC packed

[![Build Status](https://github.com/Geolm/bc_packed/actions/workflows/ci.yml/badge.svg)](https://github.com/geolm/bc_packed/actions)

An asymmetric lossless compressor for BC1, BC4 and BC5 textures, all three designed for fast GPU decompression (BC4 / BC5 reuse the BC1 machinery, see the BC4 / BC5 section).

The compressor works directly on a standard BC1 texture: 8 bytes per 4×4 block (two 16-bit 565 endpoint colors + one 32-bit word of 2-bit indices). At runtime, the compressed stream is uploaded to the GPU, decompressed with a compute shader, and written directly back to standard BC1 texture memory.

The compressor is CPU-side and can use significantly more computation than the decompressor.

| Hardware |  Processor | BC1 throughput                |
|:-----: | :------------: | ----- |
|      M5 Pro | CPU 18 cores | 2500 MiB/s      |
|      M5 Pro | GPU | 15300 MiB/s      |
|      M2 Max | CPU 12 cores | 1300 MiB/s      |
|      M2 Max | GPU | 11850 MiB/s      |
| AMD 5945WX | CPU 24 cores  | 1375 MiB/s |


| Block type |  Average compression from the image suite | 
|:-----: | :------------: 
| BC1 | 1.49:1  
| [BC4](logs/bc4_zip.md) | 1.33:1
| BC5 | 1.1:1  

All compression ratio are better than zlib.

Note: AI disclaimer, although everything is based on my ideas and my previous libraries, almost all the code was written by a LLM. It wouldn't have been possible with my fulltime job otherwise.

## Library

One c and one header, that's it, no dependency.

Check the API in the header [bc_packed.h](src/bc_packed.h) it's well documented.

[GPU decompression shader](src/bc_decompress.metal) is written in Metal Shader Language because I don't have a Windows computer at home. It should be easily translated in HLSL. 

The format is designed to allow decompression (CPU or GPU) on multiple threads.

**Note:** everything is unit tested, including the shader (only on macOS obviously)

## Design goals

* Lossless.
* Input is a standard BC1 texture (8 bytes per 4×4 block: two 565 endpoints + one 32-bit index word); decompression reconstructs the same standard format byte-exact.
* GPU decompression only requires a compute shader.
* The texture is split into 64 independent tiles, one GPU thread decodes each tile, with no dependency between threads.
* No CPU-side work is required at runtime.
* The decompressed result can be used directly as a normal BC1 texture.

## Compression

The techniques are adapted to make GPU decompression fast and simple:

* **Rice-Golomb coding** instead of arithmetic or Huffman coding. Rice-Golomb decoding is particularly well suited to GPU execution because it requires only simple integer operations and bit manipulation.
* **Static symbol ranking.** Symbols are remapped so that the most frequently used symbols have the lowest indices. The rank table is generated from a histogram during compression.
* **Parallel decompression.** The texture is divided into 64 independent tiles. 64 GPU threads decompress the tiles in parallel, with no dependency between threads.

Multiple independent Rice-Golomb models are used for the different data: endpoint color deltas (one set of red, green, blue per endpoint, red and blue each with one model per green-delta context bucket, and the second endpoint's green also contextual on the sign of the first endpoint's green — 16 color models, see Endpoints), top-table references, difference masks, and residual difference bytes (19 models in total).

### Rice-Golomb model

Each model contains a symbol count, a Rice parameter, a raw-byte escape threshold, and a rank table:

| Offset |  Size (bytes) | Description                |
| -----: | ------------: | -------------------------- |
|      0 |             1 | Number of symbols − 1      |
|      1 |             1 | Rice `k` value             |
|      2 |             1 | Raw-byte escape `q_escape` |
|      3 | `num_symbols` | Sorted rank table          |

The rank table contains only symbols that occur in the stream. Symbols are sorted by frequency, with the most frequently used symbols receiving the lowest indices (ties are broken by the lower symbol first).

The maximum model size is therefore:

```text
1 + 1 + 1 + 256 = 259 bytes
```

In practice, most models are smaller because symbols with a zero histogram count are discarded. No padding is written between models.

Each rank is written as `q = rank >> k` unary bits, a zero, then `k` remainder bits; a rank whose `q` reaches `q_escape` is written
as `q_escape` unary bits, a zero, then the full 8-bit rank as a raw byte. The escape caps the cost of the heavy tail of the
distribution, and the compressor searches `(k, q_escape)` jointly against the histogram to minimize the model's total bit count.
When the histogram has no heavy tail, the chosen `q_escape` sits beyond the largest rank that occurs and the escape never fires.

A near-uniform histogram makes Rice coding uneconomical. Such a model falls back to a raw fixed-width code: bit 7 of the `k` byte is set and its lower bits hold the code width, `ceil(log2(num_symbols))`, the number of bits written per symbol. The decoder mirrors this and reads that many bits back per symbol; `q_escape` is unused in this mode. A single-symbol model keeps the Rice path, as a 0-bit raw code would emit an empty payload.

The rank table, the best `k`, and the best `q_escape` are generated during the first compression pass from the symbol histogram.

### Two passes

As we use static models, we need two passes to compress the texture :
* first pass builds the top table, then collects histograms for all models, sorts the rank tables and chooses the best `k` and `q_escape`
* second pass encodes the texture in the stream

### Multiple streams

As the decompression happens on the GPU we need to decompress in parallel multiple streams. The texture is split in 64 tiles, each thread decodes a tile of the image.

For example for a 1024x1024 texture : each thread will decode 32 blocks (in width) x 8 blocks (in height) — the 256 x 256 block grid is split in 8 x 8 = 64 tiles.

The 64 tiles lay out the block grid in an even split: tile `s` is `(tx, ty) = (s % gx, s / gx)` with bounds `x0 = tx * width_blocks / gx, x1 = (tx + 1) * width_blocks / gx` and the same for `y`, where `gx` is 8 / 4 / 2 / 1 for `width_blocks` of 32+ / 16+ / 8+ / less and `gy = 64 / gx` (both derived from the width, no header byte, no API change). The even split covers every row and column even when the block grid is not a multiple of the tile size, so images whose height in blocks is not a multiple of the tile row count no longer lose their trailing rows.

As the models are static, each thread just loads the same 19 models from the stream (`le_static_model_load`); the color context is derived from the current block's own decoded green delta, so there is no adaptive state to keep in sync between threads.

As Rice encoding output is non-fixed, we stored in the stream 63 offsets (uint16_t), each a delta in dword units relative to the previous tile, so each thread knows where to start (tile 0 always starts the tile data, its implicit zero offset is not stored).

### Endpoints

BC1 endpoints are predicted from the previous block's endpoints.
One set of three independent Rice-Golomb models is used per endpoint for the R, G, and B components; red and blue each carry one model per context bucket, and the second endpoint's green does as well (keyed on the sign of the first endpoint's green delta) — 16 color models in total. Each (channel, endpoint, context) model is fitted to the color deltas that occur in its context, so every block is priced by a model that matches its own regime.

For each endpoint of each block:

```text
green_delta = current_green - previous_green
red_delta   = (current_red   - previous_red) - green_delta / 2
blue_delta  = (current_blue  - previous_blue) - green_delta / 2
```

BC1 colors are R5G6B5, so the deltas always fit in 7 bits. Each delta is stored as an unsigned value offset by 64, and encoded with its static Rice-Golomb model.

The red and blue deltas are contextual: the context is three value buckets of the endpoint's own 6-bit green delta (negative / zero / positive). The decorrelation removes the *linear* luminance component of the chroma deltas, but the residual chroma still scales with the local luminance energy, so a model fitted per luminance-delta regime prices each block tighter than a single image-wide model. The first endpoint's green delta is encoded uncontexted; the second endpoint's green delta is contextual on the sign of the first endpoint's green delta (negative / zero / positive). The context is derived from a value that is already decoded, so the model selection costs no stream bits.

The blocks of a tile are traversed in a zigzag order to avoid a discontinuity at the end of each scanline:

```C
uint32_t zigzag_x = tb.x0 + ((y & 1) ? x : tile_width - x - 1);
```

This keeps the prediction direction continuous when moving from one scanline to the next. There is no previous block at the start of each tile, so the first block of each tile is predicted from the tile seed, a single endpoint color pair stored in the stream header (see Compressed stream).

### Per-tile predictor

Two predictors are available for the endpoint colors:

* **zigzag-previous** (the default): the previous block of the zigzag chain, the in-row spatial neighbor.
* **row-above**: the block of the row above at the *same zigzag position* — visually, the horizontally mirrored column of the previous row.

The compressor runs a pre-pass before the histogram and, for each tile, sums the absolute magnitude of the color deltas each predictor would emit (the first scanline is identical for both, the tile seed chain): the cheaper one wins and one bit per tile is stored in the stream header (see Compressed stream). The histogram and the encoder then use the chosen predictor per tile, so the static color models always fit the distribution that is actually emitted.

Textures whose features follow the zigzag columns (e.g. plank scans) make the row-above predictor much cheaper, while most other textures keep the zigzag-previous one. On the 74-image test suite this moves the average ratio 1.41 → 1.43 (colors payload −2.7%), with 58/74 images improving and the worst single image at −0.01% (the 8-byte header overhead on an image where no tile switches). For GPU decompression the row-above predictor also removes the serial zigzag chain: every block after the first scanline depends only on the row above, so a whole row decodes in parallel.

### Color-pair dictionary

A sliding dictionary of the last 256 endpoint-color *pairs* of the tile's zigzag chain (one ring buffer per tile, 32-bit entry = the two 565 colors packed). For each block of a tile with the dictionary enabled, one flag bit is written before the deltas:

* If the block's two endpoint colors exactly match one of the previous 256 blocks of the tile, the flag is 1 and a reference is written — the distance to the most recent match, minus one, in `ceil(log2(min(position, 256)))` bits (`position` is the block's 0-indexed position in the tile; 0 bits for the first block, 8 bits once the window is full). The six color-delta symbols are skipped: the decoder copies the two colors of the referenced, already-decoded block.
* Otherwise the flag is 0 and the six color-delta symbols are written as described in Endpoints.

The match is decided on the *decoded* colors only, so it is identical on the compressor and the decompressor and independent of the color predictor. A matched block still emits its top-table reference, mask, and difference bytes (the indices are not part of the color pair).

#### Per-tile enable flag

The flag bit is only as cheap as the matches it replaces, so each tile decides independently whether to use the dictionary: one bit per tile in the stream header (8 bytes, like the predictor flag). A tile pays one flag bit per block when enabled, so it is enabled exactly when the total savings of its matched blocks — the six color deltas they would emit minus the reference they pay — exceed that flag cost. The decision is made with the models fit on every block's deltas (a one-time approximation, stored in the header and read by both the encoder and the decoder, so it stays lossless).

Disabled tiles write exactly the six color-delta symbols with no flag: byte-identical to the dictionary-less format, zero overhead. This is a two-pass model fit: the final color models are refit on the deltas that are actually emitted — every block of the disabled tiles, the non-matched blocks of the enabled ones.

Measured on the 74-image test suite this turns the dictionary into a net win: average ratio 1.4587 → 1.4654, byte-weighted 1.4020 → 1.4073, with 44 images improving, 25 byte-identical, and 5 regressing at most 0.14% (8-byte header on near break-even tiles). Structured content (wood, walls, planks) gains up to +2.95%; feature-rich content (grass, sand, rock) is left untouched. See `failed_experiments.md`, exp. 20.

### Indices

BC1 contains a 32-bit index field for each block.

#### Top table

* The compressor first scans the entire texture and builds a histogram of every unique 32-bit index pattern found.
* It selects the Top 256 most frequently occurring index patterns to form the top_table.
* The top table is refined using vector quantization. This algorithm implements Stochastic Bit-Level K-Means Clustering to optimize a BC1 VQ table. It uses Adaptive Jittered Sampling with error-feedback to efficiently assign image blocks to centroids based on Hamming Distance. Centroids are refined via Bitwise Majority Voting, flipping bits that differ in more than 50% of assigned blocks to mathematically minimize total bit-error across iterations; the first round and the two final rounds are full assignments over every block, the jittered middle rounds exploring a better local optimum and the full rounds converging the centroids on the whole image (see `failed_experiments.md`, exp. 15).
* Clusters holding at most one block in the final round are dropped, so the final table may contain fewer than 256 entries.
* An empty table (possible on tiny inputs where every cluster is dropped) is replaced by a single entry, the most frequent exact index pattern: the stream stores the table size as `entries − 1` in one byte, so zero entries would read back as 256 and corrupt the format.

#### Block indices

For each block, we find the closest top-table entry using Hamming distance:

```text
distance = popcount(block_indices XOR table_entry)
```

The selected top-table index (8 bits) is encoded using a Rice-Golomb model.
The XOR difference is then encoded as a sparse residual.
First, a 4-bit mask identifies which bytes of the difference are non-zero:

```C
uint32_t mask = 0;

if ((difference & 0x000000FF) != 0) mask |= 1;
if ((difference & 0x0000FF00) != 0) mask |= 2;
if ((difference & 0x00FF0000) != 0) mask |= 4;
if ((difference & 0xFF000000) != 0) mask |= 8;
```

The mask itself is encoded using a Rice-Golomb model.

For each set bit in the mask, the corresponding difference byte is encoded using a Rice-Golomb model.

This avoids storing zero bytes in the residual and makes the representation particularly compact when the block is close to one of the top-table patterns.

#### Flat blocks

A block whose two endpoint colors are identical carries a uniform index word: all 16 indices hold the same 2-bit value. A global flat index mode, decided once in the first pass, collapses this: mode 1 is selected when at least one block has identical endpoints and every such block holds the same 2-bit value. In mode 1 a flat block stores no index payload at all: the value, stored once for the whole image in the stream header, is expanded to all 16 indices on decode, instead of the top-table reference + mask + differences; in mode 0, or for a non-flat block, the indices are stored as usual. The mode and the value share one header byte (mode | value << 1) right after the top-table size. A decoder never needs the mode for a non-flat block, so the encoder and decoder always pick the same path; an encoder that leaves a non-uniform or a differing index word on a flat block forces mode 0 and round-trips byte-exact.

## Compressed stream

Pseudo-description of the stream.

Note : the stream does not include width, height or format, only the compressed data. This is intended, it's up to the user to store that somewhere.

The 64 tiles are the even split of the block grid (see Multiple streams): `gx` is 8 / 4 / 2 / 1 for `width_blocks` of 32+ / 16+ / 8+ / less, `gy = 64 / gx`, both derived from the width, so the stream layout itself never changes and images of any size (16×256 and up) use the same 64 streams.

The output buffer must be at least `packed_bc1_maxsize(width, height)` bytes. This is a very rough bound: `(width/4) * (height/4) * 16`, i.e. 2× the standard BC1 texture size. In practice the stream is much smaller.

```text
* 19 static models, in order: for each endpoint (0, 1): red (3 context buckets), green (3 context buckets), blue (3 context buckets) — 16 color models — then top-table reference, difference mask, table difference.
  Each is 3 + num_symbols bytes, no padding (max 259 bytes).
* Top table size: one uint8_t (number of entries − 1).
* Flat index mode + value: one uint8_t (mode | value << 1), then padded to a 4-byte boundary. See Block indices → Flat blocks.
* Tile seed: one uint32_t (little-endian) — the two endpoint colors of the "previous block" used to predict the first block of each tile, averaged over the 64 tiles at compression time. Low 16 bits = color[0], high 16 bits = color[1].
* Tile predictor flags: 8 bytes (little-endian uint64_t) — one bit per tile: 1 → endpoint colors of that tile are predicted from the row above (same zigzag position) after the first scanline, 0 → zigzag-previous block. See Per-tile predictor.
* Tile dictionary enable flags: 8 bytes (little-endian uint64_t) — one bit per tile: 1 → the tile's blocks write the color-pair dictionary flag + reference per block, 0 → the six color deltas only (byte-identical to the dictionary-less format). See Per-tile enable flag.
* Top table entries, 4 bytes each (little-endian uint32_t).
* Padded to a 2-byte boundary.
* 63 tile offsets (uint16_t each), relative deltas in dword units. Tile 0 always starts the tile data so its offset is zero and not stored; the k-th stored offset (k = 1..63) is the delta from tile k-1 to tile k in dword units.
* Padded to a 4-byte boundary.
* 64 tile bitstreams, each 4-byte aligned, each flushed independently.
```

Within a tile, the blocks are traversed in the zigzag row order. For each block:

```text
* For a tile with the dictionary enabled: the color-pair dictionary flag (1 bit): 1 if the block's two endpoint
  colors exactly match a previous block of the tile, 0 otherwise.
  * If 1: a reference to the matching block — the distance minus one, in ceil(log2(min(position, 256))) bits,
    where position is the block's 0-indexed position in the tile (0 bits for the first block, 8 bits once 256
    blocks back are reachable). The six color-delta symbols are skipped.
  * If 0: the six color deltas, as below.
* For a tile with the dictionary disabled: no flag.
* Two endpoints, each: green, red, blue deltas (7-bit values offset by 64).
* If the block is flat (its two endpoint colors are identical) and the flat index mode is 1:
  no index payload, the header value is expanded to all 16 indices on decode.
* Otherwise: top-table reference (8-bit index), difference mask (4 bits), and one difference byte per set mask bit.
```

See Color-pair dictionary. The six color deltas, the top-table reference, the mask, and the difference bytes are all encoded with Rice-Golomb using one of the 19 static models; each red/blue uses the model of its (channel, endpoint, context) — the three-bucket green delta described in Endpoints — and each green uses the model of its (endpoint, context), the first endpoint uncontexted and the second contextual on the sign of the first endpoint's green delta. The dictionary flag and reference are raw bits, not Rice-Golomb symbols.


=======
## BC4 / BC5

BC4 (one 8-bit channel) and BC5 (red + green channels) reuse the BC1 machinery: static Rice-Golomb models fitted from a histogram pre-pass, 64 independent tiles, and a self-describing byte stream. The entry points are `packed_bc4_maxsize` / `packed_bc5_maxsize`, `bcp_compress_bc4` / `bcp_compress_bc5`, and `bcp_decompress_bc4` / `bcp_decompress_bc5` (see `src/bc_packed.h`), with the same signature shape as their BC1 counterparts.

### Blocks

The standard D3D layout, consumed and reproduced byte-exact:

```c
// 8 bytes per 4x4 block
uint8_t endpoint[2]; // [0] = max endpoint, [1] = min endpoint (as stb writes them)
uint8_t indices[6];  // little-endian 48-bit field: the 16 x 3-bit indices in raster order, index 0 = top-left
```

A BC5 block is the red BC4 block followed by the green one (16 bytes). The library does not depend on stb; the unit tests build these blocks from images with `stb_compress_bc4_block` / `stb_compress_bc5_block` from `third_party/`.

### Endpoints

Each channel carries two 8-bit endpoints per block. The encoder predicts the current endpoint from the tile chain and stores the wrapped 8-bit delta (`curr - pred`, decoded as `pred + delta`, modulo 256) with one static 256-symbol Rice-Golomb model per (channel, endpoint).

The predictor is fixed — no per-tile flag, unlike the BC1 per-tile choice:

```text
first row of a tile:   pred = previous              (the tile seed for the tile's first block)
start of a row:        pred = top
other blocks:          pred = previous + top - top_previous   (mod 256)
```

`previous` is the chain-previous block in the BC1 zigzag order, `top` the block of the row above at the same column, `top_previous` the block above the chain-previous. All references stay inside the tile, so the 64 tiles remain independent. The tile seed is the per-endpoint average of the first block of each tile, stored in the stream header like the BC1 color seed.

#### Endpoint MTF-8

Optionally (a flag per channel, bit 4 of the flat-index header byte, decided in the first pass), the two endpoint deltas of a block are replaced by one symbol of a 9-symbol static model: an 8-entry move-to-front list of the last distinct (endpoint0, endpoint1) pairs of the tile's zigzag chain, cleared per tile. Rank 0..7 is the pair at that MTF position (the pair moves to the front); rank 8 is an escape that stores the two wrapped endpoint deltas exactly as above (the predictor is unchanged), and the new pair is inserted at the front of the list. The list is identical on the encoder and the decoder because every decoded pair is the same one, so the state stays in sync. The endpoint models are fitted on the escaped blocks only when the flag is set, on every block otherwise.

The first pass prices the endpoint histogram with and without MTF (payload bits plus the model headers each stores; a disabled channel writes a dummy 1-symbol MTF model, the stream layout is fixed and the flag is read after the models) and keeps the cheaper per channel — no heuristic. Textures with many repeated endpoint pairs (atlas-style content) switch on, natural textures usually keep the deltas.

### Indices

The 16 3-bit indices of a block are encoded in raster order:

```text
index 0:   the first index, encoded with a static 8-symbol model (one per channel),
            replacing the previous raw 3-bit field; the Rice / raw fallback of the
            fitter is the self-gate, so a near-uniform first index costs the same
            3 bits as raw and no flag is needed
index i:   symbol = prediction XOR index[i], encoded with a static 8-symbol Rice-Golomb model
           prediction = index[i-1]   (left)
                       index[i-4]    (directly above, for the rows at offset 4, 8, 12)
           context    = left * 8 + top, the 8 x 8 matrix of the decoded neighbor indices:
                       left = index[i-1] (a row start has no left neighbor),
                       top  = index[i-4] (the first row has no top neighbor),
                       the missing neighbor defaulting to 0
```

That is an 8 x 8 matrix = 64 index models plus one first-index model per channel. The context is derived from the already decoded indices themselves, so the model selection costs no stream bits.

The 8-symbol models use a compact rank table (8 entries instead of 256): `3 + num_symbols ≤ 11` bytes each in the stream.

#### Flat blocks

A block whose two endpoints are identical carries a uniform index word: all 16 indices hold the same 3-bit value. A per-channel flat index mode, decided once in the first pass, collapses this: mode 1 is selected for a channel when at least one block of that channel has identical endpoints and every such block holds the same 3-bit value, else mode 0. In mode 1 a flat block stores no index payload at all: the value, stored once per channel for the whole image in the stream header, is expanded to all 16 indices on decode, instead of the 15 contextual symbols; in mode 0, or for a non-flat block, the indices are stored as usual. The mode and the value share one header byte per channel (mode | value << 1) after the tile seeds. A decoder never needs the mode for a non-flat block, so the encoder and decoder always pick the same path; an encoder that leaves a non-uniform or a differing index word on a flat block forces mode 0 and round-trips byte-exact.

### Compressed stream

As BC1, the stream does not include width, height or format: the BC4 / BC5 decompressors are separate entry points and know their own header.

```text
* Per channel, in order:
  - 2 endpoint models — 256-symbol static models, 3 + num_symbols bytes each, no padding (max 259).
    Fitted on the MTF-escaped blocks when the channel's MTF-8 flag is set, on every block otherwise.
  - 1 endpoint MTF-8 model — 9-symbol static model, 3 + num_symbols bytes, no padding (max 12).
    A dummy 1-symbol model (4 bytes) when the flag is clear: the layout is fixed and the flag
    is only readable after the model region, so the slot is always present.
  - 1 first-index model — 8-symbol static model for the first index of each non-flat block,
    3 + num_symbols bytes, no padding (max 11).
  - 64 index models — 8-symbol static models, 3 + num_symbols bytes each, no padding (max 11).
* Tile seed: 2 bytes per channel (endpoint 0, endpoint 1).
* Flat index mode + value + endpoint MTF-8 enable: one uint8_t (mode | value << 1 | mtf << 4) per channel.
  See Indices → Flat blocks and Endpoints → Endpoint MTF-8.
* Padded to a 2-byte boundary.
* 63 tile offsets (uint16_t each), relative deltas in dword units. Tile 0 always starts the tile data; its offset is zero and not stored.
* Padded to a 4-byte boundary.
* 64 tile bitstreams, each 4-byte aligned, each flushed independently.
```

For BC5 the red channel's models and seed come first, then the green channel's, then the shared offsets and tiles. Within a tile the blocks follow the zigzag row order; per block the payload is the red channel complete, then the green one, and per channel: with MTF-8 enabled, the 9-symbol MTF rank of the block's endpoint pair (0..7 = the pair at that position of the per-tile move-to-front list, 8 = the two wrapped endpoint deltas, the list updated in either case and cleared per tile); without it, the two wrapped endpoint deltas; then either no index payload (a flat block in mode 1, the header value is expanded to all 16 indices on decode) or index 0 with the first-index model followed by indices 1..15 (each XORed with its intra-block prediction, as above).

### Status

* CPU + GPU: `bcp_compress_bc4/bc5` and `bcp_decompress_bc4/bc5` are implemented and tested; the GPU decoders are the `bcp_decompress_bc4` / `bcp_decompress_bc5` compute kernels of [src/bc_decompress.metal](src/bc_decompress.metal): one thread per tile, bit-identical to the CPU. Measured on the M5 Pro (1024×1024): **14540 MiB/s** for BC4, **7868 MiB/s** for BC5.
* `width >= 16` and `height >= 256` are required (multiples of 4, as BC1); the even tile split covers every row and column, so no image loses block rows.
* The output buffer must be at least `packed_bc4_maxsize` / `packed_bc5_maxsize` — 2× the standard block size (16 / 32 bytes per 4×4 block), the BC1 convention.

### Validation

Everything is validated byte-exact over all 64 tiles, on CPU and GPU:

* GPU roundtrips: the `bcp_decompress_bc4` / `bcp_decompress_bc5` Metal kernels decode the brick AO / normal maps (1024×1024, `images/bc4` / `images/bc5`) and a synthetic 256×256 texture whose endpoint pairs repeat within the tiles (the content class the real brick maps do not exercise), the result verified byte-identical to the stb-built / generated textures, the same way the BC1 kernel is tested.

* CPU: 12 synthetic 256×256 roundtrips: the flat / checkerboard / gradient / SplitMix32 random patterns as BC4 and BC5 (including the flat-index mode variants and a per-column predictor), plus the endpoint-pair content classes — repeated pairs, asserted to compress strictly smaller than an all-distinct-pairs control with the same index words, all-distinct pairs, a half-repeated / half-distinct mix, and a BC5 with repeated red / distinct green (per-channel independence). The tests check results only: byte-exact roundtrips and stream sizes, never the stream layout. The random pattern forces the raw-mode fallback and the full 64-context usage.
* CPU: 6 maps (`images/bc4`: 4 AO maps, a 4096×4096 roughness map, a 1748×1748 font atlas) and 4 normal maps (`images/bc5`), converted per block with stb_dxt: the per-image ratio is printed to stdout and logged to the hourly CSV, and the suite average must stay above 1.05. Measured: **1.3334:1** average for BC4 (1.0921 – 2.2396 per image, 2.2396 being the mostly-flat font atlas, where the endpoint MTF-8 flag switches on) and **1.0985:1** for BC5 (1.0629 – 1.1339 per image). BC5 compression ratio looks modest but zlib only achieves 1.010563:1 for the same images.

The 8-bit formats leave less headroom than BC1: the indices of these maps sit close to 3 near-uniform bits each, so most of the gain comes from the endpoint deltas.


=======
## BC3

BC3 combines a BC1 color block and a BC4 alpha block in the standard D3D layout, 16 bytes per 4×4 block. The entry points are `packed_bc3_maxsize`, `bcp_compress_bc3`, and `bcp_decompress_bc3` (see `src/bc_packed.h`), with the same signature shape as their BC1 counterparts.

### Scheme

BC3 is the composition of the two pipelines above, applied to the two halves of the block grid: the BC1 pipeline on the color half, the single-channel BC4 pipeline on the alpha half. No payload logic, model, or context state is new: the tile decomposition, the zigzag chains, the BC1 predictors / top table / color-pair dictionary, and the BC4 endpoint / index models are all the existing ones, fitted on their own half as for the standalone formats.

### Compressed stream

Self-describing, as the other streams (no width, height, or format):

```text
* u32 little-endian: the size in bytes of the BC1 substream that follows.
* The BC1 stream of the color half, exactly the [Compressed stream](#compressed-stream) format.
* The BC4 stream of the alpha half, exactly the [BC4 / BC5 → Compressed stream](#compressed-stream-1) format with 1 channel.
```

Each substream is self-contained (its own models, tile seed, 63 tile offsets, 64 tiles) and ends 4-byte aligned, so the two can be decoded independently; the 64 tiles of both halves share the same decomposition, so one `bcp_decompress_bc3` call decodes one tile of both.

### Status

* CPU only: `bcp_compress_bc3` / `bcp_decompress_bc3` are implemented and tested; BC3 is niche, so there is no Metal decoder (the BC1 and BC4 kernels of [src/bc_decompress.metal](src/bc_decompress.metal) cover the two halves, a `bcp_decompress_bc3` kernel is a possible follow-up).
* `width >= 16` and `height >= 256` are required (multiples of 4, as BC1); the even tile split covers every row and column, so no image loses block rows.
* The output buffer must be at least `packed_bc3_maxsize` — 2× the standard BC3 block size (32 bytes per 4×4 block), the BC1 convention, plus the 4-byte prefix.

### Validation

Everything is validated byte-exact over all 64 tiles on CPU:

* 4 synthetic 256×256 roundtrips (flat, checkerboard, gradient, SplitMix32 random): the color half and the alpha half each use the corresponding BC1 / BC4 pattern, so the random pattern forces the raw-mode fallback and the full model usage in both halves at once.
* 2 albedo + alpha maps (`images/bc3`), 1024×1024, converted per block with stb_dxt: the per-image ratio is printed to stdout (no CSV) and the suite average must stay above 1:1. Measured: **2.8787:1** average (3.2210:1 for leak-stain_albedo, 2.5364:1 for leaves). The ratio is dominated by the BC1 color half, which keeps the BC1 headroom; the 8-bit alpha half compresses like a standalone BC4.

## Images 

The images used in the unit test come from this sources:

https://opengameart.org/content/tiny-texture-pack-2

https://r0k.us/graphics/kodak/

The `images/bc3` set (albedo + alpha maps) is used by the BC3 suite; the leak-stain and leaves sources are the project author's own assets.
