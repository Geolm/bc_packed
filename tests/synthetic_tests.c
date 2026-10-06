#include "test_helpers.h"


#define TEST_IMAGE_SIZE (65536)
#define TEST_IMAGE_WIDTH (256)
#define TEST_IMAGE_HEIGHT (256)
#define TINY_IMAGE_BLOCKS (256) // 4 x 64 blocks, the minimum supported image

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
// walks the 17 static models (14 contextual color models: 3 red/blue per endpoint per green-delta
// context + 1 uncontexted green per endpoint, plus reference / mask / difference), the top-table
// size byte, and the 4-byte padding of the stream header, returning the offset of the strip seed,
// or (size_t)-1 if the header is truncated
static size_t header_after_models(const uint8_t* stream, size_t length)
{
    size_t pos = 0;

    for (uint32_t i = 0; i < 17; ++i)
    {
        if (pos + 3 > length) return (size_t)-1;
        uint32_t num_symbols = stream[pos++] + 1;
        pos++; // k
        pos++; // q_escape
        if (pos + num_symbols > length) return (size_t)-1;
        pos += num_symbols;
    }

    if (pos + 1 > length) return (size_t)-1;
    pos++; // top-table size

    return (pos + 3) & ~(size_t)3; // padded to a 4-byte boundary
}

//-----------------------------------------------------------------------------------------------------------------------------
// reads the strip seed from the stream header
static bool read_header_strip_seed(const uint8_t* stream, size_t length, uint32_t* seed)
{
    size_t pos = header_after_models(stream, length);
    if (pos == (size_t)-1) return false;

    if (pos + 4 > length) return false;

    *seed = (uint32_t)stream[pos] | ((uint32_t)stream[pos + 1] << 8) | ((uint32_t)stream[pos + 2] << 16) | ((uint32_t)stream[pos + 3] << 24);
    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// reads the 8 bytes of per-strip color predictor flags from the stream header, they follow the strip seed
// (bit s = 1 → strip s predicts its endpoint colors from the block above after its first scanline)
static bool read_header_predictor(const uint8_t* stream, size_t length, uint64_t* predictor)
{
    size_t pos = header_after_models(stream, length);
    if (pos == (size_t)-1) return false;

    if (pos + 4 + 8 > length) return false;
    pos += 4; // strip seed

    *predictor = 0;
    for (uint32_t j = 0; j < 8; ++j)
        *predictor |= (uint64_t)stream[pos + j] << (j * 8);
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


//-----------------------------------------------------------------------------------------------------------------------------
// Test 10: every byte of every index is an odd-multiplied permutation of the block order, so the indices are
// unique under every byte mask: no index pattern repeats, an extreme case for the top table. the table must
// never come out empty (an empty table corrupts the stream, its size is stored as size-1 in a byte, 0 reads
// back as 256), the 1-entry floor in build_top_table keeps the single most frequent exact pattern
TEST top_table_empty_floor(void)
{
    const uint32_t width = 16;
    const uint32_t height = 256;
    const uint32_t num_blocks = TINY_IMAGE_BLOCKS;

    bc1_block image[TINY_IMAGE_BLOCKS];
    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        uint32_t v = i;

        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = v | (((v * 17u + 3u) & 0xFF) << 8) | (((v * 45u + 7u) & 0xFF) << 16) | (((v * 113u + 19u) & 0xFF) << 24)
        };
    }

    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width, height);
    uint8_t* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);

    bc1_packed_stats stats;
    bc1_packed_get_stats(ctx, &stats);

    // the table must never be empty
    ASSERT(stats.top_table_size >= 1);

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


