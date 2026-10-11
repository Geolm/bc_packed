#ifndef BC_PACKED
#define BC_PACKED


#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


//----------------------------------------------------------------------------------------------------------------------------
// Memory interface, used for the large working buffers of the compressor.
// If NULL is passed to bcp_init, a standard malloc/realloc/free backend is used.

typedef struct bcp_mem_interface
{
    void*  (*malloc_fn)(size_t size, void* user);
    void*  (*realloc_fn)(void* old_ptr, size_t old_size, size_t new_size, void* user);
    void   (*free_fn)(void* ptr, void* user);
    void*   user;
} bcp_mem_interface;


typedef struct bcp_context bcp_context;

typedef struct bcp_stats
{
    uint32_t top_table_size;
    uint8_t red_k[2], green_k[2], blue_k[2]; // one per endpoint: [0] = color[0], [1] = color[1]
    uint8_t mask_k, reference_k, difference_k;

    // size in bytes of each range-coded component, valid after a successful bcp_compress_bc1
    uint32_t colors_bytes;      // 6 color delta symbols per block
    uint32_t reference_bytes;   // top-table reference symbol per block
    uint32_t mask_bytes;        // index-residual mask symbol per block
    uint32_t difference_bytes;  // 0-4 index-residual bytes per block
} bcp_stats;


//----------------------------------------------------------------------------------------------------------------------------
// Creates a context. Returns NULL if the allocation fails.
bcp_context* bcp_init(bcp_mem_interface* mem);

//----------------------------------------------------------------------------------------------------------------------------
// Frees the context and all its working buffers. NULL is accepted.
void bcp_terminate(bcp_context* ctx);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given texture, in bytes.
// The [output] buffer of bcp_compress_bc1 must be at least this large.
size_t packed_bc1_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC1 texture (width x height pixels, must be a multiple of 4) into [output].
// [num_strips] receives the number of strips used for the compressed stream.
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bcp_compress_bc1(bcp_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

//----------------------------------------------------------------------------------------------------------------------------
// Extracts statistics after compression
// The *_bytes fields are valid after a successful bcp_compress_bc1, their sum is the range-coded payload size,
// the stream additionally contains the static header (models, top-table, strip offsets)
void bcp_get_stats(bcp_context* ctx, bcp_stats* stats);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
//
// Decompress one strip of the image, for a complete image this function should be called [num_strips] times
//      [input]         compressed data (output of bcp_compress_bc1)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC1 image
//      [num_strips]    number of strips, as returned by bcp_compress_bc1
//      [output]        must be big enough to contain a BC1 image of width x height
//      [strip_index]   index of the strip, must be in [0; num_strips[
// Returns false on error otherwise true
bool bcp_decompress_bc1(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);

//----------------------------------------------------------------------------------------------------------------------------
// Maximum size of the compressed stream for a given BC4 / BC5 texture, in bytes:
// 2x the standard block size, 16 bytes per 4x4 block for BC4 and 32 for BC5.
// The [output] buffer of bcp_compress_bc4 / bcp_compress_bc5 must be at least this large.
size_t packed_bc4_maxsize(uint32_t width, uint32_t height);
size_t packed_bc5_maxsize(uint32_t width, uint32_t height);

//----------------------------------------------------------------------------------------------------------------------------
// Compresses a BC4 / BC5 texture (width x height pixels, must be a multiple of 4) into [output].
// [bc4_image] holds (width/4) * (height/4) standard BC4 blocks of 8 bytes each: two 8-bit
// endpoints followed by the little-endian 48-bit field of the 16 3-bit indices (raster order).
// [bc5_image] holds the standard 16-byte BC5 blocks: the red BC4 block followed by the green one.
// [num_strips] receives the number of strips used for the compressed stream.
// Returns the size of the compressed stream in bytes, or 0 on failure.
size_t bcp_compress_bc4(bcp_context* ctx, const void* bc4_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);
size_t bcp_compress_bc5(bcp_context* ctx, const void* bc5_image, uint32_t width, uint32_t height, uint8_t* output, size_t output_length, uint32_t* num_strips);

//----------------------------------------------------------------------------------------------------------------------------
// Extracts statistics after a successful bcp_compress_bc5: the *_bytes fields are the size in
// bytes of each range-coded payload component, the stream additionally carries the static header.
// pair_bytes is the whole index payload; dict_bytes is reserved for a future index dictionary
// and is currently always 0.
typedef struct bcp_stats_bc5
{
    uint32_t endpoint_bytes; // 4 endpoint delta symbols per block (2 endpoints x 2 channels)
    uint32_t pair_bytes;     // index payload: 3 raw bits + 15 contextual symbols per channel block
    uint32_t dict_bytes;     // reserved, always 0 for now
} bcp_stats_bc5;

void bcp_get_stats_bc5(bcp_context* ctx, bcp_stats_bc5* stats);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
//
// Decompress one strip of the image, for a complete image this function should be called [num_strips] times
//      [input]         compressed data (output of bcp_compress_bc4 / bcp_compress_bc5)
//      [input_length]  size in byte of the compressed data
//      [width, height] dimensions of the output BC4 / BC5 image
//      [num_strips]    number of strips, as returned by bcp_compress_bc4 / bcp_compress_bc5
//      [output]        must be big enough to contain a BC4 / BC5 image of width x height
//      [strip_index]   index of the strip, must be in [0; num_strips[
// Returns false on error otherwise true
bool bcp_decompress_bc4(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);
bool bcp_decompress_bc5(bcp_context* ctx, const void* input, size_t input_length, uint32_t width, uint32_t height, uint32_t num_strips, void* output, uint32_t strip_index);

#ifdef __cplusplus
}
#endif

#endif

