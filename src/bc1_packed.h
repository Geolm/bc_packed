#ifndef BC1_PACKED
#define BC1_PACKED


#include <stdint.h>
#include <stddef.h>


typedef struct bc1_packed_mem_interface
{
    void*  (*malloc_fn)(size_t size, void* user);
    void*  (*realloc_fn)(void* old_ptr, size_t old_size, size_t new_size, void* user);
    void   (*free_fn)(void* ptr, void* user);
    void*   user;
} bc1_packed_mem_interface;


typedef struct bc1_packed_context bc1_packed_context;


bc1_packed_context* bc1_packed_init(bc1_packed_mem_interface* mem);
size_t bc1_packed_maxsize(uint32_t width, uint32_t height);
size_t bc1_packed_compress(bc1_packed_context* ctx, const void* bc1_image, uint32_t width, uint32_t height, void* output);
void bc1_packed_terminate(bc1_packed_context* ctx);

#endif