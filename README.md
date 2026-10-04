# BC1 packed

An asymmetric lossless compressor for BC1 textures, designed for fast GPU decompression.

The compressor works on a *packed* representation of BC1: 8 bytes per 4×4 block (two 16-bit 565 endpoint colors + one 32-bit word of 2-bit indices), half the size of the standard 16-byte BC1 block. At runtime, the compressed stream is uploaded to the GPU, decompressed with a compute shader, and written directly to standard BC1 texture memory. The resulting textures can then be sampled normally.

The compressor is CPU-side and can use significantly more computation than the decompressor.

## Design goals

* Lossless.
* Input is a packed BC1 texture (8 bytes per 4×4 block: two 565 endpoints + one 32-bit index word); Decompression output is also a standard BC1 texture (8 bytes per block).
* GPU decompression only requires a compute shader.
* The texture is split into 64 independent strips, one GPU thread decodes each strip, with no dependency between threads.
* No CPU-side work is required at runtime.
* The decompressed result can be used directly as a normal BC1 texture.

## Compression

The techniques are adapted to make GPU decompression fast and simple:

* **Rice-Golomb coding** instead of arithmetic or Huffman coding. Rice-Golomb decoding is particularly well suited to GPU execution because it requires only simple integer operations and bit manipulation.
* **Static symbol ranking.** Symbols are remapped so that the most frequently used symbols have the lowest indices. The rank table is generated from a histogram during compression.
* **Parallel decompression.** The texture is divided into 64 independent strips. 64 GPU threads decompress the strips in parallel, with no dependency between threads.

Multiple independent Rice-Golomb models are used for the different data: endpoint color deltas (one set of red, green, blue per endpoint), top-table references, difference masks, and residual difference bytes.

### Rice-Golomb model

Each model contains a symbol count, a Rice parameter, and a rank table:

| Offset |  Size (bytes) | Description                |
| -----: | ------------: | -------------------------- |
|      0 |             1 | Number of symbols − 1      |
|      1 |             1 | Rice `k` value             |
|      2 | `num_symbols` | Sorted rank table          |

The rank table contains only symbols that occur in the stream. Symbols are sorted by frequency, with the most frequently used symbols receiving the lowest indices (ties are broken by the lower symbol first).

The maximum model size is therefore:

```text
1 + 1 + 256 = 258 bytes
```

In practice, most models are smaller because symbols with a zero histogram count are discarded. No padding is written between models.

The rank table and the best `k` are generated during the first compression pass from the symbol histogram.

### Two passes

As we use static models, we need two passes to compress the texture :
* first pass builds the top table, then collects histograms for all models, sorts the rank tables and chooses the best `k`
* second pass encodes the texture in the stream

### Multiple streams

As the decompression happens on the GPU we need to decompress in parallel multiple streams. The texture is split in 64 strips, each thread decodes a strip of the image.

For example for a 1024x1024 texture : each thread will decode 256 blocks (in width) x 4 blocks (in height).

As the models are static, each thread just loads the same nine models from the stream (`le_static_model_load`); there is no adaptive state to keep in sync between threads.

As Rice encoding output is non-fixed, we stored in the stream 64 offsets (uint16_t), each a delta in dword units relative to the previous strip, so each thread knows where to start.

### Endpoints

BC1 endpoints are predicted from the previous block's endpoints.
One set of three independent Rice-Golomb models is used per endpoint for the R, G, and B components (six models in total), so each endpoint adapts its own Rice parameter and rank table.

For each endpoint of each block:

```text
green_delta = current_green - previous_green
red_delta   = (current_red   - previous_red) - green_delta / 2
blue_delta  = (current_blue  - previous_blue) - green_delta / 2
```

BC1 colors are R5G6B5, so the deltas always fit in 7 bits. Each delta is stored as an unsigned value offset by 64, and encoded with its respective static Rice-Golomb model.

The blocks are traversed in a zigzag order to avoid a discontinuity at the end of each scanline:

```C
uint32_t zigzag_x = (y & 1) ? x : width_blocks - x - 1;
```

