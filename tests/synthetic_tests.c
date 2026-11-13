#include "test_helpers.h"


#define TEST_IMAGE_SIZE (65536)
#define TEST_IMAGE_WIDTH (256)
#define TEST_IMAGE_HEIGHT (256)
#define TINY_IMAGE_BLOCKS (256) // 4 x 64 blocks, the minimum supported image

const uint16_t red   = 0xF800;
const uint16_t cyan  = 0x07FF;

//-----------------------------------------------------------------------------------------------------------------------------
// helper function to validate roundtrip: the compressed stream fits the documented maxsize
// and decompressing all strips reproduces the input byte-exact
TEST roundtrip(const bc1_block* bc1_image, uint32_t width, uint32_t height)
{
    const size_t num_blocks = (width/4) * (height/4);
    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = packed_bc1_maxsize(width,  height);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    uint32_t num_strips = 0;
    size_t compressed_buffer_length = bcp_compress_bc1(ctx, bc1_image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
    ASSERT(compressed_buffer_length != 0);
    ASSERT(compressed_buffer_length <= compressed_buffer_size);

    bc1_block* decompressed_bc1_image = malloc(num_blocks * sizeof(bc1_block));
    ASSERT(decompressed_bc1_image != NULL);

    for(uint32_t i=0; i<num_strips; ++i)
    {
        ASSERT(bcp_decompress_bc1(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed_bc1_image, i));
    }

    for(uint32_t i=0; i<num_blocks; ++i)
    {
        ASSERT_EQ(decompressed_bc1_image[i].color[0], bc1_image[i].color[0]);
        ASSERT_EQ(decompressed_bc1_image[i].color[1], bc1_image[i].color[1]);
        ASSERT_EQ(decompressed_bc1_image[i].indices, bc1_image[i].indices);
    }

    bcp_terminate(ctx);

    free(compressed_buffer);
    free(decompressed_bc1_image);

    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
// one solid color
TEST flat(void)
{
    bc1_block image[TEST_IMAGE_SIZE];

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = 0x33333333
        };
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Alternating checkerboard pattern across block grid
TEST checkerboard(void)
{
    bc1_block image[TEST_IMAGE_SIZE];
    uint32_t blocks_per_row = TEST_IMAGE_WIDTH / 4;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / blocks_per_row;

        if ((bx + by) % 2 == 0)
        {
            image[i] = (bc1_block)
            {
                .color = {red, cyan},
                .indices = 0x33333333
            };
        }
        else
        {
            image[i] = (bc1_block)
            {
                .color = {cyan, red},
                .indices = 0xCCCCCCCC
            };
        }
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Smooth gradient across RGB565 color values and index masks
TEST gradient(void)
{
    bc1_block image[TEST_IMAGE_SIZE];
    uint32_t blocks_per_row = TEST_IMAGE_WIDTH / 4;
    uint32_t num_rows = TEST_IMAGE_HEIGHT / 4;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / num_rows;

        uint16_t c0 = (uint16_t)(((bx * 31) / blocks_per_row) << 11) | (uint16_t)(((by * 63) / num_rows) << 5);
        uint16_t c1 = (uint16_t)(((by * 31) / num_rows) << 11) | (uint16_t)((((blocks_per_row - 1 - bx) * 31) / blocks_per_row));
        uint32_t indices = (bx * 0x11111111u) ^ (by * 0x22222222u);

        image[i] = (bc1_block)
        {
            .color = {c0, c1},
            .indices = indices
        };
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Pseudo-random block generation using SplitMix32
TEST random_splitmix32(void)
{
    bc1_block image[TEST_IMAGE_SIZE];
    uint32_t rng_state = 0x85EBCA6Bu;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint32_t r1 = splitmix32(&rng_state);
        uint32_t r2 = splitmix32(&rng_state);

        image[i] = (bc1_block)
        {
            .color = {(uint16_t)(r1 & 0xFFFF), (uint16_t)(r1 >> 16)},
            .indices = r2
        };
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Blocks where color[0] <= color[1] (enables BC1 1-bit alpha mode)
TEST color0_le_color1(void)
{
    bc1_block image[TEST_IMAGE_SIZE];

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c0 = (uint16_t)(i & 0x7FFF);
        uint16_t c1 = (uint16_t)(c0 + 1); // Guarantees color[0] <= color[1]

        image[i] = (bc1_block)
        {
            .color = {c0, c1},
            .indices = 0xE4E4E4E4
        };
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Identical endpoints (color[0] == color[1])
TEST identical_colors(void)
{
    bc1_block image[TEST_IMAGE_SIZE];

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c = (uint16_t)(i * 0x1F);
        image[i] = (bc1_block)
        {
            .color = {c, c},
            .indices = 0x00000000
        };
    }

    return roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Identical endpoints all sharing the same non-zero uniform index word: the whole-image
// flat-index value (mode 1, value 2) stored in the stream header
TEST identical_colors_uniform_value(void)
{
    bc1_block* image = malloc(TEST_IMAGE_SIZE * sizeof(bc1_block));
    if (!image)
        return GREATEST_TEST_RES_FAIL;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c = (uint16_t)(i * 0x1F);
        image[i] = (bc1_block)
        {
            .color = {c, c},
            .indices = 0x22222222
        };
    }

    greatest_test_res res = roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
    free(image);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Identical endpoints with DIVERGING uniform index words (values 0 and 1 on alternating blocks):
// no single value for the whole image, the flat blocks fall back to the full index encoding (mode 0)
TEST identical_colors_divergent(void)
{
    bc1_block* image = malloc(TEST_IMAGE_SIZE * sizeof(bc1_block));
    if (!image)
        return GREATEST_TEST_RES_FAIL;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c = (uint16_t)(i * 0x1F);
        image[i] = (bc1_block)
        {
            .color = {c, c},
            .indices = (i & 1) ? 0x11111111 : 0x00000000
        };
    }

    greatest_test_res res = roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
    free(image);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Identical endpoints with a non-uniform garbage index word on every block: forces mode 0
TEST identical_colors_garbage(void)
{
    bc1_block* image = malloc(TEST_IMAGE_SIZE * sizeof(bc1_block));
    if (!image)
        return GREATEST_TEST_RES_FAIL;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c = (uint16_t)(i * 0x1F);
        image[i] = (bc1_block)
        {
            .color = {c, c},
            .indices = 0xA5A53C3C
        };
    }

    greatest_test_res res = roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
    free(image);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Identical endpoints sharing one value, interleaved with non-flat blocks: the flat blocks use the
// header value, the non-flat ones keep the full index payload
TEST identical_colors_mixed(void)
{
    bc1_block* image = malloc(TEST_IMAGE_SIZE * sizeof(bc1_block));
    if (!image)
        return GREATEST_TEST_RES_FAIL;

    for(uint32_t i=0; i<TEST_IMAGE_SIZE; ++i)
    {
        uint16_t c = (uint16_t)(i * 0x1F);

        if (i & 1)
        {
            image[i] = (bc1_block)
            {
                .color = {c, c},
                .indices = 0x33333333
            };
        }
        else
        {
            image[i] = (bc1_block)
            {
                .color = {c, (uint16_t)(c + 0x3A)},
                .indices = 0x0F0F0F0F
            };
        }
    }

    greatest_test_res res = roundtrip(image, TEST_IMAGE_WIDTH, TEST_IMAGE_HEIGHT);
    free(image);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Multi-resolution roundtrip testing with SplitMix32 random blocks
TEST variable_resolutions_random(void)
{
    const uint32_t widths[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
    const uint32_t heights[] = { 256, 512, 1024, 2048, 4096 };
    const size_t num_widths = sizeof(widths) / sizeof(widths[0]);
    const size_t num_heights = sizeof(heights) / sizeof(heights[0]);

    uint32_t rng_state = 0xA5A5A5A5u;

    for (size_t w = 0; w < num_widths; ++w)
    {
        for (size_t h = 0; h < num_heights; ++h)
        {
            uint32_t width = widths[w];
            uint32_t height = heights[h];
            size_t num_blocks = (width / 4) * (height / 4);

            printf("%ux%u\n", width, height);

            bc1_block* image = malloc(num_blocks * sizeof(bc1_block));
            if (!image) return GREATEST_TEST_RES_FAIL;

            for (size_t i = 0; i < num_blocks; ++i)
            {
                uint32_t r1 = splitmix32(&rng_state);
                uint32_t r2 = splitmix32(&rng_state);

                image[i] = (bc1_block)
                {
                    .color = { (uint16_t)(r1 & 0xFFFF), (uint16_t)(r1 >> 16) },
                    .indices = r2
                };
            }

            enum greatest_test_res res = roundtrip(image, width, height);
            free(image);

            if (res != GREATEST_TEST_RES_PASS) return res;
        }
    }

    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
// helper to pack an R5G6B5 color, the test has no access to the library private helpers
static uint16_t pack_565(uint8_t r5, uint8_t g6, uint8_t b5)
{
    return (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Each strip is a constant color pair, random per strip, so every strip starts from a
// different color and every strip differs from every other strip. stress round-trip for
// strip boundary handling: a decoder that leaked or re-seeded state incorrectly between
// strips would corrupt the colors
TEST strip_uniform_colors(void)
{
    const uint32_t width = TEST_IMAGE_WIDTH;
    const uint32_t height = TEST_IMAGE_HEIGHT;
    const uint32_t width_blocks = width / 4;
    const uint32_t strip_width = (height / 4) / BCP_NUM_STRIPS;

    bc1_block image[TEST_IMAGE_SIZE];

    uint32_t rng_state = 0x1B2C3D4Eu;
    for (uint32_t strip = 0; strip < BCP_NUM_STRIPS; ++strip)
    {
        uint32_t r = splitmix32(&rng_state);
        uint16_t c0 = (uint16_t)(r & 0xFFFF);
        uint16_t c1 = (uint16_t)(r >> 16);
        uint32_t indices = splitmix32(&rng_state);

        uint32_t start_y = strip * strip_width;
        for (uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for (uint32_t x = 0; x < width_blocks; ++x)
            {
                image[y * width_blocks + x] = (bc1_block)
                {
                    .color = { c0, c1 },
                    .indices = indices
                };
            }
        }
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Every index is an odd-multiplied permutation of the block order, so the indices are
// unique under every byte mask: no index pattern repeats, the most demanding case for the
// index coding. any loss of rare exact patterns would corrupt the round-trip
TEST all_distinct_indices(void)
{
    const uint32_t width = 16;
    const uint32_t height = 256;

    bc1_block image[TINY_IMAGE_BLOCKS];
    for (uint32_t i = 0; i < TINY_IMAGE_BLOCKS; ++i)
    {
        uint32_t v = i;

        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = v | (((v * 17u + 3u) & 0xFF) << 8) | (((v * 45u + 7u) & 0xFF) << 16) | (((v * 113u + 19u) & 0xFF) << 24)
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Two color layouts, both must round-trip byte-exact. an encoder/decoder disagreement on
// which block a color is predicted against would desynchronize the stream and corrupt it.
// colors varying with the in-row position (constant along the zigzag columns) make one
// reference exact for every block, the mirrored pattern, varying with the row (constant
// across rows), makes the other one exact instead
TEST row_varying_colors(void)
{
    const uint32_t width = 256;
    const uint32_t height = 512;
    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;

    ASSERT(height_blocks == BCP_NUM_STRIPS * 2); // two scanlines per strip, so every prediction is reachable

    bc1_block image[TEST_IMAGE_SIZE];

    // colors vary with the in-row position, so they are constant along the zigzag columns
    for (uint32_t y = 0; y < height_blocks; ++y)
    {
        for (uint32_t p = 0; p < width_blocks; ++p)
        {
            image[y * width_blocks + p] = (bc1_block)
            {
                .color = { pack_565((uint8_t)(p & 0x1F), (uint8_t)((p << 1) & 0x3F), (uint8_t)(p & 0x1F)), 0x0000 },
                .indices = 0
            };
        }
    }

    enum greatest_test_res res = roundtrip(image, width, height);
    if (res != GREATEST_TEST_RES_PASS) return res;

    // the mirrored pattern varies with the row, so it is constant across the rows
    for (uint32_t y = 0; y < height_blocks; ++y)
    {
        for (uint32_t p = 0; p < width_blocks; ++p)
        {
            image[y * width_blocks + p] = (bc1_block)
            {
                .color = { pack_565((uint8_t)(y & 0x1F), (uint8_t)((y << 1) & 0x3F), (uint8_t)(y & 0x1F)), 0x0000 },
                .indices = 0
            };
        }
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// A rare index pattern (2 of 256 blocks) next to a common one: any index coder that drops
// the rare pattern, or quantizes the two apart, would corrupt the round-trip
TEST rare_index_pattern(void)
{
    const uint32_t width = 16;
    const uint32_t height = 256;

    const uint32_t common = 0x12345678u;
    const uint32_t rare = common ^ 0x0F0F0F0F; // 4 differing bits

    bc1_block image[TINY_IMAGE_BLOCKS];
    for (uint32_t i = 0; i < TINY_IMAGE_BLOCKS; ++i)
    {
        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = (i < 2) ? rare : common
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Contextual color models. a quadratic green ramp makes every block's green delta take a
// different value (delta of i*i is 2i+1 mod 64), sweeping the context buckets on both
// endpoints. a mismatch of the context selection between encoder and decoder (or histogram
// and encode) would corrupt the stream, the round-trip must stay byte-exact
TEST color_context(void)
{
    const uint32_t width = 256;
    const uint32_t height = 256;
    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const uint32_t num_blocks = width_blocks * height_blocks;

    bc1_block image[TEST_IMAGE_SIZE];
    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        image[i] = (bc1_block)
        {
            .color = {
                pack_565((uint8_t)((i * 7) & 0x1F), (uint8_t)((i * i) & 0x3F), (uint8_t)((i * 3) & 0x1F)),
                pack_565((uint8_t)((i * 11) & 0x1F), (uint8_t)((i * i * 3) & 0x3F), (uint8_t)((i * 5) & 0x1F))
            },
            .indices = 0
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Color-pair repetition. the two endpoint colors cycle through eight distinct values along
// the zigzag chain (period 8 divides the 64-block row width, so it is period 8 in the chain
// too): every block after the first seven exactly matches the block eight back. an
// encoder/decoder disagreement on how repeating pairs are coded would desynchronize the
// stream and corrupt the round-trip, which must stay byte-exact
TEST color_dict_repeat(void)
{
    const uint32_t width = 256;
    const uint32_t height = 256;
    const uint32_t num_blocks = (width / 4) * (height / 4);

    bc1_block image[TEST_IMAGE_SIZE];
    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        uint32_t t = i & 7; // period 8 in the zigzag chain

        image[i] = (bc1_block)
        {
            .color = {
                pack_565((uint8_t)((t * 3) & 0x1F), (uint8_t)((t * 5) & 0x3F), (uint8_t)((t * 2) & 0x1F)),
                pack_565((uint8_t)((t * 4 + 1) & 0x1F), (uint8_t)((t * 6 + 2) & 0x3F), (uint8_t)((t * 2 + 3) & 0x1F))
            },
            .indices = 0x2B2B2B2Bu
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// The two extremes of endpoint-color-pair repetition, in the same image. the top half cycles
// through eight distinct color pairs (every block matches the block eight back), the bottom
// half uses all-distinct pairs (a splitmix32 state is a 32-bit permutation of the block
// index, so no pair repeats). any encoder/decoder disagreement on how repeating vs matching
// pairs are coded would corrupt the round-trip, which must stay byte-exact
TEST periodic_color_pairs(void)
{
    const uint32_t width = 256;
    const uint32_t height = 256;
    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const uint32_t num_blocks = width_blocks * height_blocks;

    bc1_block image[TEST_IMAGE_SIZE];
    uint32_t rng_state = 0x4286F4A7u;

    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        const uint32_t y = i / width_blocks;
        uint16_t c0, c1;

        if (y < height_blocks / 2)
        {
            // period 8 in the zigzag chain: the 64-block row width is a multiple of 8, like color_dict_repeat
            const uint32_t t = i & 7;

            c0 = pack_565((uint8_t)((t * 3) & 0x1F), (uint8_t)((t * 5) & 0x3F), (uint8_t)((t * 2) & 0x1F));
            c1 = pack_565((uint8_t)((t * 4 + 1) & 0x1F), (uint8_t)((t * 6 + 2) & 0x3F), (uint8_t)((t * 2 + 3) & 0x1F));
        }
        else
        {
            // all-distinct endpoint color pairs: the two 16-bit colors are the two halves of a permutation
            const uint32_t t = splitmix32(&rng_state);

            c0 = (uint16_t)t;
            c1 = (uint16_t)(t >> 16);
        }

        image[i] = (bc1_block)
        {
            .color = { c0, c1 },
            .indices = 0x11111111u
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// tile grid of the 64-tile layout: the same width-based choice the compressor makes
// (8 for wide images, shrinking to 1 for the narrowest), 64 / gx rows per the even split
static uint32_t tile_columns(uint32_t width_blocks)
{
    if (width_blocks >= 32) return 8;
    if (width_blocks >= 16) return 4;
    if (width_blocks >= 8)  return 2;
    return 1;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Each tile is one constant color pair and one index word, random per tile, so every tile
// starts from a different color and every tile differs from every other tile. stress
// round-trip for tile boundary handling: a decoder that leaked or re-seeded state
// incorrectly between tiles would corrupt the colors (the tile version of strip_uniform_colors)
TEST tile_uniform_colors(void)
{
    const uint32_t width = TEST_IMAGE_WIDTH;
    const uint32_t height = TEST_IMAGE_HEIGHT;
    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;

    const uint32_t gx = tile_columns(width_blocks);
    const uint32_t gy = BCP_NUM_STRIPS / gx;

    ASSERT(height_blocks % gy == 0); // even split: every tile the same size

    bc1_block image[TEST_IMAGE_SIZE];

    uint32_t rng_state = 0x5EED7A21u;
    for (uint32_t strip = 0; strip < BCP_NUM_STRIPS; ++strip)
    {
        const uint32_t tx = strip % gx;
        const uint32_t ty = strip / gx;

        const uint32_t x0 = (tx * width_blocks) / gx;
        const uint32_t x1 = ((tx + 1) * width_blocks) / gx;
        const uint32_t y0 = (ty * height_blocks) / gy;
        const uint32_t y1 = ((ty + 1) * height_blocks) / gy;

        const uint16_t c0 = (uint16_t)splitmix32(&rng_state);
        const uint16_t c1 = (uint16_t)splitmix32(&rng_state);
        const uint32_t indices = splitmix32(&rng_state);

        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
                image[y * width_blocks + x] = (bc1_block) { .color = { c0, c1 }, .indices = indices };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// a 36 x 70 block image: neither block count is a multiple of the 8 x 8 tile size, so the
// even split must cover every row and column (the old full-width strip truncation silently
// dropped the trailing rows of images whose height was not a multiple of 64 blocks)
TEST odd_dimensions(void)
{
    const uint32_t width = 4 * 36;    // 144 px = 36 blocks
    const uint32_t height = 4 * 70;   // 280 px = 70 blocks
    const uint32_t width_blocks = 36;
    const uint32_t height_blocks = 70;

    const uint32_t num_blocks = width_blocks * height_blocks;

    bc1_block image[36 * 70]; // literal size: a const-folded VLA trips -Wgnu-folding-constant

    uint32_t rng_state = 0x0D515B0Cu;
    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        image[i] = (bc1_block)
        {
            .color = { (uint16_t)splitmix32(&rng_state), (uint16_t)splitmix32(&rng_state) },
            .indices = splitmix32(&rng_state)
        };
    }

    return roundtrip(image, width, height);
}

//-----------------------------------------------------------------------------------------------------------------------------
// BC4 / BC5 synthetic roundtrips, 256 x 256 = 4 x 64 blocks (the minimum supported image)
#define TEST_BC45_WIDTH (256)
#define TEST_BC45_HEIGHT (256)
#define TEST_BC45_BLOCKS ((TEST_BC45_WIDTH/4) * (TEST_BC45_HEIGHT/4))

//-----------------------------------------------------------------------------------------------------------------------------
// helper function to validate a bc4/bc5 roundtrip: the compressed stream fits the documented
// maxsize and decompressing all strips reproduces the input byte-exact
TEST bc45_roundtrip(const void* image, uint32_t width, uint32_t height, uint32_t num_channels)
{
    const size_t block_bytes = num_channels * sizeof(bc4_block);
    const size_t num_blocks = (size_t)(width / 4) * (height / 4);

    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = (num_channels == 1) ? packed_bc4_maxsize(width, height) : packed_bc5_maxsize(width, height);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    uint32_t num_strips = 0;
    size_t compressed_buffer_length = (num_channels == 1) ? bcp_compress_bc4(ctx, image, width, height, compressed_buffer, compressed_buffer_size, &num_strips)
                                                          : bcp_compress_bc5(ctx, image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
    ASSERT(compressed_buffer_length != 0);
    ASSERT(compressed_buffer_length <= compressed_buffer_size);

    uint8_t* decompressed = malloc(num_blocks * block_bytes);
    ASSERT(decompressed != NULL);

    for(uint32_t i=0; i<num_strips; ++i)
    {
        bool ok = (num_channels == 1) ? bcp_decompress_bc4(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, i)
                                       : bcp_decompress_bc5(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, i);
        ASSERT(ok);
    }

    for(size_t i=0; i<num_blocks * block_bytes; ++i)
        ASSERT_EQ(decompressed[i], ((const uint8_t*)image)[i]);

    bcp_terminate(ctx);

    free(compressed_buffer);
    free(decompressed);

    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
// runs the image through both formats: 1 channel (BC4) and 2 channels (BC5, red + green).
// [image8] is 8 bytes per block (BC4 layout), [image16] 16 bytes per block (BC5 layout):
// the two formats have different block sizes, so each needs its own fully-sized buffer.
static greatest_test_res bc45_both(const bc4_block* image8, const bc4_block* image16, uint32_t width, uint32_t height)
{
    // an assertion fails by returning, so propagate the result instead of discarding it
    greatest_test_res res = bc45_roundtrip(image8, width, height, 1);
    if (res != GREATEST_TEST_RES_PASS)
        return res;
    return bc45_roundtrip(image16, width, height, 2);
}

//-----------------------------------------------------------------------------------------------------------------------------
// one solid block value: identical endpoints and constant indices, the degenerate
// single-symbol models
TEST bc45_flat(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2]; // 16-byte BC5 block: the red bc4_block followed by the green one

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].endpoint[0] = 0xC0;
        image[i].endpoint[1] = 0xC0;
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = 0x1B; // 3, 5, 3, 5, ... as 3-bit values

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel

        // green channel: the same constant pattern with different values
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[2 * i + 1].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    return bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// writes all 16 indices of the block equal to [value] in the 48-bit little-endian index field
static void bc45_set_uniform_indices(bc4_block* b, uint8_t value)
{
    uint64_t word = 0;
    for(uint32_t i=0; i<16; ++i)
        word |= (uint64_t)value << (3 * i);

    for(uint32_t j=0; j<6; ++j)
        b->indices[j] = (uint8_t)(word >> (8 * j));
}

//-----------------------------------------------------------------------------------------------------------------------------
// one solid block value per channel with a near-uniform 0x33 word (6, 3, 1 repeating as 3-bit
// values): the non-uniform word forces the full 15-symbol index encoding (mode 0), as bc45_flat
TEST bc45_flat_uniform(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2]; // 16-byte BC5 block: the red bc4_block followed by the green one

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].endpoint[0] = 0xC0;
        image[i].endpoint[1] = 0xC0;
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = 0x33; // 3, 3, 3, 3, ... all 16 indices uniform as 3-bit values

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel

        // green channel: the same endpoints (XOR of two equal bytes) with a different uniform value
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[2 * i + 1].indices[j] = 0x77; // 7, 7, 7, 7, ...
    }

    return bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// one solid block value per channel with a TRULY uniform index word (all 16 indices the same):
// selects the flat-index mode 1, the value stored once per channel in the stream header
TEST bc45_flat_uniform_word(void)
{
    bc4_block* image = malloc(TEST_BC45_BLOCKS * sizeof(bc4_block));
    bc4_block* image16 = malloc(TEST_BC45_BLOCKS * 2 * sizeof(bc4_block)); // 16-byte BC5 block: the red bc4_block followed by the green one
    if (!image || !image16)
    {
        free(image);
        free(image16);
        return GREATEST_TEST_RES_FAIL;
    }

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].endpoint[0] = 0xC0;
        image[i].endpoint[1] = 0xC0;
        bc45_set_uniform_indices(&image[i], 3);

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel

        // green channel: the same endpoints with a different uniform value
        image16[2 * i + 1].endpoint[0] = (uint8_t)(0xC0 ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(0xC0 ^ 0x5A);
        bc45_set_uniform_indices(&image16[2 * i + 1], 6);
    }

    greatest_test_res res = bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
    free(image);
    free(image16);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// identical endpoints but DIVERGING uniform index values (3 / 5 alternating per block): the channel
// falls back to the full 15-symbol index encoding (mode 0)
TEST bc45_flat_divergent(void)
{
    bc4_block* image = malloc(TEST_BC45_BLOCKS * sizeof(bc4_block));
    bc4_block* image16 = malloc(TEST_BC45_BLOCKS * 2 * sizeof(bc4_block)); // 16-byte BC5 block: the red bc4_block followed by the green one
    if (!image || !image16)
    {
        free(image);
        free(image16);
        return GREATEST_TEST_RES_FAIL;
    }

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].endpoint[0] = 0xC0;
        image[i].endpoint[1] = 0xC0;
        bc45_set_uniform_indices(&image[i], (uint8_t)((i & 1) ? 5 : 3));

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel

        image16[2 * i + 1].endpoint[0] = (uint8_t)(0xC0 ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(0xC0 ^ 0x5A);
        bc45_set_uniform_indices(&image16[2 * i + 1], (uint8_t)((i & 1) ? 7 : 1));
    }

    greatest_test_res res = bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
    free(image);
    free(image16);
    return res;
}

//-----------------------------------------------------------------------------------------------------------------------------
// alternating endpoint pairs and index patterns across the block grid
TEST bc45_checkerboard(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    uint32_t blocks_per_row = TEST_BC45_WIDTH / 4;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / blocks_per_row;

        if ((bx + by) % 2 == 0)
        {
            image[i].endpoint[0] = 0x30;
            image[i].endpoint[1] = 0x28;
            for(uint32_t j=0; j<6; ++j)
                image[i].indices[j] = (uint8_t)((bx + by + j) * 0x55);
        }
        else
        {
            image[i].endpoint[0] = 0x28;
            image[i].endpoint[1] = 0x30;
            for(uint32_t j=0; j<6; ++j)
                image[i].indices[j] = (uint8_t)((bx * 3 + by * 5 + j) * 0x69);
        }
    }

    bc4_block image16[TEST_BC45_BLOCKS * 2];
    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[2 * i + 1].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    return bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// smooth endpoint gradient (wide ranges) with position-derived indices
TEST bc45_gradient(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2];
    uint32_t blocks_per_row = TEST_BC45_WIDTH / 4;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / blocks_per_row;

        image[i].endpoint[0] = (uint8_t)((bx * 3 + by) & 0xFF);
        image[i].endpoint[1] = (uint8_t)((by * 5 + bx) & 0xFF);
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = (uint8_t)((bx ^ (by + j)) * 0x11);
    }

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[2 * i + 1].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    return bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}

//-----------------------------------------------------------------------------------------------------------------------------
// each block column is constant down the column: the top neighbor always matches the
// current texel and the left neighbor always differs, so the column-0 top context and
// the in-row left context must both stay consistent encoder and decoder across the
// 8 x 8 tile layout (256 x 256 = 64 x 64 blocks = 8 x 8 tiles of 8 x 8 blocks)
TEST tile_column_predictor(void)
{
    const uint32_t width = TEST_BC45_WIDTH;
    const uint32_t height = TEST_BC45_HEIGHT;
    const uint32_t width_blocks = width / 4;

    bc1_block image1[TEST_BC45_BLOCKS];
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2];

    for(uint32_t i = 0; i < TEST_BC45_BLOCKS; ++i)
    {
        const uint32_t bx = i % width_blocks;

        image1[i] = (bc1_block)
        {
            .color = { pack_565((uint8_t)(bx & 0x1F), (uint8_t)((bx << 1) & 0x3F), (uint8_t)(bx & 0x1F)), 0 },
            .indices = 0
        };
    }

    greatest_test_res res = roundtrip(image1, width, height);
    if (res != GREATEST_TEST_RES_PASS)
        return res;

    for(uint32_t i = 0; i < TEST_BC45_BLOCKS; ++i)
    {
        const uint32_t bx = i % width_blocks;

        image[i].endpoint[0] = (uint8_t)(bx & 0xFF);
        image[i].endpoint[1] = (uint8_t)((bx >> 4) & 0xFF);
        for(uint32_t j = 0; j < 6; ++j)
            image[i].indices[j] = (uint8_t)((bx * 5 + j * 3) & 0xFF);

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j = 0; j < 6; ++j)
            image16[2 * i + 1].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    res = bc45_both(image, image16, width, height);
    if (res != GREATEST_TEST_RES_PASS)
        return res;
    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
// pseudo-random blocks: near-uniform symbols (the raw-mode fallback) and full
// 64-context usage in a single image
TEST bc45_random_splitmix32(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2];
    uint32_t rng_state = 0x85EBCA6Bu;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].endpoint[0] = (uint8_t)splitmix32(&rng_state);
        image[i].endpoint[1] = (uint8_t)splitmix32(&rng_state);
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = (uint8_t)splitmix32(&rng_state);
    }

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        memcpy(&image16[i], &image[i], sizeof(bc4_block)); // red channel
        image16[i].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[i].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[i].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    return bc45_both(image, image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
// compresses [image] and returns the stream length (0 on failure): the size of the stream is
// the only stream property the tests compare, the content is never parsed
static size_t bc45_stream_length(const void* image, uint32_t width, uint32_t height, uint32_t num_channels)
{
    bcp_context* ctx = bcp_init(NULL);
    if (!ctx)
        return 0;

    size_t compressed_buffer_size = (num_channels == 1) ? packed_bc4_maxsize(width, height) : packed_bc5_maxsize(width, height);
    uint8_t* compressed_buffer = malloc(compressed_buffer_size);
    if (!compressed_buffer)
    {
        bcp_terminate(ctx);
        return 0;
    }

    uint32_t num_strips = 0;
    size_t length = (num_channels == 1) ? bcp_compress_bc4(ctx, image, width, height, compressed_buffer, compressed_buffer_size, &num_strips)
                                        : bcp_compress_bc5(ctx, image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);

    free(compressed_buffer);
    bcp_terminate(ctx);
    return length;
}

//-----------------------------------------------------------------------------------------------------------------------------
// endpoint pairs cycle through four distinct values along every row (period 4 divides the row
// width, so it holds in the zigzag chain too), the index words are position-derived. the
// control keeps the same index words but uses all-distinct endpoint pairs: repeated endpoint
// pairs must compress, so the stream of the cycling image is strictly shorter. both images
// round-trip byte-exact, in BC4 and BC5
TEST bc45_repeated_endpoint_pairs(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    bc4_block image16[TEST_BC45_BLOCKS * 2];
    bc4_block distinct[TEST_BC45_BLOCKS];
    uint32_t blocks_per_row = TEST_BC45_WIDTH / 4;
    uint32_t rng_state = 0x7C3B5A2Du;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t t = (i % blocks_per_row) & 3;

        image[i].endpoint[0] = (uint8_t)(0x10 + t * 0x20);
        image[i].endpoint[1] = (uint8_t)(0x08 + t * 0x10);

        uint64_t v = 0;
        for(uint32_t k=1; k<16; ++k)
            v |= (uint64_t)(((i % blocks_per_row) ^ (i / blocks_per_row) ^ k) & 7) << (3 * k);
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = (uint8_t)(v >> (8 * j));

        // control: the same index words, no endpoint pair repeats
        uint32_t pair = splitmix32(&rng_state);
        distinct[i].endpoint[0] = (uint8_t)(pair & 0xFF);
        distinct[i].endpoint[1] = (uint8_t)((pair >> 8) & 0xFF);
        memcpy(distinct[i].indices, image[i].indices, sizeof(distinct[i].indices));

        memcpy(&image16[2 * i], &image[i], sizeof(bc4_block)); // red channel
        image16[2 * i + 1].endpoint[0] = (uint8_t)(image[i].endpoint[0] ^ 0x5A);
        image16[2 * i + 1].endpoint[1] = (uint8_t)(image[i].endpoint[1] ^ 0x5A);
        for(uint32_t j=0; j<6; ++j)
            image16[2 * i + 1].indices[j] = (uint8_t)(image[i].indices[j] ^ 0x38);
    }

    greatest_test_res res = bc45_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
    if (res != GREATEST_TEST_RES_PASS)
        return res;
    res = bc45_roundtrip(image16, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 2);
    if (res != GREATEST_TEST_RES_PASS)
        return res;
    res = bc45_roundtrip(distinct, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
    if (res != GREATEST_TEST_RES_PASS)
        return res;

    size_t repeated_length = bc45_stream_length(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
    size_t distinct_length = bc45_stream_length(distinct, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
    ASSERT(repeated_length != 0 && distinct_length != 0);
    ASSERT(repeated_length < distinct_length);

    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
// all-distinct endpoint pairs (a splitmix32 state is a permutation of the block index, so no
// pair repeats anywhere): the endpoint deltas carry full entropy, the worst case for the
// endpoint coding. the round-trip must stay byte-exact
TEST bc45_distinct_endpoint_pairs(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    uint32_t rng_state = 0x7C3B5A2Du;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t pair = splitmix32(&rng_state);

        image[i].endpoint[0] = (uint8_t)(pair & 0xFF);
        image[i].endpoint[1] = (uint8_t)((pair >> 8) & 0xFF);
        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = (uint8_t)((pair >> (4 * j)) & 0xFF);
    }

    return bc45_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
}

//-----------------------------------------------------------------------------------------------------------------------------
// half of the blocks repeat one endpoint pair, the other half all-distinct random pairs:
// repeated and never-repeated pairs in the same tiles. the round-trip must stay byte-exact
TEST bc45_mixed_endpoint_pairs(void)
{
    bc4_block image[TEST_BC45_BLOCKS];
    uint32_t rng_state = 0x15F2D84Eu;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        if ((i % 4) == 0)
        {
            image[i].endpoint[0] = 0x40;
            image[i].endpoint[1] = 0x20;
        }
        else
        {
            uint32_t pair = splitmix32(&rng_state);
            image[i].endpoint[0] = (uint8_t)(pair & 0xFF);
            image[i].endpoint[1] = (uint8_t)((pair >> 8) & 0xFF);
        }

        for(uint32_t j=0; j<6; ++j)
            image[i].indices[j] = (uint8_t)(i & 0xFF);
    }

    return bc45_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT, 1);
}

//-----------------------------------------------------------------------------------------------------------------------------
// per-channel independence in one BC5 stream: the red channel cycles four endpoint pairs,
// the green channel uses all-distinct random pairs. both channels round-trip byte-exact
TEST bc45_per_channel_content(void)
{
    const uint32_t width = TEST_BC45_WIDTH;
    const uint32_t height = TEST_BC45_HEIGHT;
    const uint32_t blocks_per_row = width / 4;
    const uint32_t num_blocks = (width / 4) * (height / 4);

    bc4_block* image16 = malloc(num_blocks * 2 * sizeof(bc4_block));
    if (!image16)
        return GREATEST_TEST_RES_FAIL;

    uint32_t rng_state = 0x3D9C47B1u;
    for(uint32_t i=0; i<num_blocks; ++i)
    {
        uint32_t t = (i % blocks_per_row) & 3;

        image16[2 * i].endpoint[0] = (uint8_t)(0x10 + t * 0x20);
        image16[2 * i].endpoint[1] = (uint8_t)(0x08 + t * 0x10);

        uint32_t pair = splitmix32(&rng_state);
        image16[2 * i + 1].endpoint[0] = (uint8_t)(pair & 0xFF);
        image16[2 * i + 1].endpoint[1] = (uint8_t)((pair >> 8) & 0xFF);

        for(uint32_t j=0; j<6; ++j)
        {
            image16[2 * i].indices[j] = (uint8_t)(i & 0xFF);
            image16[2 * i + 1].indices[j] = (uint8_t)((pair >> (4 * j)) & 0xFF);
        }
    }

    greatest_test_res res = bc45_roundtrip(image16, width, height, 2);
    free(image16);
    return res;
}


//-----------------------------------------------------------------------------------------------------------------------------
// helper function to validate a bc3 roundtrip: the compressed stream fits the documented
// maxsize and decompressing all strips reproduces the input byte-exact
// [image] is the standard 16-byte BC3 layout: a bc1_block followed by a bc4_block, 16 bytes per block
TEST bc3_roundtrip(const bc3_block* image, uint32_t width, uint32_t height)
{
    const size_t num_blocks = (width/4) * (height/4);
    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = packed_bc3_maxsize(width, height);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    uint32_t num_strips = 0;
    size_t compressed_buffer_length = bcp_compress_bc3(ctx, image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
    ASSERT(compressed_buffer_length != 0);
    ASSERT(compressed_buffer_length <= compressed_buffer_size);

    uint8_t* decompressed = malloc(num_blocks * sizeof(bc3_block));
    ASSERT(decompressed != NULL);

    for(uint32_t i=0; i<num_strips; ++i)
    {
        ASSERT(bcp_decompress_bc3(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, i));
    }

    for(size_t i=0; i<num_blocks * sizeof(bc3_block); ++i)
        ASSERT_EQ(decompressed[i], ((const uint8_t*)image)[i]);

    bcp_terminate(ctx);

    free(compressed_buffer);
    free(decompressed);

    PASS();
}


//-----------------------------------------------------------------------------------------------------------------------------
// one solid color + one solid alpha: the degenerate single-symbol models of both halves
TEST bc3_flat(void)
{
    bc3_block image[TEST_BC45_BLOCKS];

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].color.color[0] = red;
        image[i].color.color[1] = cyan;
        image[i].color.indices = 0x33333333;

        image[i].alpha.endpoint[0] = 0xC0;
        image[i].alpha.endpoint[1] = 0xC0;
        for(uint32_t j=0; j<6; ++j)
            image[i].alpha.indices[j] = 0x1B;
    }

    return bc3_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
// one solid color (non-flat endpoints, color half in mode 0) + one solid alpha with UNIFORM indices
// (flat endpoints, alpha half in the single-index mode 1): the two halves take different modes in
// one stream, unlike bc3_flat whose non-uniform alpha indices force mode 0 in both halves
TEST bc3_flat_uniform(void)
{
    bc3_block image[TEST_BC45_BLOCKS];

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        image[i].color.color[0] = red;
        image[i].color.color[1] = cyan;
        image[i].color.indices = 0x33333333;

        image[i].alpha.endpoint[0] = 0xC0;
        image[i].alpha.endpoint[1] = 0xC0;
        for(uint32_t j=0; j<6; ++j)
            image[i].alpha.indices[j] = 0x33; // 3, 3, 3, 3, ... all 16 indices uniform as 3-bit values
    }

    return bc3_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
// alternating color and alpha patterns across the block grid
TEST bc3_checkerboard(void)
{
    bc3_block image[TEST_BC45_BLOCKS];
    uint32_t blocks_per_row = TEST_BC45_WIDTH / 4;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / blocks_per_row;

        if ((bx + by) % 2 == 0)
        {
            image[i].color.color[0] = red;
            image[i].color.color[1] = cyan;
            image[i].color.indices = 0x33333333;

            image[i].alpha.endpoint[0] = 0x30;
            image[i].alpha.endpoint[1] = 0x28;
            for(uint32_t j=0; j<6; ++j)
                image[i].alpha.indices[j] = (uint8_t)((bx + by + j) * 0x55);
        }
        else
        {
            image[i].color.color[0] = cyan;
            image[i].color.color[1] = red;
            image[i].color.indices = 0xCCCCCCCC;

            image[i].alpha.endpoint[0] = 0x28;
            image[i].alpha.endpoint[1] = 0x30;
            for(uint32_t j=0; j<6; ++j)
                image[i].alpha.indices[j] = (uint8_t)((bx * 3 + by * 5 + j) * 0x69);
        }
    }

    return bc3_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
// smooth gradients in both halves: the endpoint deltas of the color and the alpha stay small
TEST bc3_gradient(void)
{
    bc3_block image[TEST_BC45_BLOCKS];
    uint32_t blocks_per_row = TEST_BC45_WIDTH / 4;
    uint32_t num_rows = TEST_BC45_HEIGHT / 4;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t bx = i % blocks_per_row;
        uint32_t by = i / blocks_per_row;

        image[i].color.color[0] = (uint16_t)(((bx * 31) / blocks_per_row) << 11) | (uint16_t)(((by * 63) / num_rows) << 5);
        image[i].color.color[1] = (uint16_t)(((by * 31) / num_rows) << 11) | (uint16_t)((((blocks_per_row - 1 - bx) * 31) / blocks_per_row));
        image[i].color.indices = (bx * 0x11111111u) ^ (by * 0x22222222u);

        image[i].alpha.endpoint[0] = (uint8_t)((bx * 3 + by) & 0xFF);
        image[i].alpha.endpoint[1] = (uint8_t)((by * 5 + bx) & 0xFF);
        for(uint32_t j=0; j<6; ++j)
            image[i].alpha.indices[j] = (uint8_t)((bx ^ (by + j)) * 0x11);
    }

    return bc3_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
// pseudo-random blocks in both halves: near-uniform symbols (the raw-mode fallback) and full
// context usage in a single image
TEST bc3_random_splitmix32(void)
{
    bc3_block image[TEST_BC45_BLOCKS];
    uint32_t rng_state = 0x85EBCA6Bu;

    for(uint32_t i=0; i<TEST_BC45_BLOCKS; ++i)
    {
        uint32_t r1 = splitmix32(&rng_state);
        uint32_t r2 = splitmix32(&rng_state);

        image[i].color = (bc1_block)
        {
            .color = {(uint16_t)(r1 & 0xFFFF), (uint16_t)(r1 >> 16)},
            .indices = r2
        };

        image[i].alpha.endpoint[0] = (uint8_t)splitmix32(&rng_state);
        image[i].alpha.endpoint[1] = (uint8_t)splitmix32(&rng_state);
        for(uint32_t j=0; j<6; ++j)
            image[i].alpha.indices[j] = (uint8_t)splitmix32(&rng_state);
    }

    return bc3_roundtrip(image, TEST_BC45_WIDTH, TEST_BC45_HEIGHT);
}


//-----------------------------------------------------------------------------------------------------------------------------
SUITE(suite_synthetic)
{
    RUN_TEST(flat);
    RUN_TEST(checkerboard);
    RUN_TEST(gradient);
    RUN_TEST(random_splitmix32);
    RUN_TEST(bc45_flat);
    RUN_TEST(bc45_flat_uniform);
    RUN_TEST(bc45_flat_uniform_word);
    RUN_TEST(bc45_flat_divergent);
    RUN_TEST(bc45_checkerboard);
    RUN_TEST(bc45_gradient);
    RUN_TEST(bc45_random_splitmix32);
    RUN_TEST(bc45_repeated_endpoint_pairs);
    RUN_TEST(bc45_distinct_endpoint_pairs);
    RUN_TEST(bc45_mixed_endpoint_pairs);
    RUN_TEST(bc45_per_channel_content);
    RUN_TEST(bc3_flat);
    RUN_TEST(bc3_flat_uniform);
    RUN_TEST(bc3_checkerboard);
    RUN_TEST(bc3_gradient);
    RUN_TEST(bc3_random_splitmix32);
    RUN_TEST(color0_le_color1);
    RUN_TEST(identical_colors);
    RUN_TEST(identical_colors_uniform_value);
    RUN_TEST(identical_colors_divergent);
    RUN_TEST(identical_colors_garbage);
    RUN_TEST(identical_colors_mixed);
    RUN_TEST(strip_uniform_colors);
    RUN_TEST(variable_resolutions_random);
    RUN_TEST(all_distinct_indices);
    RUN_TEST(row_varying_colors);
    RUN_TEST(rare_index_pattern);
    RUN_TEST(color_context);
    RUN_TEST(color_dict_repeat);
    RUN_TEST(periodic_color_pairs);
    RUN_TEST(tile_uniform_colors);
    RUN_TEST(tile_column_predictor);
    RUN_TEST(odd_dimensions);
}
