#ifndef BC1_PACKED
#define BC1_PACKED


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BC1_PACKED_NUM_STRIPS              (64)


//----------------------------------------------------------------------------------------------------------------------------
// Memory interface, used for the large working buffers of the compressor.
// If NULL is passed to bc1_packed_init, a standard malloc/realloc/free backend is used.

typedef struct bc1_packed_mem_interface
{
    void*  (*malloc_fn)(size_t size, void* user);
    void*  (*realloc_fn)(void* old_ptr, size_t old_size, size_t new_size, void* user);
    void   (*free_fn)(void* ptr, void* user);
    void*   user;
} bc1_packed_mem_interface;


typedef struct bc1_packed_context bc1_packed_context;

typedef struct bc1_packed_stats
{
    uint32_t top_table_size;
    uint8_t red_k, green_k, blue_k;
    uint8_t mask_k, reference_k, difference_k;

    // size in bytes of each range-coded component, valid after a successful bc1_packed_compress
    uint32_t colors_bytes;      // 6 color delta symbols per block
    uint32_t reference_bytes;   // top-table reference symbol per block
    uint32_t mask_bytes;        // index-residual mask symbol per block
    uint32_t difference_bytes;  // 0-4 index-residual bytes per block
} bc1_packed_stats;


//----------------------------------------------------------------------------------------------------------------------------
// Creates a context. Returns NULL if the allocation fails.
bc1_packed_context* bc1_packed_init(bc1_packed_mem_interface* mem);

//----------------------------------------------------------------------------------------------------------------------------
// Frees the context and all its working buffers. NULL is accepted.
void bc1_packed_terminate(bc1_packed_context* ctx);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given texture, in bytes.
// The [output] buffer of bc1_packed_compress must be at least this large.
size_t bc1_packed_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC1 texture (width x height pixels, must be a multiple of 4) into [output].
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bc1_packed_compress(bc1_packed_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length);

//----------------------------------------------------------------------------------------------------------------------------
// Extracts statistics after compression
// The *_bytes fields are valid after a successful bc1_packed_compress, their sum is the range-coded payload size,
// the stream additionally contains the static header (models, top-table, strip offsets)
void bc1_packed_get_stats(bc1_packed_context* ctx, bc1_packed_stats* stats);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
// 
// Decompress one strip of the image, for a complete image this function should be called BC1_PACKED_NUM_STRIPS times
//      [input]         compressed data (output of bc1_packed_compress)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC1 image
//      [output]        must be big enough to contain a BC1 image of width x height
//      [strip_index]   index of the strip, must be [0; BC1_PACKED_NUM_STRIPS[
// Returns false on error otherwise true
bool bc1_packed_decompress(bc1_packed_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, void* output, uint32_t strip_index);

#endif

