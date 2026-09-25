# BC1 packed

An asymmetric lossless compressor for already-compressed BC1 textures, designed for full GPU decompression.

At runtime, the compressed stream is uploaded to the GPU, decompressed with a compute shader, and written directly to BC1 texture memory. The resulting textures can then be sampled normally.

The compressor is CPU-side and can use significantly more computation than the decompressor.

## Design goals

* Lossless.
* Input and output are standard BC1 textures.
* GPU decompression only requires a compute shader.
* Blocks can be decompressed independently.
* No CPU-side work is required at runtime.
* The decompressed result can be used directly as a normal BC1 texture.

## Compression

Several ideas are borrowed from [bc_crunch](https://github.com/Geolm/bc_crunch), but some techniques are adapted to make GPU decompression fast and simple:

* **Rice-Golomb coding** instead of arithmetic or Huffman coding. Rice-Golomb decoding is particularly well suited to GPU execution because it requires only simple integer operations and bit manipulation.
* **Static symbol ranking.** Symbols are remapped so that the most frequently used symbols have the lowest indices. The rank table is generated from a histogram during compression.
* **Parallel decompression.** The texture is divided into independent blocks. 64 GPU threads decompress blocks in parallel, with no dependency between threads.

Multiple independent Rice-Golomb models are used for different data streams, such as endpoint components, index patterns, masks, and residual bytes.

### Rice-Golomb model

Each model contains a symbol count, a Rice parameter, and a rank table:

| Offset |  Size (bytes) | Description                |
| -----: | ------------: | -------------------------- |
|      0 |             1 | Number of symbols, max 256 |
|      1 |             1 | Rice `k` value             |
|      2 | `num_symbols` | Sorted rank table          |

The rank table contains only symbols that occur in the stream. Symbols are sorted by frequency, with the most frequently used symbols receiving the lowest indices.

Since the GPU operates most efficiently on 32-bit values, each model is padded to a 4-byte boundary with zeroes.

The maximum model size is therefore:

```text
2 + 256 + padding = 260 bytes
```

In practice, most models are smaller because symbols with a zero histogram count are discarded.

The rank table is generated during the first compression pass from the symbol histogram.

### Endpoints

BC1 endpoints are predicted from the previous block's endpoints.

Three independent Rice-Golomb models are used for the R, G, and B components.

For each block:

```text
green_delta = current_green - previous_green
red_delta   = (current_red   - previous_red) - green_delta / 2
blue_delta  = (current_blue  - previous_blue) - green_delta / 2
```

The deltas are then encoded using their respective Rice-Golomb models.

The blocks are traversed in a zigzag order to avoid a discontinuity at the end of each scanline:

```C
uint32_t zigzag_x = (y & 1) ? x : width - x - 1;
```

This keeps the prediction direction continuous when moving from one scanline to the next.

## Indices

BC1 contains a 32-bit index field for each block.

### Top table

A top table is built from the most frequently occurring index patterns, sorted by usage frequency.

A vector-quantization pass further refines the entries of the top table to better represent the index patterns found in the texture.

### Block indices

For each block, we find the closest top-table entry using Hamming distance:

```text
distance = popcount(block_indices XOR table_entry)
```

The selected top-table index is encoded using a Rice-Golomb model.

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
