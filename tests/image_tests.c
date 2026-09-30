#include "test_helpers.h"
#include <dirent.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>

#include "stb_image.h"
#include "stb_dxt.h"

#define IMAGE_MAX_SIDE (4096)

//----------------------------------------------------------------------------------------------------------------------------
static inline void extract_4x4_rgba_block(const uint8_t* rgba, uint32_t width, uint32_t x, uint32_t y, uint8_t block[64])
{
    for (uint32_t j = 0; j < 4; j++)
    {
        const uint8_t* src = &rgba[(y + j) * width * 4 + x * 4];
        memcpy(&block[j * 4 * 4], src, 16);
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
FILE* open_hourly_stats_file(const char *folder_path)
{
    if (!folder_path) return NULL;

    time_t now = time(NULL);
    if (now == (time_t)(-1)) return NULL;

    struct tm *t = localtime(&now);
    if (t == NULL) return NULL;

    // 1. Format the filename
    char filename[64];
    if (strftime(filename, sizeof(filename), "stats_%Y-%m-%d_%H_%M.csv", t) == 0) return NULL;

    // 2. Build the full path
    char full_path[1024];
    int len = snprintf(full_path, sizeof(full_path), "%s/%s", folder_path, filename);
    
    if (len < 0 || (size_t)len >= sizeof(full_path)) return NULL;

    // 3. Open the file in append mode
    return fopen(full_path, "a");
}

//-----------------------------------------------------------------------------------------------------------------------------
TEST test_all_images_in_folder(const char *dir_path) 
{
    DIR *dir = opendir(dir_path);
    if (!dir) 
    {
        fprintf(stderr, "Unable to open directory %s\n", dir_path);
        FAIL();
    }

    FILE* stats = open_hourly_stats_file("./logs");
    if (!stats)
    {
        fprintf(stderr, "Unable to create stats file\n");
        FAIL();
    }

    // init and alloc buffers
    bc1_packed_context* ctx = bc1_packed_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = bc1_packed_maxsize(IMAGE_MAX_SIDE,  IMAGE_MAX_SIDE);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);
    uint8_t* original_bc1 = malloc((IMAGE_MAX_SIDE/4)*(IMAGE_MAX_SIDE/4)*sizeof(bc1_block));
    ASSERT(original_bc1 != NULL);

    fprintf(stats, "filename,width,height,compression_ratio\n");

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) 
    {
        // Skip current (".") and parent ("..") directory pointers
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
            continue;

        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
        
        uint32_t width, height, num_channels;
        uint8_t *rgba = stbi_load(file_path, (int*)&width, (int*)&height, (int*)&num_channels, 4);

        if (rgba == NULL)
            continue;

        uint32_t block_index=0;
        for(uint32_t y=0; y<height/4; ++y)
        {
            for(uint32_t x=0; x<width/4; ++x)
            {
                uint8_t block_image[64];
                extract_4x4_rgba_block(rgba, width, x*4, y*4, block_image);
                stb_compress_dxt_block(&original_bc1[8 * block_index], block_image, 0, STB_DXT_HIGHQUAL);
                block_index++;
            }
        }

        size_t compressed_buffer_length = bc1_packed_compress(ctx, original_bc1, width, height, compressed_buffer, compressed_buffer_size);
        ASSERT(compressed_buffer_length != 0);

        const float compression_ratio = 0.f;
        fprintf(stats, "%s,%" PRIu32 ",%" PRIu32 ",%.6f\n", file_path, width, height, compression_ratio);
        fflush(stats);


        stbi_image_free(rgba);
    }
    closedir(dir);
    free(original_bc1);
    fclose(stats);

    PASS();
}


SUITE(suite_image)
{
    RUN_TEST1(test_all_images_in_folder, "./images/");
}


