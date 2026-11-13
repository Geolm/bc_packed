# BC4 : bc_packed vs zip -9 baseline on raw standard BC4

General-purpose compressor baseline for the bc_packed BC4 stream ratios. Measured 2026-11-12
on the six-image `images/bc4` suite (v1.0.2); the bc_packed column re-measured 2026-11-13 on
v2.0.0 (endpoint MTF-8 + first-index model), and again 2026-11-13 on v2.1.0 (index-word
dictionary), which switches on for the font atlas and stays off for the five real-content maps.

**Method:** raw blocks built with stb_dxt v1.13 (`stb_compress_dxt_block`'s BC4 path, red
channel) exactly as the image suite builds them; `zip -9` per file; compressed size read from
the deflate entry (`unzip -v`), no container overhead. Ratio = raw bytes / compressed bytes —
the same numerator as the project's stream ratios, so the columns are directly comparable.

| Image | Size | Raw bytes | zip -9 bytes | zip ratio | bc_packed ratio | bc_packed stream bytes |
|---|---|---:|---:|---:|---:|---:|
| grey_roof_tiles_02_ao_1k.png | 1024×1024 | 524,288 | 504,935 | 1.0383 | 1.1592 | 452,300 |
| patterned_cobblestone_ao_1k.png | 1024×1024 | 524,288 | 516,041 | 1.0160 | 1.0920 | 480,104 |
| red_brick_ao_1k.png | 1024×1024 | 524,288 | 495,479 | 1.0581 | 1.1550 | 453,944 |
| rough_wood_ao_1k.png | 1024×1024 | 524,288 | 516,923 | 1.0142 | 1.1351 | 461,880 |
| Metal002_4K_Roughness.png | 4096×4096 | 8,388,608 | 8,031,124 | 1.0445 | 1.2195 | 6,878,820 |
| font_atlas.png | 1748×1748 | 1,527,752 | 596,663 | 2.5605 | 2.4040 | 635,504 |
| **average (per image)** | | | | **1.2886** | **1.3608** | |
| **byte-weighted** | | 12,013,512 | 10,661,165 | **1.1268** | **1.2831** | 9,362,552 |

## Notes

- v2.1.0 stream deltas: the five real-content maps gain only the always-present dictionary
  slot (4-byte dummy rank model + 8-byte tile word, +12 B each; red_brick +16 B — its model
  region lands 1 byte off the 4-byte alignment of the tile word), the font atlas drops
  46,640 B.
- bc_packed beats zip -9 on the five real-content maps by +7.5% to +16.8% (byte-weighted
  +13.9% across all six maps). AO / roughness content is ~99.9% unique 8-byte blocks
  (red_brick: 65,442 / 65,536 distinct; the 4K roughness map: 1,047,560 / 1,048,576), which
  leaves deflate no LZ material — only weak statistical compression. MTF-8 and the
  index-word dictionary stay off for all five (the exact per-tile pricing keeps the plain
  payload); their streams moved by header bytes only.
- zip -9 still wins on the font atlas, 2.5605 vs 2.4040: the v2.1.0 index-word dictionary
  (enabled on the atlas tiles, one global rank model per channel) cut the gap from zip +14.3%
  (v2.0.0) to +6.5%. The atlas's 57.5% byte-identical duplicate blocks (81,252 of 190,969
  distinct, scattered in raster order) now cost a rank hit instead of a full index payload;
  what remains is the escape payload of the unique words plus the endpoint stream, where
  deflate's 32 KB window still reaches farther than a per-tile list of 255 words.
- The atlas is a small share of the raw bytes (12.7%), so its zip advantage no longer flips
  the simple per-image average (the metric the image suite reports): bc_packed ahead by
  +5.6% (1.3608 vs 1.2886), up from +3.5% before the dictionary. The README's "all
  compression ratios are better than zlib" claim holds for the BC4 suite average, as for the
  original four AO maps, BC1, BC5, and BC4 on a byte-weighted basis.
