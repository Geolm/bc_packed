#ifndef BC1_PACKED
#define BC1_PACKED


#include <stdint.h>
#include <stddef.h>


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
size_t bc1_packed_compress(bc1_packed_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, void* output);

//----------------------------------------------------------------------------------------------------------------------------
// CPU decompression, for unit tests and validation.
// [output] must be at least width*height/2 bytes (width/height are read from the stream).
// Returns the number of bytes written (= width*height/2), or 0 on failure (the output is zeroed).
size_t bc1_packed_uncompress(bc1_packed_context* ctx, const void* input, size_t input_length, void* output, size_t output_length);

#endif