#include "bc1_packed.h"
#include "../third_party/lite_encoding.h"
#include <stdlib.h>
#include <assert.h>


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

#define HASHMAP_SIZE            (1U << 20U)
#define TOP_TABLE_SIZE          (256U)
#define COLOR_DELTA_NUM_BITS    (7)
#define COLOR_DELTA_OFFSET      (1 << (COLOR_DELTA_NUM_BITS-1))


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

    le_model red_model[2], green_model[2], blue_model[2]; // one set of 3 per endpoint
    le_model mask_model; // 4 bits
    le_model table_reference_model;
    le_model table_difference_model;

    uint32_t top_table[TOP_TABLE_SIZE];
    uint32_t top_table_size;

    // seed "previous block" of every strip: average of the first block of each strip, the two 16-bit 565 colors packed in one uint32
    uint32_t strip_seed;

    // per-strip color predictor: bit s = 1 → strip s predicts endpoint colors from the block of the row above,
    // same zigzag position (visually: the horizontally mirrored column of the previous row), after its first
    // scanline; 0 → zigzag-previous block. the first scanline of every strip is always predicted from the
    // strip seed via the zigzag chain, as there is no previous row within the strip
    uint64_t strip_predictor;

    // bit counters of the range-coded payload, per component
    uint64_t colors_bits, reference_bits, mask_bits, difference_bits;
};

typedef struct bc1_block
{
    uint16_t color[2];
    uint32_t indices;
} bc1_block;

typedef struct byte_stream
{
    uint8_t* buffer;
    size_t length;
    size_t pos;
} byte_stream;

typedef struct vq_cluster
{
    uint32_t bit_diff_count[32];
    uint32_t count;
} vq_cluster;


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
// Unpacks the strip seed into a "previous" block. The seed packs the two endpoint colors in one uint32, color[0] in the low word.
static inline void unpack_strip_seed(uint32_t seed, bc1_block* previous)
{
    previous->color[0] = (uint16_t)seed;
    previous->color[1] = (uint16_t)(seed >> 16);
    previous->indices = 0;
}

//----------------------------------------------------------------------------------------------------------------------------
// branchless absolute value (arithmetic shift: mask is -1 for a negative, 0 otherwise)
static inline uint32_t abs32(int v)
{
    int32_t mask = v >> 31;
    return (uint32_t)((v + mask) ^ mask);
}

