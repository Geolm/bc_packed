#include "test_helpers.h"


#define TEST_IMAGE_SIZE (65536)
#define TEST_IMAGE_WIDTH (256)
#define TEST_IMAGE_HEIGHT (256)

const uint16_t red   = 0xF800;
const uint16_t cyan  = 0x07FF;

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


SUITE(suite_synthetic)
{
    RUN_TEST(flat);
    RUN_TEST(checkerboard);
    RUN_TEST(gradient);
    RUN_TEST(random_splitmix32);
    RUN_TEST(color0_le_color1);
    RUN_TEST(identical_colors);
}
