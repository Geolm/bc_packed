#include "test_helpers.h"


#define TEST_IMAGE_SIZE (65536)
#define TEST_IMAGE_WIDTH (256)
#define TEST_IMAGE_HEIGHT (256)

const uint16_t red   = 0xF800;
const uint16_t cyan  = 0x07FF;

//-----------------------------------------------------------------------------------------------------------------------------
// helper function to validate roundtrip
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
// Test 2: Alternating checkerboard pattern across block grid
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
// Test 3: Smooth gradient across RGB565 color values and index masks
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
// Test 4: Pseudo-random block generation using SplitMix32
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
// Test 5: Blocks where color[0] <= color[1] (enables BC1 1-bit alpha mode)
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
// Test 6: Identical endpoints (color[0] == color[1])
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
// Test 7: Stats on a constant image, components should match the expected stream content
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

    bc1_packed_stats stats;
    bc1_packed_get_stats(ctx, &stats);

    // constant indices match the top-table entry exactly -> no residual byte is written
    ASSERT_EQ(stats.difference_bytes, 0);

    // 6 color delta symbols per block, each at least 1 bit
    ASSERT(stats.colors_bytes >= (6 * num_blocks) / 8);

    // 1 reference and 1 mask symbol per block, each at least 1 bit
    ASSERT(stats.reference_bytes >= num_blocks / 8);
    ASSERT(stats.mask_bytes >= num_blocks / 8);

    // the components fit in the stream
    const uint64_t payload = stats.colors_bytes + stats.reference_bytes + stats.mask_bytes + stats.difference_bytes;
    ASSERT(payload <= stream_size);

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
// Test 8: Multi-resolution roundtrip testing with SplitMix32 random blocks
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


// helper to pack an R5G6B5 color, the test has no access to the library private helpers
static uint16_t pack_565(uint8_t r5, uint8_t g6, uint8_t b5)
{
    return (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
}

//-----------------------------------------------------------------------------------------------------------------------------
// walks the stream header (6 static models, top-table size byte, padded to a 4-byte boundary) to read the strip seed
static bool read_header_strip_seed(const uint8_t* stream, size_t length, uint32_t* seed)
{
    size_t pos = 0;

    for (uint32_t i = 0; i < 6; ++i)
    {
        if (pos + 2 > length) return false;
        uint32_t num_symbols = stream[pos++] + 1;
        pos++; // k
        if (pos + num_symbols > length) return false;
        pos += num_symbols;
    }

    if (pos + 1 > length) return false;
    pos++; // top-table size

    pos = (pos + 3) & ~(size_t)3; // padded to a 4-byte boundary

    if (pos + 4 > length) return false;

    *seed = (uint32_t)stream[pos] | ((uint32_t)stream[pos + 1] << 8) | ((uint32_t)stream[pos + 2] << 16) | ((uint32_t)stream[pos + 3] << 24);
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// Test 9: the strip seed stored in the stream header must be the average of the first block of each strip
TEST strip_seed_header(void)
{
    const uint32_t width = TEST_IMAGE_WIDTH;
    const uint32_t height = TEST_IMAGE_HEIGHT;
    const uint32_t num_blocks = (width / 4) * (height / 4);
    const uint32_t width_blocks = width / 4;
    const uint32_t strip_width = (height / 4) / BC1_PACKED_NUM_STRIPS;

    bc1_block image[TEST_IMAGE_SIZE];

    uint32_t rng_state = 0x1B2C3D4Eu;
    uint32_t sums[6] = { 0, 0, 0, 0, 0, 0 }; // r, g, b of color[0] and color[1], one first block per strip

    for (uint32_t strip = 0; strip < BC1_PACKED_NUM_STRIPS; ++strip)
    {
        uint32_t r = splitmix32(&rng_state);

        uint16_t c0 = (uint16_t)(r & 0xFFFF);
        uint16_t c1 = (uint16_t)(r >> 16);
        uint32_t indices = splitmix32(&rng_state);

        sums[0] += (c0 >> 11) & 0x1F;
        sums[1] += (c0 >> 5) & 0x3F;
        sums[2] += c0 & 0x1F;
        sums[3] += (c1 >> 11) & 0x1F;
        sums[4] += (c1 >> 5) & 0x3F;
        sums[5] += c1 & 0x1F;

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

    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width, height);
    uint8_t* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);

    // the seed stored in the header must be the per-channel average of the first block of each strip
    uint32_t seed = 0;
    ASSERT(read_header_strip_seed(compressed_buffer, stream_size, &seed));

    uint16_t expected0 = pack_565((uint8_t)(sums[0] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[1] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[2] / BC1_PACKED_NUM_STRIPS));
    uint16_t expected1 = pack_565((uint8_t)(sums[3] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[4] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[5] / BC1_PACKED_NUM_STRIPS));
    ASSERT_EQ(seed, ((uint32_t)expected1 << 16) | (uint32_t)expected0);

    // roundtrip must stay exact, the decoder must use the same seed from the stream
    bc1_block* decompressed = malloc(num_blocks * sizeof(bc1_block));
    ASSERT(decompressed != NULL);

    for (uint32_t i = 0; i < BC1_PACKED_NUM_STRIPS; ++i)
    {
        ASSERT(bc1_packed_decompress(ctx, compressed_buffer, stream_size, width, height, decompressed, i));
    }

    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        ASSERT_EQ(decompressed[i].color[0], image[i].color[0]);
        ASSERT_EQ(decompressed[i].color[1], image[i].color[1]);
        ASSERT_EQ(decompressed[i].indices, image[i].indices);
    }

    bc1_packed_terminate(ctx);

    free(compressed_buffer);
    free(decompressed);

    PASS();
}


SUITE(suite_synthetic)
{
    (void)flat;
    (void)checkerboard;
    (void)gradient;
    (void)random_splitmix32;
    (void)color0_le_color1;
    (void)identical_colors;
    (void)stats_constant_image;
    (void)strip_seed_header;

    RUN_TEST(flat);
    RUN_TEST(checkerboard);
    RUN_TEST(gradient);
    RUN_TEST(random_splitmix32);
    RUN_TEST(color0_le_color1);
    RUN_TEST(identical_colors);
    RUN_TEST(stats_constant_image);
    RUN_TEST(strip_seed_header);
    RUN_TEST(variable_resolutions_random);
}