//----------------------------------------------------------------------------------------------------------------------------
// total absolute magnitude of the color deltas [current] would emit against [reference]: both endpoints,
// the decorrelated green / red / blue magnitudes, i.e. the six symbols the encoder writes per block.
// used to pick the cheaper per-strip predictor (top block vs zigzag-previous)
static inline uint32_t color_delta_energy(const bc1_block* current, const bc1_block* reference)
{
    uint32_t energy = 0;

    for(uint32_t j=0; j<2; ++j)
    {
        uint8_t current_red, current_green, current_blue;
        uint8_t reference_red, reference_green, reference_blue;

        bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
        bc1_extract_565(reference->color[j], &reference_red, &reference_green, &reference_blue);

        int dred = current_red - reference_red;
        int dgreen = current_green - reference_green;
        int dblue = current_blue - reference_blue;

        energy += abs32(dgreen);

        dgreen /= 2;
        dred -= dgreen;
        dblue -= dgreen;

        energy += abs32(dred) + abs32(dblue);
    }

    return energy;
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
// Returns (popcount << 16) | index of the table entry with the smallest XOR popcount against [bitfield].
// Ties are broken by the fewest differing bytes, then by the largest entry value.
uint32_t nearest32(const uint32_t* table, uint32_t table_size, uint32_t bitfield)
{
    uint32_t scores[TOP_TABLE_SIZE];
    uint32_t masks[TOP_TABLE_SIZE];
    uint32_t i = 0;

#ifdef BC1_PACKED_NEON
    static const uint8_t byte_weight[16] = { 1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8, 1, 2, 4, 8 };
    uint32x4_t bf_vec = vdupq_n_u32(bitfield);
    uint8x16_t one_bytes = vdupq_n_u8(1);
    uint8x16_t weight_bytes = vld1q_u8(byte_weight);

    for (; i + 3 < table_size; i += 4)
    {
        uint32x4_t dict_vec = vld1q_u32(&table[i]);
        uint32x4_t delta = veorq_u32(dict_vec, bf_vec);
        uint8x16_t delta_bytes = vreinterpretq_u8_u32(delta);
        uint8x16_t counts = vcntq_u8(delta_bytes);
        uint16x8_t sum16 = vpaddlq_u8(counts);
        uint32x4_t sum32 = vpaddlq_u16(sum16);

        vst1q_u32(&scores[i], sum32);

        // differing-byte mask per entry: 1 per differing byte, weighted {1, 2, 4, 8} inside the entry then summed
        uint8x16_t nz_bytes = vandq_u8(vcgeq_u8(counts, one_bytes), one_bytes);
        uint16x8_t half = vpaddlq_u8(vmulq_u8(nz_bytes, weight_bytes));
        vst1q_u32(&masks[i], vpaddlq_u16(half));
    }
#else
    const __m128i bf_vec   = _mm_set1_epi32(bitfield);
    const __m128i mask_low = _mm_set1_epi8(0x0F);
    const __m128i lookup   = _mm_setr_epi8(0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4);
    const __m128i zero16   = _mm_setzero_si128();
    const __m128i one8     = _mm_set1_epi8(1);
    const __m128i weights  = _mm_setr_epi8(1,2,4,8, 1,2,4,8, 1,2,4,8, 1,2,4,8);
    const __m128i one16    = _mm_set1_epi16(1);

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
        __m128i final = _mm_madd_epi16(sums, one16);

        _mm_storeu_si128((__m128i*)&scores[i], final);

        // differing-byte mask per entry: 1 per differing byte, weighted {1, 2, 4, 8} inside the entry then summed
        __m128i nz_bytes = _mm_and_si128(_mm_cmpgt_epi8(cnt, zero16), one8);
        __m128i half = _mm_maddubs_epi16(nz_bytes, weights);
        _mm_storeu_si128((__m128i*)&masks[i], _mm_madd_epi16(half, one16));
    }
#endif

    // tail (or whole array on x64)
    for (; i < table_size; ++i)
    {
        uint32_t difference = table[i] ^ bitfield;
        uint32_t score = popcount(difference);
        if (score == 0) 
            return (0 << 16) | (i & 0xffff);
        scores[i] = score;

        uint32_t mask = 0;
        if ((difference & 0x000000FF) != 0) mask |= 1;
        if ((difference & 0x0000FF00) != 0) mask |= 2;
        if ((difference & 0x00FF0000) != 0) mask |= 4;
        if ((difference & 0xFF000000) != 0) mask |= 8;
        masks[i] = mask;
    }

    // find best: lowest popcount, then fewest differing bytes, then the largest table value
    uint32_t best_index = 0;
    uint32_t best_score = UINT32_MAX;
    uint32_t best_mask  = 0;
    for (uint32_t j = 0; j < table_size; ++j)
    {
        uint32_t score = scores[j];
        int mask_bits = popcount(masks[j]);
        if (score < best_score ||
            (score == best_score && (mask_bits < popcount(best_mask) || (mask_bits == popcount(best_mask) && table[j] > table[best_index]))))
        {
            best_score = score;
            best_index = j;
            best_mask  = masks[j];
        }
    }
    return ((best_score&0xffff)<<16) | (best_index&0xffff);
}

