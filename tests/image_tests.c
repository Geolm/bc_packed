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
    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    size_t compressed_buffer_size = packed_bc1_maxsize(IMAGE_MAX_SIDE,  IMAGE_MAX_SIDE);
    void* compressed_buffer = malloc(compressed_buffer_size);
    ASSERT(compressed_buffer != NULL);
    size_t max_image_size = (IMAGE_MAX_SIDE/4)*(IMAGE_MAX_SIDE/4)*sizeof(bc1_block);
    uint8_t* original_bc1 = malloc(max_image_size);
    uint8_t* decompressed_bc1 = malloc(max_image_size);
    ASSERT(original_bc1 != NULL && decompressed_bc1 != NULL);

    fprintf(stats, "filename,width,height,compression_ratio, top_table_size, red0_k, red1_k, green0_k, green1_k, blue0_k, blue1_k, reference_k, mask_k, difference_k, colors_bytes, reference_bytes, mask_bytes, difference_bytes\n");

    float global_ratio = 0.f;
    uint32_t num_images = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) 
    {
        // Skip current (".") and parent ("..") directory pointers
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) 
            continue;

        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
        
        // load and convert to bc1
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

        // compress the bc1 image
        uint32_t num_strips = 0;
        size_t compressed_buffer_length = bcp_compress_bc1(ctx, original_bc1, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
        ASSERT(compressed_buffer_length != 0);
        ASSERT(compressed_buffer_length <= compressed_buffer_size);

        const size_t image_size = (width/4)*(height/4)*sizeof(bc1_block);
        const float compression_ratio = (float) image_size / (float) compressed_buffer_length;
        fprintf(stats, "%s,%" PRIu32 ",%" PRIu32 ",%.6f,", file_path, width, height, compression_ratio);

        bcp_stats cmp_stats;
        bcp_get_stats(ctx, &cmp_stats);

        fprintf(stats, "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
            cmp_stats.top_table_size, cmp_stats.red_k[0], cmp_stats.red_k[1], cmp_stats.green_k[0], cmp_stats.green_k[1], cmp_stats.blue_k[0], cmp_stats.blue_k[1],
            cmp_stats.reference_k, cmp_stats.mask_k, cmp_stats.difference_k,
            cmp_stats.colors_bytes, cmp_stats.reference_bytes, cmp_stats.mask_bytes, cmp_stats.difference_bytes);

        fflush(stats);

        global_ratio += compression_ratio;
        num_images++;

        // roundtrip test
        for(uint32_t i=0; i<num_strips; ++i)
        {
            ASSERT(bcp_decompress_bc1(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed_bc1, i));
        }

        for(uint32_t i=0; i<image_size; ++i)
        {
            ASSERT_EQ(original_bc1[i], decompressed_bc1[i]);
        }

        stbi_image_free(rgba);
    }

    const float average_compression_ratio = global_ratio / (float) num_images;

    fprintf(stats, "\n\n# %u images, average compression ratio : %2.4f:1\n\n", num_images, average_compression_ratio);

    closedir(dir);
    free(original_bc1);
    free(decompressed_bc1);
    free(compressed_buffer);
    fclose(stats);


    fprintf(stdout, "\n==> %u images, average compression ratio : %2.4f:1\n\n", num_images, average_compression_ratio);

    ASSERT(average_compression_ratio>1.45f);

    PASS();
}


//-----------------------------------------------------------------------------------------------------------------------------
// BC4 / BC5 image roundtrips on the hardcoded sets of images/bc4 (AO maps) and images/bc5
// (normal maps, red + green channels). the filenames are hardcoded, the folder is not scanned,
// and the per-image ratios are printed to stdout (no CSV): the average must stay above 1:1
static const char* const bc4_image_filenames[] =
{
    "images/bc4/grey_roof_tiles_02_ao_1k.png",
    "images/bc4/patterned_cobblestone_ao_1k.png",
    "images/bc4/red_brick_ao_1k.png",
    "images/bc4/rough_wood_ao_1k.png"
};

static const char* const bc5_image_filenames[] =
{
    "images/bc5/grey_roof_tiles_02_nor_dx_1k.png",
    "images/bc5/patterned_cobblestone_nor_dx_1k.png",
    "images/bc5/red_brick_nor_dx_1k.png",
    "images/bc5/rough_wood_nor_dx_1k.png"
};

