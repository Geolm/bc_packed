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
    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width,  height);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t compressed_buffer_length = bc1_packed_compress(ctx, bc1_image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(compressed_buffer_length != 0);
    ASSERT(compressed_buffer_length <= compressed_buffer_size);

    bc1_block* decompressed_bc1_image = malloc(num_blocks * sizeof(bc1_block));
    ASSERT(decompressed_bc1_image != NULL);

    for(uint32_t i=0; i<BC1_PACKED_NUM_STRIPS; ++i)
    {
        ASSERT(bc1_packed_decompress(ctx, compressed_buffer, compressed_buffer_length, width, height, decompressed_bc1_image, i));
    }

    for(uint32_t i=0; i<num_blocks; ++i)
    {
        ASSERT_EQ(decompressed_bc1_image[i].color[0], bc1_image[i].color[0]);
        ASSERT_EQ(decompressed_bc1_image[i].color[1], bc1_image[i].color[1]);
        ASSERT_EQ(decompressed_bc1_image[i].indices, bc1_image[i].indices);
    }

    bc1_packed_terminate(ctx);

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
// Stats on a constant image. the assertions are cross-output invariants documented by the
// public API only: the stream fits the reported maxsize, and the sum of the reported
// payload components never exceeds the stream size (the stream additionally carries the
// static header). no assumption is made about how the payload is coded internally;
// the roundtrip must stay exact
TEST stats_constant_image(void)
{
    const uint32_t width = TEST_IMAGE_WIDTH;
    const uint32_t height = TEST_IMAGE_HEIGHT;
    const uint32_t num_blocks = (width / 4) * (height / 4);

    bc1_block image[TEST_IMAGE_SIZE];
    for(uint32_t i=0; i<num_blocks; ++i)
        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = 0x33333333
        };

    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width, height);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);
    ASSERT(stream_size <= compressed_buffer_size);

    // the reported payload components are parts of the stream, so their sum fits the stream
    bc1_packed_stats stats;
    bc1_packed_get_stats(ctx, &stats);
    const uint64_t payload = (uint64_t)stats.colors_bytes + stats.reference_bytes + stats.mask_bytes + stats.difference_bytes;
    ASSERT(payload <= (uint64_t)stream_size);

    // roundtrip
    bc1_block* decompressed = malloc(num_blocks * sizeof(bc1_block));
    ASSERT(decompressed != NULL);

    for(uint32_t i=0; i<BC1_PACKED_NUM_STRIPS; ++i)
    {
        ASSERT(bc1_packed_decompress(ctx, compressed_buffer, stream_size, width, height, decompressed, i));
    }

    for(uint32_t i=0; i<num_blocks; ++i)
    {
        ASSERT_EQ(decompressed[i].indices, 0x33333333);
    }

    bc1_packed_terminate(ctx);

    free(compressed_buffer);
    free(decompressed);

    PASS();
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
    const uint32_t strip_width = (height / 4) / BC1_PACKED_NUM_STRIPS;

    bc1_block image[TEST_IMAGE_SIZE];

    uint32_t rng_state = 0x1B2C3D4Eu;
    for (uint32_t strip = 0; strip < BC1_PACKED_NUM_STRIPS; ++strip)
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

    ASSERT(height_blocks == BC1_PACKED_NUM_STRIPS * 2); // two scanlines per strip, so every prediction is reachable

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
SUITE(suite_synthetic)
{
    (void)flat;
    (void)checkerboard;
    (void)gradient;
    (void)random_splitmix32;
    (void)color0_le_color1;
    (void)identical_colors;
    (void)stats_constant_image;
    (void)strip_uniform_colors;
    (void)all_distinct_indices;
    (void)row_varying_colors;
    (void)rare_index_pattern;
    (void)color_context;
    (void)color_dict_repeat;
    (void)periodic_color_pairs;

    RUN_TEST(flat);
    RUN_TEST(checkerboard);
    RUN_TEST(gradient);
    RUN_TEST(random_splitmix32);
    RUN_TEST(color0_le_color1);
    RUN_TEST(identical_colors);
    RUN_TEST(stats_constant_image);
    RUN_TEST(strip_uniform_colors);
    RUN_TEST(variable_resolutions_random);
    RUN_TEST(all_distinct_indices);
    RUN_TEST(row_varying_colors);
    RUN_TEST(rare_index_pattern);
    RUN_TEST(color_context);
    RUN_TEST(color_dict_repeat);
    RUN_TEST(periodic_color_pairs);
}