//----------------------------------------------------------------------------------------------------------------------------
// One Lloyd refinement round of the top table: assigns every sampled block to its nearest centroid,
// accumulates the XOR difference bit by bit per centroid, and moves each centroid to the bitwise
// majority of its assigned blocks.
//
// [sample_step] of 1 assigns every block (a full round), a larger step samples one block in
// [sample_step] to keep the round cheap, and [start_offset] de-aliases consecutive rounds.
// The bucket feedback drops the skip while the accumulated popcount error is high, so a round
// adapts between the two: with [sample_step] 1 both branches advance by one block.
static void vq_refine_round(const void* input, uint32_t num_blocks, uint32_t* centroids, uint32_t num_centroids,
                            vq_cluster* clusters, uint32_t sample_step, uint32_t start_offset)
{
    for(uint32_t i=0; i<num_centroids; ++i)
    {
        for(uint32_t j=0; j<32; ++j)
            clusters[i].bit_diff_count[j] = 0;

        clusters[i].count = 0;
    }

    uint32_t bucket = 0;
    const uint32_t threshold = 16;

    // find the best cluster for each sampled block, jittering the first block
    for(uint32_t block_index=start_offset; block_index<num_blocks;)
    {
        const bc1_block* b = (const bc1_block*) input + block_index;

        uint32_t result = nearest32(centroids, num_centroids, b->indices);
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

    // move centroid to the bitwise majority of its assigned blocks
    for(uint32_t i=0; i<num_centroids; ++i)
    {
        if (clusters[i].count>0)
        {
            for(uint32_t bit=0; bit<32; ++bit)
                if (clusters[i].bit_diff_count[bit] > (clusters[i].count/2))
                    centroids[i] ^= (1u << bit);

        }
    }
}

//----------------------------------------------------------------------------------------------------------------------------
void vq_top_table(const void* input, uint32_t num_blocks, uint32_t* output, uint32_t* num_entries)
{
    // static array of odd steps to avoid aliasing and branches
    // first iteration is always 1 to ensure 100% initial coverage, and the two final full-assignment
    // rounds (step 1) converge the centroids on every block: the jittered sampling of the middle
    // rounds explores a better local optimum, the full rounds settle it (exp 15 in failed_experiments.md)
    static const uint32_t steps[6] = { 1, 5, 11, 17, 1, 1 };

    uint32_t centroids[TOP_TABLE_SIZE];
    vq_cluster clusters[TOP_TABLE_SIZE];

    // use the top table entries as candidate for the cluster
    for(uint32_t i=0; i<*num_entries; ++i)
        centroids[i] = output[i];

    // multiple iterations to stabilize the clusters
    for(uint32_t iteration=0; iteration<6; ++iteration)
        vq_refine_round(input, num_blocks, centroids, *num_entries, clusters, steps[iteration], iteration % steps[iteration]);

    uint32_t num_clusters = 0;

    // fill the table with centroid
    for(uint32_t i=0; i<*num_entries; ++i)
        if (clusters[i].count > 1)
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

    // an empty top table corrupts the stream (the size is stored as size-1 in a byte, 0 reads back as 256 entries),
    // fall back to the single most frequent exact pattern, the hashmap still holds the exact pattern counts
    if (*num_entries == 0)
    {
        uint32_t best_key = 0;
        uint32_t best_count = 0;

        for (uint32_t i = 0; i < HASHMAP_SIZE; ++i)
        {
            if (hashmap[i].count == 0)
                continue;

            if ((hashmap[i].count > best_count) || ((hashmap[i].count == best_count) && (hashmap[i].key > best_key)))
            {
                best_count = hashmap[i].count;
                best_key = hashmap[i].key;
            }
        }

        output[0] = best_key;
        *num_entries = 1;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
void init_static_models(bc1_packed_context* ctx, const void* input, uint32_t width_blocks, uint32_t strip_width)
{
    uint32_t hist_red[2][1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_green[2][1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_blue[2][1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_reference[TOP_TABLE_SIZE];
    uint32_t hist_mask[16];
    uint32_t hist_difference[LE_ALPHABET_SIZE];

    memset(hist_red, 0, sizeof(hist_red));
    memset(hist_green, 0, sizeof(hist_green));
    memset(hist_blue, 0, sizeof(hist_blue));
    memset(hist_reference, 0, sizeof(hist_reference));
    memset(hist_mask, 0, sizeof(hist_mask));
    memset(hist_difference, 0, sizeof(hist_difference));

    // pre-pass: the average of the first block of each strip becomes the seed "previous" block used to start every strip
    uint32_t sums[6] = { 0, 0, 0, 0, 0, 0 }; // r, g, b of color[0] and color[1]

    for(uint32_t strip_index=0; strip_index<BC1_PACKED_NUM_STRIPS; ++strip_index)
    {
        uint32_t start_y = strip_index * strip_width;
        uint32_t zigzag_x = (start_y & 1) ? 0 : width_blocks - 1;

        const bc1_block* first = (const bc1_block*) input + (start_y * width_blocks) + zigzag_x;

        for(uint32_t j=0; j<2; ++j)
        {
            uint8_t r, g, b;
            bc1_extract_565(first->color[j], &r, &g, &b);

            sums[j*3 + 0] += r;
            sums[j*3 + 1] += g;
            sums[j*3 + 2] += b;
        }
    }

    uint16_t average0 = bc1_pack_565((uint8_t)(sums[0] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[1] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[2] / BC1_PACKED_NUM_STRIPS));
    uint16_t average1 = bc1_pack_565((uint8_t)(sums[3] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[4] / BC1_PACKED_NUM_STRIPS), (uint8_t)(sums[5] / BC1_PACKED_NUM_STRIPS));
    ctx->strip_seed = ((uint32_t)average1 << 16) | (uint32_t)average0;

    // pre-pass: per strip, pick the cheaper color predictor, zigzag-previous vs the row above (same zigzag
    // position) after the first scanline, by the total absolute magnitude of the color deltas each would emit
    ctx->strip_predictor = 0;
    for(uint32_t strip_index=0; strip_index<BC1_PACKED_NUM_STRIPS; ++strip_index)
    {
        uint64_t energy_previous = 0;
        uint64_t energy_top = 0;

        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);

        uint32_t start_y = strip_index * strip_width;
        for(uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for(uint32_t x = 0; x < width_blocks; ++x)
            {
                uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;
                const bc1_block* current = (const bc1_block*) input + (y * width_blocks) + zigzag_x;

                const bc1_block* top = &previous;
                if (y > start_y)
                    top = (const bc1_block*) input + (y - 1) * width_blocks + zigzag_x; // same zigzag position, row above

                energy_previous += color_delta_energy(current, &previous);
                energy_top += color_delta_energy(current, top);

                previous = *current;
            }
        }

        if (energy_top < energy_previous)
            ctx->strip_predictor |= (uint64_t)1 << strip_index;
    }

    for(uint32_t strip_index=0; strip_index<BC1_PACKED_NUM_STRIPS; ++strip_index)
    {
        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);
        const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
        uint32_t start_y = strip_index * strip_width;
        for(uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for(uint32_t x = 0; x < width_blocks; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;

                const bc1_block* current = (const bc1_block*) input + (y * width_blocks) + zigzag_x;

                // first scanline of a strip is always predicted from the seed via the zigzag-previous chain,
                // after that a top-flagged strip is predicted from the row above, same zigzag position
                const bc1_block* color_reference = &previous;
                if (use_top && y > start_y)
                    color_reference = (const bc1_block*) input + (y - 1) * width_blocks + zigzag_x;

                for(uint32_t j=0; j<2; ++j)
                {
                    uint8_t current_red, current_green, current_blue;
                    uint8_t reference_red, reference_green, reference_blue;

                    bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
                    bc1_extract_565(color_reference->color[j], &reference_red, &reference_green, &reference_blue);

                    int dred = current_red - reference_red;
                    int dgreen = current_green - reference_green;
                    int dblue = current_blue - reference_blue;

                    hist_green[j][dgreen + COLOR_DELTA_OFFSET]++;

                    dgreen /= 2;
                    dred -= dgreen;
                    dblue -= dgreen;

                    hist_red[j][dred + COLOR_DELTA_OFFSET]++;
                    hist_blue[j][dblue + COLOR_DELTA_OFFSET]++;
                }

                uint8_t reference = nearest32(ctx->top_table, ctx->top_table_size, current->indices) & 0xff;
                hist_reference[reference]++;

                uint32_t difference = current->indices ^ ctx->top_table[reference];
                uint32_t mask = 0;
                if ((difference & 0x000000FF) != 0) mask |= 1;
                if ((difference & 0x0000FF00) != 0) mask |= 2;
                if ((difference & 0x00FF0000) != 0) mask |= 4;
                if ((difference & 0xFF000000) != 0) mask |= 8;

                hist_mask[mask]++;

                for(uint32_t j=0; j<4; ++j)
                    if (mask & (1u << j))
                        hist_difference[(difference >> (j*8)) & 0xff]++;

                previous = *current;
            }
        }
    }

    for(uint32_t j=0; j<2; ++j)
    {
        le_static_model_init(&ctx->red_model[j], hist_red[j], 1<<COLOR_DELTA_NUM_BITS);
        le_static_model_init(&ctx->blue_model[j], hist_blue[j], 1<<COLOR_DELTA_NUM_BITS);
        le_static_model_init(&ctx->green_model[j], hist_green[j], 1<<COLOR_DELTA_NUM_BITS);
    }

    le_static_model_init(&ctx->table_reference_model, hist_reference, TOP_TABLE_SIZE);
    le_static_model_init(&ctx->mask_model, hist_mask, 16);
    le_static_model_init(&ctx->table_difference_model, hist_difference, LE_ALPHABET_SIZE);
}


//-----------------------------------------------------------------------------------------------------------------------------
static inline void stream_align(byte_stream* stream, size_t alignment)
{
    // align with 0 value padding
    while ((stream->pos % alignment) != 0 && stream->pos < stream->length)
        stream->buffer[stream->pos++] = 0;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline void read_stream_align(byte_stream* stream, size_t power_two_alignment)
{
    size_t mask = power_two_alignment - 1;
    stream->pos = (stream->pos + mask) & ~(mask);
}

//-----------------------------------------------------------------------------------------------------------------------------
// Adds to [count] the bits written to [stream] since the last call, and updates [bit_pos] to the current bit position.
// [bit_pos] must be initialized to 0 after le_begin_encode.
static inline void stream_bits_since(uint64_t* bit_pos, const le_stream* stream, uint64_t* count)
{
    const uint64_t pos = (uint64_t)stream->position * 8 + stream->bits_available;
    *count += pos - *bit_pos;
    *bit_pos = pos;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool save_static_model(const le_model* model, byte_stream* stream)
{
    // a model whose histogram is all zero has no symbols but the stream format can only store a count of 1 to 256,
    // so we save a single dummy entry. alphabet[0] is always initialized by le_static_model_init and the decoder
    // never reads it because the encoder never encoded with it.
    uint32_t num_symbols = (model->num_symbols > 0) ? model->num_symbols : 1;

    size_t model_size = 3 + num_symbols;

    if (stream->pos + model_size > stream->length)
        return false;

    stream->buffer[stream->pos++] = (uint8_t)(num_symbols - 1); // minus 1 to store 256 symbols count on uint8_t
    stream->buffer[stream->pos++] = model->k;
    stream->buffer[stream->pos++] = model->q_escape;

    for(uint32_t i=0; i<num_symbols; ++i)
        stream->buffer[stream->pos++] = model->alphabet[i];

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool load_static_model(le_model* model, byte_stream* stream)
{
    // mirror of save_static_model
    if (stream->pos + 3 > stream->length)
        return false;

    uint32_t num_symbols = stream->buffer[stream->pos++] + 1;
    uint8_t k = stream->buffer[stream->pos++];
    uint8_t q_escape = stream->buffer[stream->pos++];

    if (k >= LE_Q_ESCAPE_SIZE)
        k = (uint8_t)(LE_Q_ESCAPE_SIZE - 1); 

    if (stream->pos + num_symbols > stream->length)
        return false;

    le_static_model_load(model, &stream->buffer[stream->pos], num_symbols, k, q_escape);
    stream->pos += num_symbols;

    return true;
}


//-----------------------------------------------------------------------------------------------------------------------------
// Public functions
//-----------------------------------------------------------------------------------------------------------------------------


//-----------------------------------------------------------------------------------------------------------------------------
bc1_packed_context* bc1_packed_init(bc1_packed_mem_interface* user_mem)
{
    bc1_packed_mem_interface mem = (user_mem) ? *user_mem : default_allocator();

    bc1_packed_context* ctx = mem.malloc_fn(sizeof(bc1_packed_context), mem.user);
    if (ctx == NULL)
        return NULL;

    *ctx = (bc1_packed_context)
    {
        .hashmap = mem.malloc_fn(sizeof(hashmap_entry) * HASHMAP_SIZE, mem.user),
        .mem = mem
    };

    if (ctx->hashmap == NULL)
    {
        mem.free_fn(ctx, mem.user);
        return NULL;
    }

    return ctx;
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bc1_packed_maxsize(uint32_t width, uint32_t height)
{
    // very rough
    return (width/4) * (height/4) * 2 * sizeof(bc1_block);
}

//-----------------------------------------------------------------------------------------------------------------------------
size_t bc1_packed_compress(bc1_packed_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length)
{
    if (width < 16 || height < 256 || !bc1_image || !ctx || !output)
        return 0;

    // reset stats, they are only valid after a successful compression
    ctx->colors_bits = 0;
    ctx->reference_bits = 0;
    ctx->mask_bits = 0;
    ctx->difference_bits = 0;

    const uint32_t num_blocks = (width*height) / 16;
    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;
    const uint32_t strip_width = height_blocks / BC1_PACKED_NUM_STRIPS;

    build_top_table(ctx->hashmap, bc1_image, num_blocks, ctx->top_table, &ctx->top_table_size);
    init_static_models(ctx, bc1_image, width_blocks, strip_width);

    byte_stream stream = {.buffer = output, .length = output_length, .pos = 0};

    // store models
    for(uint32_t j=0; j<2; ++j)
    {
        if (!save_static_model(&ctx->red_model[j], &stream)) return 0;
        if (!save_static_model(&ctx->green_model[j], &stream)) return 0;
        if (!save_static_model(&ctx->blue_model[j], &stream)) return 0;
    }
    if (!save_static_model(&ctx->table_reference_model, &stream)) return 0;
    if (!save_static_model(&ctx->mask_model, &stream)) return 0;
    if (!save_static_model(&ctx->table_difference_model, &stream)) return 0;

    // top-table + strip seed + predictor flags
    if (stream.pos + 1 + 3 + sizeof(uint32_t) + 8 + ctx->top_table_size * sizeof(uint32_t) > stream.length)
        return 0;

    stream.buffer[stream.pos++] = (uint8_t) (ctx->top_table_size - 1); // there is no zero toptable, so minus 1 to fit in a uint8_t
    stream_align(&stream, sizeof(uint32_t));

    for(uint32_t j=0; j<4; ++j)
        stream.buffer[stream.pos++] = (uint8_t) ((ctx->strip_seed >> (j*8)) & 0xff);

    for(uint32_t j=0; j<8; ++j)
        stream.buffer[stream.pos++] = (uint8_t) ((ctx->strip_predictor >> (j*8)) & 0xff);

    for(uint32_t i=0; i<ctx->top_table_size; ++i)
        for(uint32_t j=0; j<4; ++j)
            stream.buffer[stream.pos++] = (ctx->top_table[i] >> (j*8)) & 0xff;

    stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * BC1_PACKED_NUM_STRIPS;

    // check if we have enough space
    if (stream.pos + strips_offset_array_size >= stream.length)
        return 0;
    
    // keep a pointer to strips offset
    uint16_t* strips_offset = (uint16_t*) &stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    // compressed data is aligned on 4 bytes to allow fast gpu loading
    stream_align(&stream, sizeof(uint32_t));
    size_t previous_offset = stream.pos;

    for(uint32_t strip_index=0; strip_index<BC1_PACKED_NUM_STRIPS; ++strip_index)
    {
        le_stream compressed_stream;
        le_init(&compressed_stream, &stream.buffer[stream.pos], stream.length - stream.pos);
        le_begin_encode(&compressed_stream);

        // strip offset is counted in dword, based on the previous one (delta compression)
        strips_offset[strip_index] = (uint16_t)((stream.pos - previous_offset) / sizeof(uint32_t));
        previous_offset = stream.pos;

        uint64_t bit_pos = 0; // current bit position, used to measure the per-component stats

        bc1_block previous;
        unpack_strip_seed(ctx->strip_seed, &previous);
        const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
        uint32_t start_y = strip_index * strip_width;
        for(uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for(uint32_t x = 0; x < width_blocks; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;
                const bc1_block* current = (const bc1_block*) bc1_image + (y * width_blocks) + zigzag_x;

                // first scanline of a strip is always predicted from the seed via the zigzag-previous chain,
                // after that a top-flagged strip is predicted from the row above, same zigzag position
                const bc1_block* color_reference = &previous;
                if (use_top && y > start_y)
                    color_reference = (const bc1_block*) bc1_image + (y - 1) * width_blocks + zigzag_x;

                for(uint32_t j=0; j<2; ++j)
                {
                    uint8_t current_red, current_green, current_blue;
                    uint8_t reference_red, reference_green, reference_blue;

                    bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
                    bc1_extract_565(color_reference->color[j], &reference_red, &reference_green, &reference_blue);

                    int dred = current_red - reference_red;
                    int dgreen = current_green - reference_green;
                    int dblue = current_blue - reference_blue;

                    le_encode_symbol(&compressed_stream, &ctx->green_model[j], dgreen + COLOR_DELTA_OFFSET);

                    dgreen /= 2;
                    dred -= dgreen;
                    dblue -= dgreen;

                    le_encode_symbol(&compressed_stream, &ctx->red_model[j], dred + COLOR_DELTA_OFFSET);
                    le_encode_symbol(&compressed_stream, &ctx->blue_model[j], dblue + COLOR_DELTA_OFFSET);
                }
                stream_bits_since(&bit_pos, &compressed_stream, &ctx->colors_bits);

                uint8_t reference = nearest32(ctx->top_table, ctx->top_table_size, current->indices) & 0xff;

                le_encode_symbol(&compressed_stream, &ctx->table_reference_model, reference);
                stream_bits_since(&bit_pos, &compressed_stream, &ctx->reference_bits);

                uint32_t difference = current->indices ^ ctx->top_table[reference];
                uint32_t mask = 0;
                if ((difference & 0x000000FF) != 0) mask |= 1;
                if ((difference & 0x0000FF00) != 0) mask |= 2;
                if ((difference & 0x00FF0000) != 0) mask |= 4;
                if ((difference & 0xFF000000) != 0) mask |= 8;

                le_encode_symbol(&compressed_stream, &ctx->mask_model, mask);
                stream_bits_since(&bit_pos, &compressed_stream, &ctx->mask_bits);

                for(uint32_t j=0; j<4; ++j)
                    if (mask & (1u << j))
                        le_encode_symbol(&compressed_stream, &ctx->table_difference_model, (difference >> (j*8)) & 0xff);
                stream_bits_since(&bit_pos, &compressed_stream, &ctx->difference_bits);

                previous = *current;
            }
        }

        size_t strip_size = le_end_encode(&compressed_stream);
        assert(strip_size < 262144); // if it happens, there is a problem somewhere else

        stream.pos += strip_size;

        stream_align(&stream, sizeof(uint32_t));

        if (compressed_stream.status == LE_BUFFER_OVERRUN)
            return 0;
    }

    return stream.pos;
}

//-----------------------------------------------------------------------------------------------------------------------------
bool bc1_packed_decompress(bc1_packed_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, void* output, uint32_t strip_index)
{
    if (width < 16 || height < 256 || !input || !ctx || !output || strip_index >= BC1_PACKED_NUM_STRIPS)
        return false;

    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;
    const uint32_t strip_width = height_blocks / BC1_PACKED_NUM_STRIPS;

    byte_stream stream = {.buffer = (uint8_t *) input, .length = input_length, .pos = 0};

    for(uint32_t j=0; j<2; ++j)
    {
        if (!load_static_model(&ctx->red_model[j], &stream)) return false;
        if (!load_static_model(&ctx->green_model[j], &stream)) return false;
        if (!load_static_model(&ctx->blue_model[j], &stream)) return false;
    }
    if (!load_static_model(&ctx->table_reference_model, &stream)) return false;
    if (!load_static_model(&ctx->mask_model, &stream)) return false;
    if (!load_static_model(&ctx->table_difference_model, &stream)) return false;

    // top-table
    if (stream.pos >= stream.length)
        return false;

    ctx->top_table_size = (uint32_t)stream.buffer[stream.pos++] + 1;
    read_stream_align(&stream, sizeof(uint32_t));

    if (stream.pos + sizeof(uint32_t) + 8 > stream.length)
        return false;

    ctx->strip_seed = 0;
    for(uint32_t j=0; j<4; ++j)
        ctx->strip_seed |= (uint32_t)stream.buffer[stream.pos++] << (j*8);

    ctx->strip_predictor = 0;
    for(uint32_t j=0; j<8; ++j)
        ctx->strip_predictor |= (uint64_t)stream.buffer[stream.pos++] << (j*8);

    for(uint32_t i=0; i<ctx->top_table_size; ++i)
    {
        ctx->top_table[i] = 0;
    
        for(uint32_t j=0; j<4; ++j)
            ctx->top_table[i] |= stream.buffer[stream.pos++] << (j*8);
    }

    // strip offsets
    read_stream_align(&stream, sizeof(uint16_t));
    size_t strips_offset_array_size = sizeof(uint16_t) * BC1_PACKED_NUM_STRIPS;

    if (stream.pos + strips_offset_array_size >= stream.length)
        return false;
    
    uint16_t* strips_offset_array = (uint16_t*) &stream.buffer[stream.pos];
    stream.pos += strips_offset_array_size;

    read_stream_align(&stream, sizeof(uint32_t));

    // compute the current strip offset and setup compressed stream
    size_t strip_offset = stream.pos;
    for(uint32_t i=0; i<=strip_index; ++i)
        strip_offset += strips_offset_array[i] * sizeof(uint32_t);

    assert(stream.length > strip_offset);

    le_stream compressed_stream;
    le_init(&compressed_stream, &stream.buffer[strip_offset], stream.length - strip_offset);
    le_begin_decode(&compressed_stream);
    
    bc1_block previous;
    unpack_strip_seed(ctx->strip_seed, &previous);
    const bool use_top = ((ctx->strip_predictor >> strip_index) & 1) != 0;
    uint32_t start_y = strip_index * strip_width;
    for(uint32_t y = start_y; y < start_y + strip_width; ++y)
    {
        for(uint32_t x = 0; x < width_blocks; ++x)
        {
            uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;
            bc1_block* current = (bc1_block*) output + (y * width_blocks) + zigzag_x;

            // first scanline of a strip is always predicted from the seed via the zigzag-previous chain,
            // after that a top-flagged strip is predicted from the row above, same zigzag position
            const bc1_block* color_reference = &previous;
            if (use_top && y > start_y)
                color_reference = (const bc1_block*) output + (y - 1) * width_blocks + zigzag_x;

            for(uint32_t j=0; j<2; ++j)
            {
                uint8_t reference_red, reference_green, reference_blue;
                bc1_extract_565(color_reference->color[j], &reference_red, &reference_green, &reference_blue);

                uint8_t delta_green = le_decode_symbol(&compressed_stream, &ctx->green_model[j]);
                uint8_t delta_red = le_decode_symbol(&compressed_stream, &ctx->red_model[j]);
                uint8_t delta_blue = le_decode_symbol(&compressed_stream, &ctx->blue_model[j]);

                // red and blue delta are based on green delta
                int dgreen_orig = (int)delta_green - COLOR_DELTA_OFFSET;
                int current_green_value = reference_green + dgreen_orig;
                int dgreen_halved = dgreen_orig / 2;

                int dred_orig = ((int)delta_red - COLOR_DELTA_OFFSET) + dgreen_halved;
                int dblue_orig = ((int)delta_blue - COLOR_DELTA_OFFSET) + dgreen_halved;

                int current_red_value = reference_red + dred_orig;
                int current_blue_value = reference_blue + dblue_orig;

                current->color[j] = bc1_pack_565((uint8_t)current_red_value, (uint8_t)current_green_value, (uint8_t)current_blue_value);
            }
            
            // indices difference with top table
            uint32_t reference = le_decode_symbol(&compressed_stream, &ctx->table_reference_model);
            uint32_t mask = le_decode_symbol(&compressed_stream, &ctx->mask_model);

            uint32_t difference=0;
            for(uint32_t j=0; j<4; ++j)
                if (mask & (1 << j))
                    difference = difference | (le_decode_symbol(&compressed_stream, &ctx->table_difference_model) << (j*8));

            current->indices =  difference ^ ctx->top_table[reference];

            previous = *current;
        }
    }

    le_end_decode(&compressed_stream);

    return compressed_stream.status == LE_OK;
}

//-----------------------------------------------------------------------------------------------------------------------------
void bc1_packed_get_stats(bc1_packed_context* ctx, bc1_packed_stats* stats)
{
    *stats = (bc1_packed_stats)
    {
        .blue_k = { ctx->blue_model[0].k, ctx->blue_model[1].k },
        .difference_k = ctx->table_difference_model.k,
        .green_k = { ctx->green_model[0].k, ctx->green_model[1].k },
        .red_k = { ctx->red_model[0].k, ctx->red_model[1].k },
        .mask_k = ctx->mask_model.k,
        .reference_k = ctx->table_reference_model.k,
        .top_table_size = ctx->top_table_size,
        .colors_bytes = (uint32_t)((ctx->colors_bits + 7) / 8),
        .reference_bytes = (uint32_t)((ctx->reference_bits + 7) / 8),
        .mask_bytes = (uint32_t)((ctx->mask_bits + 7) / 8),
        .difference_bytes = (uint32_t)((ctx->difference_bits + 7) / 8)
    };
}

//-----------------------------------------------------------------------------------------------------------------------------
void bc1_packed_terminate(bc1_packed_context* ctx)
{
    bc1_packed_mem_interface mem = ctx->mem;

    mem.free_fn(ctx->hashmap, mem.user);
    mem.free_fn(ctx, mem.user);
}
