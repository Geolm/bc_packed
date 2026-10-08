# BC packed

An asymmetric lossless compressor for BC1 textures, designed for fast GPU decompression.

The compressor works directly on a standard BC1 texture: 8 bytes per 4×4 block (two 16-bit 565 endpoint colors + one 32-bit word of 2-bit indices). At runtime, the compressed stream is uploaded to the GPU, decompressed with a compute shader, and written directly back to standard BC1 texture memory. The resulting textures can then be sampled normally.

The compressor is CPU-side and can use significantly more computation than the decompressor.

| Hardware |  Processor | Throughput                |
|:-----: | :------------: | ----- |
|      M5 Pro | CPU 18 cores | 2500 MiB/s      |
|      M5 Pro | GPU | 15300 MiB/s      |
|      M2 Max | CPU 12 cores | 1300 MiB/s      |
|      M2 Max | GPU | 11850 MiB/s      |

Average compression ratio for 74 images in test is **1:47:1**

Note: AI disclaimer, although everything is based on my ideas and my previous libraries, almost all the code was written by a LLM. It wouldn't have been possible with my fulltime job otherwise.

## Design goals

* Lossless.
* Input is a standard BC1 texture (8 bytes per 4×4 block: two 565 endpoints + one 32-bit index word); decompression reconstructs the same standard format byte-exact.
* GPU decompression only requires a compute shader.
* The texture is split into 64 independent strips, one GPU thread decodes each strip, with no dependency between threads.
* No CPU-side work is required at runtime.
* The decompressed result can be used directly as a normal BC1 texture.

## Compression

The techniques are adapted to make GPU decompression fast and simple:

* **Rice-Golomb coding** instead of arithmetic or Huffman coding. Rice-Golomb decoding is particularly well suited to GPU execution because it requires only simple integer operations and bit manipulation.
* **Static symbol ranking.** Symbols are remapped so that the most frequently used symbols have the lowest indices. The rank table is generated from a histogram during compression.
* **Parallel decompression.** The texture is divided into 64 independent strips. 64 GPU threads decompress the strips in parallel, with no dependency between threads.

Multiple independent Rice-Golomb models are used for the different data: endpoint color deltas (one set of red, green, blue per endpoint, red and blue each with one model per green-delta context bucket — 14 color models, see Endpoints), top-table references, difference masks, and residual difference bytes (17 models in total).

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

As the decompression happens on the GPU we need to decompress in parallel multiple streams. The texture is split in 64 strips, each thread decodes a strip of the image.

For example for a 1024x1024 texture : each thread will decode 256 blocks (in width) x 4 blocks (in height).

As the models are static, each thread just loads the same 17 models from the stream (`le_static_model_load`); the color context is derived from the current block's own decoded green delta, so there is no adaptive state to keep in sync between threads.

As Rice encoding output is non-fixed, we stored in the stream 63 offsets (uint16_t), each a delta in dword units relative to the previous strip, so each thread knows where to start (strip 0 always starts the strip data, its implicit zero offset is not stored).

### Endpoints

BC1 endpoints are predicted from the previous block's endpoints.
One set of three independent Rice-Golomb models is used per endpoint for the R, G, and B components; red and blue additionally carry one model per context bucket — 14 color models in total. Each (channel, endpoint, context) model is fitted to the color deltas that occur in its context, so every block is priced by a model that matches its own regime.

For each endpoint of each block:

```text
green_delta = current_green - previous_green
red_delta   = (current_red   - previous_red) - green_delta / 2
blue_delta  = (current_blue  - previous_blue) - green_delta / 2
```

BC1 colors are R5G6B5, so the deltas always fit in 7 bits. Each delta is stored as an unsigned value offset by 64, and encoded with its static Rice-Golomb model.

The red and blue deltas are contextual: the context is three value buckets of the endpoint's own 6-bit green delta (negative / zero / positive). The decorrelation removes the *linear* luminance component of the chroma deltas, but the residual chroma still scales with the local luminance energy, so a model fitted per luminance-delta regime prices each block tighter than a single image-wide model. The green deltas are encoded uncontexted. The context is derived from a value that is already decoded, so the model selection costs no stream bits.

The blocks are traversed in a zigzag order to avoid a discontinuity at the end of each scanline:

```C
uint32_t zigzag_x = (y & 1) ? x : width_blocks - x - 1;
```

This keeps the prediction direction continuous when moving from one scanline to the next. There is no previous block at the start of each strip, so the first block of each strip is predicted from the strip seed, a single endpoint color pair stored in the stream header (see Compressed stream).

### Per-strip predictor

Two predictors are available for the endpoint colors:

