#include "bc1_packed.h"
#include "../third_party/lite_encoding.h"
#include <stdlib.h>


#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
    #include <arm_neon.h>
    #define BC1_PACKED_NEON
#else
    #include <immintrin.h>
    #define BC1_PACKED_SSE
#endif

#if defined(_MSC_VER)
    #include <intrin.h>
    #pragma intrinsic(__popcnt)
    #pragma intrinsic(__popcnt64)
    #define popcount64(x) ((int)__popcnt64(x))
    #define popcount(x) ((int)__popcnt(x))
#else
    #define popcount64(x) __builtin_popcountll(x)
    #define popcount(x) __builtin_popcount(x)
#endif

//-----------------------------------------------------------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------------------------------------------------------

#define HASHMAP_SIZE        (1U << 20U)
#define TOP_TABLE_SIZE          (256U)


//-----------------------------------------------------------------------------------------------------------------------------
// Structures
//-----------------------------------------------------------------------------------------------------------------------------
typedef struct hashmap_entry
{
    uint32_t key;
    uint32_t count;
} hashmap_entry;

struct bc1_packed_context
{
    hashmap_entry* hashmap;
    bc1_packed_mem_interface mem;

    le_model red_model, green_model, blue_model;
};

typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;


//-----------------------------------------------------------------------------------------------------------------------------
// Private functions
//-----------------------------------------------------------------------------------------------------------------------------

static inline void* malloc_wrapper(size_t size, void* user) {(void)user; return malloc(size);}
static inline void* realloc_wrapper(void* old_ptr, size_t old_size, size_t new_size, void* user){(void)user;(void)old_size;return realloc(old_ptr, new_size);}
static inline void free_wrapper(void* ptr, void* user) {(void)user; free(ptr);}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bc1_packed_mem_interface default_allocator(void) 
{
    return (bc1_packed_mem_interface) 
    {
        .malloc_fn  = malloc_wrapper,
        .realloc_fn = realloc_wrapper,
        .free_fn    = free_wrapper,
        .user       = NULL
    };
}

//----------------------------------------------------------------------------------------------------------------------------
static inline void bc1_extract_565(uint16_t color, uint8_t *r5, uint8_t *g6, uint8_t *b5)
{
    *r5 = (uint8_t)((color >> 11) & 0x1F);
    *g6 = (uint8_t)((color >> 5)  & 0x3F);
    *b5 = (uint8_t)(color & 0x1F);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint16_t bc1_pack_565(uint8_t r5, uint8_t g6, uint8_t b5)
{
    return (uint16_t)(((uint16_t)r5 << 11) | ((uint16_t)g6 << 5) | (uint16_t)b5);
}

//----------------------------------------------------------------------------------------------------------------------------
static inline uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

//----------------------------------------------------------------------------------------------------------------------------
uint32_t nearest32(const uint32_t* table, uint32_t table_size, uint32_t bitfield)
{
    uint32_t scores[TOP_TABLE_SIZE];
    uint32_t i = 0;

#ifdef BC1_PACKED_NEON
    uint32x4_t bf_vec = vdupq_n_u32(bitfield);

    for (; i + 3 < table_size; i += 4)
    {
        uint32x4_t dict_vec = vld1q_u32(&table[i]);
        uint32x4_t delta = veorq_u32(dict_vec, bf_vec);
        uint8x16_t delta_bytes = vreinterpretq_u8_u32(delta);
        uint8x16_t counts = vcntq_u8(delta_bytes);
        uint16x8_t sum16 = vpaddlq_u8(counts);
        uint32x4_t sum32 = vpaddlq_u16(sum16);

        vst1q_u32(&scores[i], sum32);
    }
#else
    const __m128i bf_vec   = _mm_set1_epi32(bitfield);
    const __m128i mask_low = _mm_set1_epi8(0x0F);
    const __m128i lookup   = _mm_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);

    for (; i + 3 < table_size; i += 4) 
    {
        __m128i x = _mm_xor_si128(_mm_loadu_si128((const __m128i*)&table[i]), bf_vec);

        __m128i low  = _mm_and_si128(x, mask_low);
        __m128i high = _mm_and_si128(_mm_srli_epi32(x, 4), mask_low); 
        __m128i cnt  = _mm_add_epi8(_mm_shuffle_epi8(lookup, low), 
                                    _mm_shuffle_epi8(lookup, high));

        __m128i lo_words = _mm_and_si128(cnt, _mm_set1_epi16(0x00FF));
        __m128i hi_words = _mm_srli_epi16(cnt, 8);
        __m128i sums = _mm_add_epi16(lo_words, hi_words);
        __m128i final = _mm_madd_epi16(sums, _mm_set1_epi16(1));

        _mm_storeu_si128((__m128i*)&scores[i], final);
    }
#endif

    // tail (or whole array on x64)
    for (; i < table_size; ++i)
    {
        uint32_t score = popcount(table[i] ^ bitfield);
        if (score == 0) 
            return (0 << 16) | (i & 0xffff);
        scores[i] = score;
    }

    // find best
    uint32_t best_index = 0;
    uint32_t best_score = UINT32_MAX;
    for (uint32_t j = 0; j < table_size; ++j)
    {
        uint32_t score = scores[j];
        if (score < best_score || (score == best_score && table[j] > table[best_index]))
        {
            best_score = score;
            best_index = j;
        }
    }
    return ((best_score&0xffff)<<16) | (best_index&0xffff);
}

