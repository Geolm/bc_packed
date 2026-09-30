#include "greatest.h"
#include "bc1_packed.h"
#include <stdlib.h>

//-----------------------------------------------------------------------------------------------------------------------------
typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;

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
        ASSERT(bc1_packed_uncompress(ctx, compressed_buffer, compressed_buffer_length, width, height, decompressed_bc1_image, i));
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


SUITE(suite_synthetic)
{
    RUN_TEST(flat);
}