This keeps the prediction direction continuous when moving from one scanline to the next. There is no previous block at the start of each strip, so the first block of each strip is predicted from the strip seed, a single endpoint color pair stored in the stream header (see Compressed stream).

### Indices

BC1 contains a 32-bit index field for each block.

#### Top table

* The compressor first scans the entire texture and builds a histogram of every unique 32-bit index pattern found.
* It selects the Top 256 most frequently occurring index patterns to form the top_table.
* The top table is refined using vector quantization. This algorithm implements Stochastic Bit-Level K-Means Clustering to optimize a BC1 VQ table. It uses Adaptive Jittered Sampling with error-feedback to efficiently assign image blocks to centroids based on Hamming Distance. Centroids are refined via Bitwise Majority Voting, flipping bits that differ in more than 50% of assigned blocks to mathematically minimize total bit-error across iterations.
* Clusters that end up empty are dropped, so the final table may contain fewer than 256 entries.
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

The output buffer must be at least `bc1_packed_maxsize(width, height)` bytes. This is a very rough bound: `(width/4) * (height/4) * 16`, i.e. the size of the standard raw BC1 texture (2× the packed input). In practice the stream is much smaller.

```text
* 9 static models, in order: endpoint 0 red, endpoint 0 green, endpoint 0 blue, endpoint 1 red, endpoint 1 green, endpoint 1 blue, top-table reference, difference mask, table difference.
  Each is 2 + num_symbols bytes, no padding (max 258 bytes).
* Top table size: one uint8_t (number of entries − 1), padded to a 4-byte boundary.
* Strip seed: one uint32_t (little-endian) — the two endpoint colors of the "previous block" used to predict the first block of each strip, averaged over the 64 strips at compression time. Low 16 bits = color[0], high 16 bits = color[1].
* Top table entries, 4 bytes each (little-endian uint32_t).
* Padded to a 2-byte boundary.
* 64 strip offsets (uint16_t each), relative deltas in dword units.
* Padded to a 4-byte boundary.
* 64 strip bitstreams, each 4-byte aligned, each flushed independently.
```

Within a strip, the blocks are traversed in the zigzag row order. For each block:

```text
* Two endpoints, each: green, red, blue deltas (7-bit values offset by 64).
* Top-table reference (8-bit index).
* Difference mask (4 bits).
* One difference byte per set mask bit.
```

All symbols are encoded with Rice-Golomb using one of the nine static models.

## Status

The stream matches the format documented above, and both `bc1_packed_compress` and `bc1_packed_decompress` are implemented. All nine models are image-wide: one Rice `k` per model, no per-strip model state (a per-strip `k` for the mask model was prototyped and measured +0.03% — not adopted, the gain did not justify the added stream bytes and decoder state).

* CPU decompression (`bc1_packed_decompress`) decodes one strip at a time; the unit tests decompress every strip and compare byte-exact against the input.
* Unit tests (`tests/`) cover synthetic textures, a real-image roundtrip suite over `images/` (loaded with `stb_image.h`, converted with `stb_dxt.h` from `third_party/`, which the library itself does not depend on), and a multithreaded decompression benchmark.
* Textures whose height in blocks is not a multiple of 64 (i.e. height not a multiple of 256) silently drop the trailing block rows.
* GPU decoding is not part of this tree; the compute-shader decoder described in Design goals is the intended consumer of this stream (the static Rice-Golomb models are what keep it cheap to port).

## Validation 

Everything must be validated on CPU, on multiple images

* Load a power-of-two image (third_party/stb_image.h)
* Compress the image into standard BC1 (third_party/stb_dxt.h); this is the reference texture
* Pack it into packed BC1 (8 bytes per 4×4 block)
* bc1_packed_compress, store the output stream. 
* Compute the compression ratio against the standard BC1 size
* bc1_packed_decompress, compare the output, should be byte-exact with the packed input
* Gather total compression ratio
* Proceed the next image


To be interesting the ratio should be higher than 1.4x.

Compression is expected to be a lot slower than decompression.