//----------------------------------------------------------------------------------------------------------------------------
void vq_top_table(const void* input, uint32_t num_blocks, uint32_t* output, uint32_t* num_entries)
{
    // static array of odd steps to avoid aliasing and branches
    // first iteration is always 1 to ensure 100% initial coverage.
    static const uint32_t steps[4] = { 1, 5, 11, 17 };

    uint32_t centroids[TOP_TABLE_SIZE];
    struct 
    {
        uint32_t bit_diff_count[32];
        uint32_t count;
    } clusters[TOP_TABLE_SIZE];

    // use the top table entries as candidate for the cluster
    for(uint32_t i=0; i<*num_entries; ++i)
        centroids[i] = output[i];

    // multiple iteration to stabilize cluster
    for(uint32_t iteration=0; iteration<4; ++iteration)
    {
        // clear cluster counters
        for(uint32_t i=0; i<*num_entries; ++i)
        {
            for(uint32_t j=0; j<32; ++j)
                clusters[i].bit_diff_count[j] = 0;

            clusters[i].count = 0;
        }

        const uint32_t sample_step = steps[iteration];

        uint32_t bucket = 0;
        const uint32_t threshold = 16;

        // find the best cluster for each block, jittering the first block
        for(uint32_t block_index=(iteration % sample_step); block_index<num_blocks;)
        {
            const bc1_block* b = (const bc1_block*) input + block_index;

            uint32_t result = nearest32(centroids, *num_entries, b->indices);
            uint32_t score = (result >> 16);
            uint32_t best_entry = result & 0xffff;
            uint32_t diff = b->indices ^ centroids[best_entry];

            for(uint32_t i=0; i<32; ++i)
                clusters[best_entry].bit_diff_count[i] += (diff >> i) & 1;

            clusters[best_entry].count++;
            bucket += score;

            if (bucket >= threshold) 
            {
                // if error is high, we only move 1 block
                block_index++;
                bucket -= threshold; 
            } else 
            {
                // normal skip
                block_index += sample_step;
            }
        }

        // move centroid
        for(uint32_t i=0; i<*num_entries; ++i)
        {
            if (clusters[i].count>0)
            {
                for(uint32_t bit=0; bit<32; ++bit)
                    if (clusters[i].bit_diff_count[bit] > (clusters[i].count/2))
                        centroids[i] ^= (1u << bit);

            }
            
        }
    }

    uint32_t num_clusters = 0;

    // fill the table with centroid
    for(uint32_t i=0; i<*num_entries; ++i)
        if (clusters[i].count > 0)
            output[num_clusters++] = centroids[i];

    // reduce if needed the size of the table to the number of cluster with at least one block
    *num_entries = num_clusters;
}

//----------------------------------------------------------------------------------------------------------------------------
void build_top_table(hashmap_entry* hashmap, const void* input, uint32_t num_blocks, uint32_t* output, uint32_t* num_entries)
{
    // clear the hashmap
    for(uint32_t i=0; i<HASHMAP_SIZE; ++i)
        hashmap[i].count = 0;

    // insert all blocks indices in the hashmap
    for(uint32_t i=0; i<num_blocks; ++i)
    {
        const bc1_block* b = (const bc1_block*) input + i;

        uint32_t h = hash32(b->indices);
        uint32_t index = h & (HASHMAP_SIZE - 1);
        uint32_t first_index = index;

        bool inserted = false;
        while (!inserted)
        {
            if ((hashmap[index].count == 0) || (hashmap[index].key == b->indices))
            {
                hashmap[index].key = b->indices;
                hashmap[index].count++;
                inserted = true;
            }
            else
            {
                index = (index + 1) & (HASHMAP_SIZE - 1);
                assert(index != first_index);
            }
        }
    }

    // clear the table
    hashmap_entry table[TOP_TABLE_SIZE];
    for(uint32_t i=0; i<TOP_TABLE_SIZE; ++i)
        table[i].count = 0;

    // fill the table with top most used indices
    for (uint32_t i = 0; i<HASHMAP_SIZE; ++i)
    {
        uint32_t c = hashmap[i].count;
        if (c == 0) 
            continue;

        // if smaller than current min, skip
        if (c <= table[0].count)
            continue;

        // replace min
        table[0] = hashmap[i];

        // bubble new smallest to front
        for (uint32_t j = 1; j < TOP_TABLE_SIZE; j++)
        {
            if (table[j-1].count > table[j].count)
            {
                hashmap_entry tmp = table[j - 1];
                table[j-1] = table[j];
                table[j] = tmp;
            }
            else break;
        }
    }
    
    // reverse the table for output and count
    *num_entries = 0;
    for(uint32_t i=0; i<TOP_TABLE_SIZE; ++i)
    {
        output[i] = table[TOP_TABLE_SIZE-i-1].key;
        if (table[TOP_TABLE_SIZE-i-1].count>0)
            (*num_entries)++;
    }

    vq_top_table(input, num_blocks, output, num_entries);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Public functions
//-----------------------------------------------------------------------------------------------------------------------------


//-----------------------------------------------------------------------------------------------------------------------------
bc1_packed_context* bc1_packed_init(bc1_packed_mem_interface* user_mem)
{
    bc1_packed_mem_interface mem = (user_mem) ? *user_mem : default_allocator();

    bc1_packed_context* ctx = mem.malloc_fn(sizeof(bc1_packed_context), mem.user);

    *ctx = (bc1_packed_context)
    {
        .hashmap = mem.malloc_fn(sizeof(uint32_t) * HASHMAP_SIZE, mem.user),
        .mem = mem
    };

    return ctx;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bc1_packed_maxsize(uint32_t width, uint32_t height)
{
    // very rough
    return (width/4) * (height/4) * 2 * sizeof(bc1_block);
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bc1_packed_compress(bc1_packed_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, void* output)
{
    return 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
void bc1_packed_terminate(bc1_packed_context* ctx)
{
    bc1_packed_mem_interface mem = ctx->mem;

    mem.free_fn(ctx->hashmap, mem.user);
    mem.free_fn(ctx, mem.user);
}