static greatest_test_res run_bc_image_suite(bool bc5)
{
    const char* const* filenames = bc5 ? bc5_image_filenames : bc4_image_filenames;
    const uint32_t num_filenames = sizeof(bc4_image_filenames) / sizeof(bc4_image_filenames[0]);

    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    float global_ratio = 0.f;
    uint32_t num_images = 0;

    for(uint32_t i=0; i<num_filenames; ++i)
    {
        // load the source image and convert to the standard bc4 / bc5 blocks with stb_dxt
        uint32_t width, height, num_channels;
        uint8_t* rgba = stbi_load(filenames[i], (int*)&width, (int*)&height, (int*)&num_channels, 4);
        ASSERT(rgba != NULL);

        uint32_t width_blocks = width / 4;
        uint32_t height_blocks = height / 4;
        uint32_t num_blocks = width_blocks * height_blocks;
        const size_t block_bytes = bc5 ? 16 : 8;

        uint8_t* bc_image = malloc(num_blocks * block_bytes);
        uint8_t* decompressed = malloc(num_blocks * block_bytes);
        ASSERT(bc_image != NULL && decompressed != NULL);

        for(uint32_t by=0; by<height_blocks; ++by)
        {
            for(uint32_t bx=0; bx<width_blocks; ++bx)
            {
                uint8_t block_image[32]; // bc4: 16 bytes of one channel, bc5: 32 interleaved red + green

                for(uint32_t j=0; j<4; ++j)
                {
                    for(uint32_t k=0; k<4; ++k)
                    {
                        const uint8_t* px = &rgba[((by * 4 + j) * width + (bx * 4 + k)) * 4];
                        uint8_t* dst = &block_image[(j * 4 + k) * (bc5 ? 2 : 1)];

                        dst[0] = px[0];
                        if (bc5)
                            dst[1] = px[1]; // red and green channels of the normal map
                    }
                }

                if (bc5)
                    stb_compress_bc5_block(&bc_image[(by * width_blocks + bx) * 16], block_image);
                else
                    stb_compress_bc4_block(&bc_image[(by * width_blocks + bx) * 8], block_image);
            }
        }

        // compress the bc4 / bc5 image and report the ratio against the standard block size
        size_t compressed_buffer_size = bc5 ? packed_bc5_maxsize(width, height) : packed_bc4_maxsize(width, height);
        uint8_t* compressed_buffer = malloc(compressed_buffer_size);
        ASSERT(compressed_buffer != NULL);

        uint32_t num_strips = 0;
        size_t compressed_buffer_length = bc5 ? bcp_compress_bc5(ctx, bc_image, width, height, compressed_buffer, compressed_buffer_size, &num_strips)
                                              : bcp_compress_bc4(ctx, bc_image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
        ASSERT(compressed_buffer_length != 0);
        ASSERT(compressed_buffer_length <= compressed_buffer_size);

        const float compression_ratio = (float)(num_blocks * block_bytes) / (float)compressed_buffer_length;
        fprintf(stdout, "%s: %u x %u, %s ratio %.4f:1 (stream %zu bytes)\n",
            filenames[i], width, height, bc5 ? "bc5" : "bc4", compression_ratio, compressed_buffer_length);

        global_ratio += compression_ratio;
        num_images++;

        // roundtrip test: decompress every strip, byte-exact against the standard blocks
        for(uint32_t s=0; s<num_strips; ++s)
        {
            bool ok = bc5 ? bcp_decompress_bc5(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, s)
                          : bcp_decompress_bc4(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, s);
            ASSERT(ok);
        }

        for(size_t j=0; j<num_blocks * block_bytes; ++j)
            ASSERT_EQ(decompressed[j], bc_image[j]);

        stbi_image_free(rgba);
        free(bc_image);
        free(decompressed);
        free(compressed_buffer);
    }

    const float average_compression_ratio = global_ratio / (float)num_images;

    fprintf(stdout, "==> %s: %u images, average compression ratio : %2.4f:1\n\n", bc5 ? "bc5" : "bc4", num_images, average_compression_ratio);

    bcp_terminate(ctx);

    ASSERT(average_compression_ratio >= 1.0f);

    PASS();
}

//-----------------------------------------------------------------------------------------------------------------------------
TEST bc4_images(void)
{
    // an assertion fails by returning, so propagate the result
    return run_bc_image_suite(false);
}

//-----------------------------------------------------------------------------------------------------------------------------
TEST bc5_images(void)
{
    // an assertion fails by returning, so propagate the result
    return run_bc_image_suite(true);
}


SUITE(suite_image)
{
    RUN_TEST1(test_all_images_in_folder, "./images/");
    RUN_TEST(bc4_images);
    RUN_TEST(bc5_images);
}