* **zigzag-previous** (the default): the previous block of the zigzag chain, the in-row spatial neighbor.
* **row-above**: the block of the row above at the *same zigzag position* — visually, the horizontally mirrored column of the previous row.

The compressor runs a pre-pass before the histogram and, for each strip, sums the absolute magnitude of the color deltas each predictor would emit (the first scanline is identical for both, the strip seed chain): the cheaper one wins and one bit per strip is stored in the stream header (see Compressed stream). The histogram and the encoder then use the chosen predictor per strip, so the static color models always fit the distribution that is actually emitted.

Textures whose features follow the zigzag columns (e.g. plank scans) make the row-above predictor much cheaper, while most other textures keep the zigzag-previous one. On the 74-image test suite this moves the average ratio 1.41 → 1.43 (colors payload −2.7%), with 58/74 images improving and the worst single image at −0.01% (the 8-byte header overhead on an image where no strip switches). For GPU decompression the row-above predictor also removes the serial zigzag chain: every block after the first scanline depends only on the row above, so a whole row decodes in parallel.

### Color-pair dictionary

A sliding dictionary of the last 256 endpoint-color *pairs* of the strip's zigzag chain (one ring buffer per strip, 32-bit entry = the two 565 colors packed). For each block of a strip with the dictionary enabled, one flag bit is written before the deltas:

* If the block's two endpoint colors exactly match one of the previous 256 blocks of the strip, the flag is 1 and a reference is written — the distance to the most recent match, minus one, in `ceil(log2(min(position, 256)))` bits (`position` is the block's 0-indexed position in the strip; 0 bits for the first block, 8 bits once the window is full). The six color-delta symbols are skipped: the decoder copies the two colors of the referenced, already-decoded block.
* Otherwise the flag is 0 and the six color-delta symbols are written as described in Endpoints.

The match is decided on the *decoded* colors only, so it is identical on the compressor and the decompressor and independent of the color predictor. A matched block still emits its top-table reference, mask, and difference bytes (the indices are not part of the color pair).

#### Per-strip enable flag

The flag bit is only as cheap as the matches it replaces, so each strip decides independently whether to use the dictionary: one bit per strip in the stream header (8 bytes, like the predictor flag). A strip pays one flag bit per block when enabled, so it is enabled exactly when the total savings of its matched blocks — the six color deltas they would emit minus the reference they pay — exceed that flag cost. The decision is made with the models fit on every block's deltas (a one-time approximation, stored in the header and read by both the encoder and the decoder, so it stays lossless).

Disabled strips write exactly the six color-delta symbols with no flag: byte-identical to the dictionary-less format, zero overhead. This is a two-pass model fit: the final color models are refit on the deltas that are actually emitted — every block of the disabled strips, the non-matched blocks of the enabled ones.

Measured on the 74-image test suite this turns the dictionary into a net win: average ratio 1.4587 → 1.4654, byte-weighted 1.4020 → 1.4073, with 44 images improving, 25 byte-identical, and 5 regressing at most 0.14% (8-byte header on near break-even strips). Structured content (wood, walls, planks) gains up to +2.95%; feature-rich content (grass, sand, rock) is left untouched. See `failed_experiments.md`, exp. 20.

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

## Compressed stream

Pseudo-description of the stream.

Note : the stream does not include width, height or format, only the compressed data. This is intended, it's up to the user to store that somewhere.

The output buffer must be at least `packed_bc1_maxsize(width, height)` bytes. This is a very rough bound: `(width/4) * (height/4) * 16`, i.e. 2× the standard BC1 texture size. In practice the stream is much smaller.

```text
* 17 static models, in order: for each endpoint (0, 1): red (3 context buckets), green, blue (3 context buckets) — 14 color models — then top-table reference, difference mask, table difference.
  Each is 3 + num_symbols bytes, no padding (max 259 bytes).
* Top table size: one uint8_t (number of entries − 1), padded to a 4-byte boundary.
* Strip seed: one uint32_t (little-endian) — the two endpoint colors of the "previous block" used to predict the first block of each strip, averaged over the 64 strips at compression time. Low 16 bits = color[0], high 16 bits = color[1].
* Strip predictor flags: 8 bytes (little-endian uint64_t) — one bit per strip: 1 → endpoint colors of that strip are predicted from the row above (same zigzag position) after the first scanline, 0 → zigzag-previous block. See Per-strip predictor.
* Strip dictionary enable flags: 8 bytes (little-endian uint64_t) — one bit per strip: 1 → the strip's blocks write the color-pair dictionary flag + reference per block, 0 → the six color deltas only (byte-identical to the dictionary-less format). See Per-strip enable flag.
* Top table entries, 4 bytes each (little-endian uint32_t).
* Padded to a 2-byte boundary.
* 63 strip offsets (uint16_t each), relative deltas in dword units. Strip 0 always starts the strip data so its offset is zero and not stored; the k-th stored offset (k = 1..63) is the delta from strip k-1 to strip k in dword units.
* Padded to a 4-byte boundary.
* 64 strip bitstreams, each 4-byte aligned, each flushed independently.
```

