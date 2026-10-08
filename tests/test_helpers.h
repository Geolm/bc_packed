#ifndef TEST_HELPERS
#define TEST_HELPERS

#include "greatest.h"
#include "bc_packed.h"
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

#endif