//---------------------------------------------------------------------------------------------------------------
// Test 11: per-strip color predictor. colors constant along the zigzag columns (varying with the array
// index) make the row-above reference an exact match while the zigzag-previous pays a delta for every
// block, so every strip must pick the row-above predictor (the all-ones flag word in the header). the
// mirrored pattern, constant along the rows, must keep the zigzag-previous predictor (all zeros).
// both must round-trip byte-exact
TEST strip_predictor(void)
{
    const uint32_t width = 256;
    const uint32_t height = 512;
    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const uint32_t num_blocks = width_blocks * height_blocks;

    ASSERT(height_blocks == BC1_PACKED_NUM_STRIPS * 2); // two scanlines per strip, so the top predictor is reachable

    bc1_block image[TEST_IMAGE_SIZE];

    // colors vary with the array index, so they are constant along the zigzag columns: the row-above
    // reference (same zigzag position) is an exact match for every block after the first scanline
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

    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width, height);
    uint8_t* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);

    uint64_t predictor = 0;
    ASSERT(read_header_predictor(compressed_buffer, stream_size, &predictor));
    ASSERT_EQ(predictor, (uint64_t)-1); // every strip predicts from the block above

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

    // the mirrored pattern is constant along the rows: the zigzag-previous (in-row neighbor) is an exact
    // match for every block, so the row-above predictor must never be picked
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

    stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);

    ASSERT(read_header_predictor(compressed_buffer, stream_size, &predictor));
    ASSERT_EQ(predictor, (uint64_t)0); // no strip uses the top predictor

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


//---------------------------------------------------------------------------------------------------------------
// Test 12: a rare exact pattern (2 of 256 blocks) must survive the VQ refinement. the rare pattern
// differs from the common one by 4 bits, so every block matches a table entry exactly (score 0) and
// the refinement never moves a centroid: only the final count filter decides. in the old jittered
// sampled final round the rare pattern's blocks can go unvisited and its entry gets dropped; the
// full-assignment final rounds (exp 15) visit every block, so the table must keep both entries
TEST top_table_full_refine(void)
{
    const uint32_t width = 16;
    const uint32_t height = 256;
    const uint32_t num_blocks = TINY_IMAGE_BLOCKS;

    const uint32_t common = 0x12345678u;
    const uint32_t rare = common ^ 0x0F0F0F0F; // 4 differing bits

    bc1_block image[TINY_IMAGE_BLOCKS];
    for (uint32_t i = 0; i < num_blocks; ++i)
    {
        image[i] = (bc1_block)
        {
            .color = {red, cyan},
            .indices = (i < 2) ? rare : common
        };
    }

    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(width, height);
    uint8_t* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);

    size_t stream_size = bc1_packed_compress(ctx, image, width, height, compressed_buffer, compressed_buffer_size);
    ASSERT(stream_size != 0);

    bc1_packed_stats stats;
    bc1_packed_get_stats(ctx, &stats);

    // both exact patterns are used by the image and the full-assignment final rounds keep the rare one
    ASSERT_EQ(stats.top_table_size, 2);

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


//---------------------------------------------------------------------------------------------------------------
// Test 13: contextual color models. a quadratic green ramp makes every block's green delta take a
// different value (delta of i*i is 2i+1 mod 64), sweeping the negative/positive context buckets on
// both endpoints. a mismatch of the context selection between encoder and decoder (or histogram and
// encode) would corrupt the stream, the round-trip must stay byte-exact
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
    (void)top_table_empty_floor;
    (void)strip_predictor;
    (void)top_table_full_refine;
    (void)color_context;

    RUN_TEST(flat);
    RUN_TEST(checkerboard);
    RUN_TEST(gradient);
    RUN_TEST(random_splitmix32);
    RUN_TEST(color0_le_color1);
    RUN_TEST(identical_colors);
    RUN_TEST(stats_constant_image);
    RUN_TEST(strip_seed_header);
    RUN_TEST(variable_resolutions_random);
    RUN_TEST(top_table_empty_floor);
    RUN_TEST(strip_predictor);
    RUN_TEST(top_table_full_refine);
    RUN_TEST(color_context);
}
