# BC4 — zip -9 baseline on raw standard BC4

General-purpose compressor baseline for the bc_packed BC4 stream ratios. Measured 2026-11-12
on the six-image `images/bc4` suite (v1.0.2).

**Method:** raw blocks built with stb_dxt v1.13 (`stb_compress_dxt_block`'s BC4 path, red
channel) exactly as the image suite builds them; `zip -9` per file; compressed size read from
the deflate entry (`unzip -v`), no container overhead. Ratio = raw bytes / compressed bytes —
the same numerator as the project's stream ratios, so the columns are directly comparable.

| Image | Size | Raw bytes | zip -9 bytes | zip ratio | bc_packed ratio | bc_packed stream bytes |
|---|---|---:|---:|---:|---:|---:|
| grey_roof_tiles_02_ao_1k.png | 1024×1024 | 524,288 | 504,935 | 1.0383 | 1.1592 | 452,272 |
| patterned_cobblestone_ao_1k.png | 1024×1024 | 524,288 | 516,041 | 1.0160 | 1.0921 | 480,080 |
| red_brick_ao_1k.png | 1024×1024 | 524,288 | 495,479 | 1.0581 | 1.1544 | 454,180 |
| rough_wood_ao_1k.png | 1024×1024 | 524,288 | 516,923 | 1.0142 | 1.1352 | 461,856 |
| Metal002_4K_Roughness.png | 4096×4096 | 8,388,608 | 8,031,124 | 1.0445 | 1.2195 | 6,878,792 |
| font_atlas.png | 1748×1748 | 1,527,752 | 596,663 | 2.5605 | 1.9432 | 786,208 |
| **average (per image)** | | | | **1.2886** | **1.2839** | |
| **byte-weighted** | | 12,013,512 | 10,661,165 | **1.1268** | **1.2628** | 9,513,388 |

## Notes

- bc_packed beats zip -9 on the five real-content maps by +7.5% to +16.8% (byte-weighted
  +12.1% overall). AO / roughness content is ~99.9% unique 8-byte blocks (red_brick:
  65,442 / 65,536 distinct; the 4K roughness map: 1,047,560 / 1,048,576), which leaves
  deflate no LZ material — only weak statistical compression.
- zip -9 wins on the font atlas, 2.5605 vs 1.9432: a small palette (black + a couple of AA
  grays) makes 57.5% of the blocks duplicates (81,252 of 190,969 distinct), scattered in
  raster order (adjacent-run RLE alone only reaches 1.04:1). Deflate's 32 KB window matches
  the repeated blocks for a few bits each, while bc_packed's per-block models (endpoint
  predictor + static endpoint/index models, no block-level deduplication in the BC4 path —
  BC1's top table has no BC4 equivalent yet) price every duplicate in full.
- The atlas is a small share of the raw bytes (12.7%), so the flip shows up only in the
  simple per-image average (the metric the image suite reports): zip ahead by 0.4%. The
  README's "all compression ratios are better than zlib" claim is currently false for the
  BC4 suite average; it still holds for the original four AO maps, BC1, and BC5, and for
  BC4 on a byte-weighted basis.
- Fight-back target: block-level deduplication for BC4 (a per-image-gated block dictionary
  along the lines of BC1's top table) would close the atlas-class gap while gating off on
  real content.