Within a strip, the blocks are traversed in the zigzag row order. For each block:

```text
* For a strip with the dictionary enabled: the color-pair dictionary flag (1 bit): 1 if the block's two endpoint
  colors exactly match a previous block of the strip, 0 otherwise.
  * If 1: a reference to the matching block — the distance minus one, in ceil(log2(min(position, 256))) bits,
    where position is the block's 0-indexed position in the strip (0 bits for the first block, 8 bits once 256
    blocks back are reachable). The six color-delta symbols are skipped.
  * If 0: the six color deltas, as below.
* For a strip with the dictionary disabled: no flag.
* Two endpoints, each: green, red, blue deltas (7-bit values offset by 64).
* Top-table reference (8-bit index).
* Difference mask (4 bits).
* One difference byte per set mask bit.
```

See Color-pair dictionary. The six color deltas, the top-table reference, the mask, and the difference bytes are all encoded with Rice-Golomb using one of the 17 static models; each red/blue uses the model of its (channel, endpoint, context) — the three-bucket green delta described in Endpoints — and each green uses its endpoint's uncontexted model. The dictionary flag and reference are raw bits, not Rice-Golomb symbols.

## Status

The stream matches the format documented above, and both `bcp_compress_bc1` and `bcp_decompress_bc1` are implemented. All 17 models are image-wide: one `k` (the Rice parameter, or the raw flag + width in raw mode, see Rice-Golomb model) and one `q_escape` per model, no per-strip model state. The per-block color context (the three-bucket green delta) selects among the red/blue models; the color predictor flag (8 bytes in the header, see Per-strip predictor) selects between two predictors, it does not change any model. The color-pair dictionary (see Color-pair dictionary) adds a 1-bit flag + a variable-width reference per block of the enabled strips (the per-strip enable word, 8 header bytes); it is a breaking change against the previous format — both the CPU and the GPU decoders must read the enable word, the flag, and the reference.

The 17-model header is a breaking change against the previous 9-model format (exp 19 in `failed_experiments.md`): the GPU decoder must load 17 models and select the red/blue model by context. Measured on the 74-image suite: +1.0% stream size (74/74 images improved, worst +0.19%) at neutral CPU decompression throughput (ground.png 1024², 18 threads, ≈3065 → ≈2970 MiB/s, inside run-to-run noise).

* Both `bcp_compress_bc1` and `bcp_decompress_bc1` require `width >= 16` and `height >= 256` (pixels, multiples of 4), i.e. at least 4x64 blocks: smaller textures are rejected (0 returned / false returned).
* CPU decompression (`bcp_decompress_bc1`) decodes one strip at a time; the unit tests decompress every strip and compare byte-exact against the input.
* Unit tests (`tests/`) cover synthetic textures, a real-image roundtrip suite over `images/` (loaded with `stb_image.h`, converted with `stb_dxt.h` from `third_party/`, which the library itself does not depend on), and a multithreaded decompression benchmark.
* Textures whose height in blocks is not a multiple of 64 (i.e. height not a multiple of 256) silently drop the trailing block rows.
* GPU decoding is not part of this tree; the compute-shader decoder described in Design goals is the intended consumer of this stream (the static Rice-Golomb models are what keep it cheap to port).

## Validation 

Everything must be validated on CPU, on multiple images

* Load a power-of-two image (third_party/stb_image.h)
* Compress the image into standard BC1 (third_party/stb_dxt.h); this is both the input and the reference texture
* bcp_compress_bc1; per-image stats (ratio, model parameters, payload bytes) are logged as a CSV to `logs/`
* Compute the compression ratio against the standard BC1 size (8 bytes per 4x4 block)
* bcp_decompress_bc1, compare the output, should be byte-exact with the input BC1
* Gather total compression ratio
* Proceed the next image


To be interesting the ratio should be higher than 1.4x; the current 74-image suite averages ~1.46x without the color-pair dictionary and ~1.455x with it (the dictionary is a net win on structured textures and a small loss on feature-rich ones; see `failed_experiments.md`).

Compression is expected to be a lot slower than decompression (the compressor scans the 256-entry color-pair dictionary of the previous blocks for every block).
