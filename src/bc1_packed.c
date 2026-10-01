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

    le_model red_model, green_model, blue_model;
    le_model mask_model; // 4 bits
    le_model table_reference_model;
    le_model table_difference_model;

    uint32_t top_table[TOP_TABLE_SIZE];
    uint32_t top_table_size;
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
}

//-----------------------------------------------------------------------------------------------------------------------------
void init_static_models(bc1_packed_context* ctx, const void* input, uint32_t width_blocks, uint32_t strip_width)
{
    uint32_t hist_red[1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_green[1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_blue[1<<COLOR_DELTA_NUM_BITS];
    uint32_t hist_reference[TOP_TABLE_SIZE];
    uint32_t hist_mask[16];
    uint32_t hist_difference[LE_ALPHABET_SIZE];

    memset(hist_red, 0, sizeof(hist_red));
    memset(hist_green, 0, sizeof(hist_green));
    memset(hist_blue, 0, sizeof(hist_blue));
    memset(hist_reference, 0, sizeof(hist_reference));
    memset(hist_mask, 0, sizeof(hist_mask));
    memset(hist_difference, 0, sizeof(hist_difference));

    for(uint32_t strip_index=0; strip_index<BC1_PACKED_NUM_STRIPS; ++strip_index)
    {
        bc1_block previous = {.color = {bc1_pack_565(8, 16, 8), bc1_pack_565(24, 48, 24)}}; // TODO : better default previous, average?
        uint32_t start_y = strip_index * strip_width;
        for(uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for(uint32_t x = 0; x < width_blocks; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;

                const bc1_block* current = (const bc1_block*) input + (y * width_blocks) + zigzag_x;

                for(uint32_t j=0; j<2; ++j)
                {
                    uint8_t current_red, current_green, current_blue;
                    uint8_t previous_red, previous_green, previous_blue;

                    bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
                    bc1_extract_565(previous.color[j], &previous_red, &previous_green, &previous_blue);

                    int dred = current_red - previous_red;
                    int dgreen = current_green - previous_green;
                    int dblue = current_blue - previous_blue;

                    hist_green[dgreen + COLOR_DELTA_OFFSET]++;

                    dgreen /= 2;
                    dred -= dgreen;
                    dblue -= dgreen;

                    hist_red[dred + COLOR_DELTA_OFFSET]++;
                    hist_blue[dblue + COLOR_DELTA_OFFSET]++;
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

    le_static_model_init(&ctx->red_model, hist_red, 1<<COLOR_DELTA_NUM_BITS);
    le_static_model_init(&ctx->blue_model, hist_blue, 1<<COLOR_DELTA_NUM_BITS);
    le_static_model_init(&ctx->green_model, hist_green, 1<<COLOR_DELTA_NUM_BITS);

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
static inline bool save_static_model(const le_model* model, byte_stream* stream)
{
    // a model whose histogram is all zero has no symbols but the stream format can only store a count of 1 to 256,
    // so we save a single dummy entry. alphabet[0] is always initialized by le_static_model_init and the decoder
    // never reads it because the encoder never encoded with it.
    uint32_t num_symbols = (model->num_symbols > 0) ? model->num_symbols : 1;

    size_t model_size = 2 + num_symbols;

    if (stream->pos + model_size > stream->length)
        return false;

    stream->buffer[stream->pos++] = (uint8_t)(num_symbols - 1); // minus 1 to store 256 symbols count on uint8_t
    stream->buffer[stream->pos++] = model->k;

    for(uint32_t i=0; i<num_symbols; ++i)
        stream->buffer[stream->pos++] = model->alphabet[i];

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
static inline bool load_static_model(le_model* model, byte_stream* stream)
{
    // mirror of save_static_model
    if (stream->pos + 2 > stream->length)
        return false;

    uint32_t num_symbols = stream->buffer[stream->pos++] + 1;
    uint8_t k = stream->buffer[stream->pos++];

    if (k >= LE_Q_ESCAPE_SIZE)
        k = (uint8_t)(LE_Q_ESCAPE_SIZE - 1); 

    if (stream->pos + num_symbols > stream->length)
        return false;

    le_static_model_load(model, &stream->buffer[stream->pos], num_symbols, k);
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

    const uint32_t num_blocks = (width*height) / 16;
    const uint32_t height_blocks = height / 4;
    const uint32_t width_blocks = width / 4;
    const uint32_t strip_width = height_blocks / BC1_PACKED_NUM_STRIPS;

    build_top_table(ctx->hashmap, bc1_image, num_blocks, ctx->top_table, &ctx->top_table_size);
    init_static_models(ctx, bc1_image, width_blocks, strip_width);

    byte_stream stream = {.buffer = output, .length = output_length, .pos = 0};

    // store models
    if (!save_static_model(&ctx->red_model, &stream)) return 0;
    if (!save_static_model(&ctx->green_model, &stream)) return 0;
    if (!save_static_model(&ctx->blue_model, &stream)) return 0;
    if (!save_static_model(&ctx->table_reference_model, &stream)) return 0;
    if (!save_static_model(&ctx->mask_model, &stream)) return 0;
    if (!save_static_model(&ctx->table_difference_model, &stream)) return 0;

    // top-table
    if (stream.pos + ctx->top_table_size + 1 > stream.length)
        return 0;

    stream.buffer[stream.pos++] = (uint8_t) (ctx->top_table_size - 1); // there is no zero toptable, so minus 1 to fit in a uint8_t
    stream_align(&stream, sizeof(uint32_t));

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

        bc1_block previous = {.color = {bc1_pack_565(8, 16, 8), bc1_pack_565(24, 48, 24)}};
        uint32_t start_y = strip_index * strip_width;
        for(uint32_t y = start_y; y < start_y + strip_width; ++y)
        {
            for(uint32_t x = 0; x < width_blocks; ++x)
            {
                // zig-zag pattern delta compression for colors
                uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;
                const bc1_block* current = (const bc1_block*) bc1_image + (y * width_blocks) + zigzag_x;

                for(uint32_t j=0; j<2; ++j)
                {
                    uint8_t current_red, current_green, current_blue;
                    uint8_t previous_red, previous_green, previous_blue;

                    bc1_extract_565(current->color[j], &current_red, &current_green, &current_blue);
                    bc1_extract_565(previous.color[j], &previous_red, &previous_green, &previous_blue);

                    int dred = current_red - previous_red;
                    int dgreen = current_green - previous_green;
                    int dblue = current_blue - previous_blue;

                    le_encode_symbol(&compressed_stream, &ctx->green_model, dgreen + COLOR_DELTA_OFFSET);

                    dgreen /= 2;
                    dred -= dgreen;
                    dblue -= dgreen;

                    le_encode_symbol(&compressed_stream, &ctx->red_model, dred + COLOR_DELTA_OFFSET);
                    le_encode_symbol(&compressed_stream, &ctx->blue_model, dblue + COLOR_DELTA_OFFSET);
                }

                uint8_t reference = nearest32(ctx->top_table, ctx->top_table_size, current->indices) & 0xff;

                le_encode_symbol(&compressed_stream, &ctx->table_reference_model, reference);

                uint32_t difference = current->indices ^ ctx->top_table[reference];
                uint32_t mask = 0;
                if ((difference & 0x000000FF) != 0) mask |= 1;
                if ((difference & 0x0000FF00) != 0) mask |= 2;
                if ((difference & 0x00FF0000) != 0) mask |= 4;
                if ((difference & 0xFF000000) != 0) mask |= 8;

                le_encode_symbol(&compressed_stream, &ctx->mask_model, mask);

                for(uint32_t j=0; j<4; ++j)
                    if (mask & (1u << j))
                        le_encode_symbol(&compressed_stream, &ctx->table_difference_model, (difference >> (j*8)) & 0xff);

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

    if (!load_static_model(&ctx->red_model, &stream)) return false;
    if (!load_static_model(&ctx->green_model, &stream)) return false;
    if (!load_static_model(&ctx->blue_model, &stream)) return false;
    if (!load_static_model(&ctx->table_reference_model, &stream)) return false;
    if (!load_static_model(&ctx->mask_model, &stream)) return false;
    if (!load_static_model(&ctx->table_difference_model, &stream)) return false;

    // top-table
    if (stream.pos >= stream.length)
        return false;

    ctx->top_table_size = (uint32_t)stream.buffer[stream.pos++] + 1;
    read_stream_align(&stream, sizeof(uint32_t));

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
    
    bc1_block previous = {.color = {bc1_pack_565(8, 16, 8), bc1_pack_565(24, 48, 24)}};
    uint32_t start_y = strip_index * strip_width;
    for(uint32_t y = start_y; y < start_y + strip_width; ++y)
    {
        for(uint32_t x = 0; x < width_blocks; ++x)
        {
            uint32_t zigzag_x = (y&1) ? x : width_blocks - x - 1;
            bc1_block* current = (bc1_block*) output + (y * width_blocks) + zigzag_x;

            for(uint32_t j=0; j<2; ++j)
            {
                uint8_t reference_red, reference_green, reference_blue;
                bc1_extract_565(previous.color[j], &reference_red, &reference_green, &reference_blue);

                uint8_t delta_green = le_decode_symbol(&compressed_stream, &ctx->green_model);
                uint8_t delta_red = le_decode_symbol(&compressed_stream, &ctx->red_model);
                uint8_t delta_blue = le_decode_symbol(&compressed_stream, &ctx->blue_model);

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
        .blue_k = ctx->blue_model.k,
        .difference_k = ctx->table_difference_model.k,
        .green_k = ctx->green_model.k,
        .red_k = ctx->red_model.k,
        .mask_k = ctx->mask_model.k,
        .reference_k = ctx->table_reference_model.k,
        .top_table_size = ctx->top_table_size
    };
}

//-----------------------------------------------------------------------------------------------------------------------------
void bc1_packed_terminate(bc1_packed_context* ctx)
{
    bc1_packed_mem_interface mem = ctx->mem;

    mem.free_fn(ctx->hashmap, mem.user);
    mem.free_fn(ctx, mem.user);
}
