#ifndef TEST_HELPERS
#define TEST_HELPERS

#include "greatest.h"
#include "bc1_packed.h"
#include <stdlib.h>

//-----------------------------------------------------------------------------------------------------------------------------
typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;

//-----------------------------------------------------------------------------------------------------------------------------
// SplitMix32 PRNG implementation
static inline uint32_t splitmix32(uint32_t* state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

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

#endif


