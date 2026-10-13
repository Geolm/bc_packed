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
// All image suites log to the same hourly CSV, each opening it in append mode, so the
// header is only written for a new file: after fseek to SEEK_END, ftell returns the size
static void ensure_stats_header(FILE* stats)
{
    fseek(stats, 0, SEEK_END);
    if (ftell(stats) <= 0)
        fprintf(stats, "format,filename,width,height,compression_ratio\n");
}

//-----------------------------------------------------------------------------------------------------------------------------
// one row per image and a comment line summarizing the suite, keeping the CSV self
// documenting across the bc1, bc4, bc5 and bc3 suites
static void log_image_stats(FILE* stats, const char* format, const char* file_path, uint32_t width, uint32_t height, float ratio)
{
    fprintf(stats, "%s,%s,%" PRIu32 ",%" PRIu32 ",%.6f\n", format, file_path, width, height, ratio);
    fflush(stats);
}

static void log_stats_summary(FILE* stats, const char* format, uint32_t num_images, float average_ratio)
{
    fprintf(stats, "\n\n# %s: %" PRIu32 " images, average compression ratio : %.4f:1\n\n", format, num_images, average_ratio);
    fflush(stats);
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

    ensure_stats_header(stats);

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
        log_image_stats(stats, "bc1", file_path, width, height, compression_ratio);

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

    log_stats_summary(stats, "bc1", num_images, average_compression_ratio);

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
// the per-image ratios are logged to the hourly CSV and printed to stdout: the average must
// stay above 1:1
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
    const char* format = bc5 ? "bc5" : "bc4";
    const char* const* filenames = bc5 ? bc5_image_filenames : bc4_image_filenames;
    const uint32_t num_filenames = sizeof(bc4_image_filenames) / sizeof(bc4_image_filenames[0]);

    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    FILE* stats = open_hourly_stats_file("./logs");
    if (!stats)
    {
        fprintf(stderr, "Unable to create stats file\n");
        FAIL();
    }

    ensure_stats_header(stats);

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
        log_image_stats(stats, format, filenames[i], width, height, compression_ratio);
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

    log_stats_summary(stats, format, num_images, average_compression_ratio);
    fclose(stats);

    fprintf(stdout, "==> %s: %u images, average compression ratio : %2.4f:1\n\n", bc5 ? "bc5" : "bc4", num_images, average_compression_ratio);

    bcp_terminate(ctx);

    ASSERT(average_compression_ratio >= 1.05f);

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

//-----------------------------------------------------------------------------------------------------------------------------
// BC3 image roundtrips on the hardcoded set of images/bc3 (albedo + alpha maps): the filenames
// are hardcoded, the folder is not scanned, the per-image ratios are logged to the hourly CSV
// and printed to stdout, and the average must stay above 1:1
static const char* const bc3_image_filenames[] =
{
    "images/bc3/leak-stain_albedo.png",
    "images/bc3/leaves.png"
};

TEST bc3_images(void)
{
    const uint32_t num_filenames = sizeof(bc3_image_filenames) / sizeof(bc3_image_filenames[0]);

    bcp_context* ctx = bcp_init(NULL);
    ASSERT(ctx != NULL);

    FILE* stats = open_hourly_stats_file("./logs");
    if (!stats)
    {
        fprintf(stderr, "Unable to create stats file\n");
        FAIL();
    }

    ensure_stats_header(stats);

    float global_ratio = 0.f;
    uint32_t num_images = 0;

    for(uint32_t i=0; i<num_filenames; ++i)
    {
        // load the source image and convert to the standard bc3 blocks with stb_dxt
        uint32_t width, height, num_channels;
        uint8_t* rgba = stbi_load(bc3_image_filenames[i], (int*)&width, (int*)&height, (int*)&num_channels, 4);
        ASSERT(rgba != NULL);

        uint32_t width_blocks = width / 4;
        uint32_t height_blocks = height / 4;
        uint32_t num_blocks = width_blocks * height_blocks;

        uint8_t* bc_image = malloc(num_blocks * 16);
        uint8_t* decompressed = malloc(num_blocks * 16);
        ASSERT(bc_image != NULL && decompressed != NULL);

        for(uint32_t by=0; by<height_blocks; ++by)
        {
            for(uint32_t bx=0; bx<width_blocks; ++bx)
            {
                uint8_t block_rgba[64]; // 16 pixels x (r + g + b + a)
                uint8_t block_alpha[16];

                for(uint32_t j=0; j<4; ++j)
                {
                    for(uint32_t k=0; k<4; ++k)
                    {
                        const uint8_t* px = &rgba[((by * 4 + j) * width + (bx * 4 + k)) * 4];
                        uint8_t* dst = &block_rgba[(j * 4 + k) * 4];

                        dst[0] = px[0];
                        dst[1] = px[1];
                        dst[2] = px[2];
                        dst[3] = px[3];
                        block_alpha[j * 4 + k] = px[3];
                    }
                }

                // stb_compress_dxt_block with alpha = 1 writes the alpha block before the color one,
                // the opposite of the D3D bc3 order, so build the two halves separately
                stb_compress_dxt_block(&bc_image[(by * width_blocks + bx) * 16], block_rgba, 0, STB_DXT_HIGHQUAL); // color, bytes 0..7
                stb_compress_bc4_block(&bc_image[(by * width_blocks + bx) * 16 + 8], block_alpha);                // alpha, bytes 8..15
            }
        }

        // compress the bc3 image and report the ratio against the standard block size
        size_t compressed_buffer_size = packed_bc3_maxsize(width, height);
        uint8_t* compressed_buffer = malloc(compressed_buffer_size);
        ASSERT(compressed_buffer != NULL);

        uint32_t num_strips = 0;
        size_t compressed_buffer_length = bcp_compress_bc3(ctx, bc_image, width, height, compressed_buffer, compressed_buffer_size, &num_strips);
        ASSERT(compressed_buffer_length != 0);
        ASSERT(compressed_buffer_length <= compressed_buffer_size);

        const float compression_ratio = (float)(num_blocks * 16) / (float)compressed_buffer_length;
        log_image_stats(stats, "bc3", bc3_image_filenames[i], width, height, compression_ratio);
        fprintf(stdout, "%s: %u x %u, bc3 ratio %.4f:1 (stream %zu bytes)\n",
            bc3_image_filenames[i], width, height, compression_ratio, compressed_buffer_length);

        global_ratio += compression_ratio;
        num_images++;

        // roundtrip test: decompress every strip, byte-exact against the standard blocks
        for(uint32_t s=0; s<num_strips; ++s)
        {
            ASSERT(bcp_decompress_bc3(ctx, compressed_buffer, compressed_buffer_length, width, height, num_strips, decompressed, s));
        }

        for(size_t j=0; j<num_blocks * 16; ++j)
            ASSERT_EQ(decompressed[j], bc_image[j]);

        stbi_image_free(rgba);
        free(bc_image);
        free(decompressed);
        free(compressed_buffer);
    }

    const float average_compression_ratio = global_ratio / (float)num_images;

    log_stats_summary(stats, "bc3", num_images, average_compression_ratio);
    fclose(stats);

    fprintf(stdout, "==> bc3: %u images, average compression ratio : %2.4f:1\n\n", num_images, average_compression_ratio);

    bcp_terminate(ctx);

    ASSERT(average_compression_ratio >= 2.7f);

    PASS();
}


SUITE(suite_image)
{
    RUN_TEST1(test_all_images_in_folder, "./images/");
    RUN_TEST(bc4_images);
    RUN_TEST(bc5_images);
    RUN_TEST(bc3_images);
}


