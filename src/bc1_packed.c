#include "bc1_packed.h"
#include "../third_party/lite_encoding.h"
#include <stdlib.h>

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