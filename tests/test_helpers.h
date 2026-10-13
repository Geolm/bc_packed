#ifndef TEST_HELPERS
#define TEST_HELPERS

#include "greatest.h"
#include "bc_packed.h"
#include <stdlib.h>

// number of strips the compressor currently uses; kept here for tests that
// build image patterns with a fixed strip count before calling the API
#define BCP_NUM_STRIPS (64)

//-----------------------------------------------------------------------------------------------------------------------------
typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of the bc4 block of src/bc_packed.c: the standard D3D layout, 8 bytes per 4x4 block
// (a BC5 block is the red bc4_block followed by the green one, 16 bytes)
typedef struct bc4_block
{
    uint8_t endpoint[2]; // [0] = max endpoint, [1] = min endpoint
    uint8_t indices[6];  // 16 x 3-bit indices, little-endian 48-bit field, raster order
} bc4_block;

// the standard 16-byte BC5 block: the red bc4_block followed by the green one
typedef struct bc5_block
{
    bc4_block channel[2]; // [0] = red, [1] = green
} bc5_block;

// the standard 16-byte BC3 block: the bc1_block color half followed by the bc4_block alpha half
typedef struct bc3_block
{
    bc1_block color;
    bc4_block alpha;
} bc3_block;

//-----------------------------------------------------------------------------------------------------------------------------
// SplitMix32 PRNG implementation
static inline uint32_t splitmix32(uint32_t* state)
{
    uint32_t z = (*state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

#endif


