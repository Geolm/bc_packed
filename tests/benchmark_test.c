#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Single-header implementations, compiled only in this file.
// sched.h's SCHED_ALIGNOF uses null pointer arithmetic, ignored here to keep -Werror clean.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#define SCHED_STATIC
#define SCHED_IMPLEMENTATION
#include "sched.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#define SOKOL_TIME_IMPL
#include "sokol_time.h"

// Declarations only, implementations live in main_test.c
#include "stb_image.h"
#include "stb_dxt.h"

#define BENCH_IMAGE_PATH           ("./images/ground.png")
#define BENCH_IMAGE_WIDTH          (1024)
#define BENCH_IMAGE_HEIGHT         (1024)
#define BENCH_DECOMPRESS_RUNS      (1000)

// matches sizeof(hashmap_entry) * HASHMAP_SIZE of bc1_packed.c
#define BENCH_DUMMY_HASHMAP_SIZE   ((1U << 20U) * 8U)


//----------------------------------------------------------------------------------------------------------------------------
// The context hashmap is only used by the compressor. To avoid allocating a useless 8 MiB block
// per worker context, decompression-only contexts get their hashmap mapped to one shared no-op
// buffer through the memory interface, and the 8 MiB buffer is freed directly by the test.
//----------------------------------------------------------------------------------------------------------------------------
static void* bench_malloc_fn(size_t size, void* user)
{
    if (size == BENCH_DUMMY_HASHMAP_SIZE)
        return user;
    return malloc(size);
}

static void* bench_realloc_fn(void* old_ptr, size_t old_size, size_t new_size, void* user)
{
    (void)old_size;
    if (new_size == BENCH_DUMMY_HASHMAP_SIZE)
        return user;
    return realloc(old_ptr, new_size);
}

static void bench_free_fn(void* ptr, void* user)
{
    if (ptr == user)
        return;
    free(ptr);
}

static bc1_packed_mem_interface bench_decompress_mem(void* shared_hashmap)
{
    return (bc1_packed_mem_interface)
    {
        .malloc_fn  = bench_malloc_fn,
        .realloc_fn = bench_realloc_fn,
        .free_fn    = bench_free_fn,
        .user       = shared_hashmap
    };
}


//----------------------------------------------------------------------------------------------------------------------------
// Decompresses the strips of [partition] into the shared output buffer.
// One context per worker thread: bc1_packed_decompress rewrites ctx state on every call,
// so a single context must never be used from multiple threads. Strips write disjoint
// rows of the output buffer, the compressed stream is read-only.
//----------------------------------------------------------------------------------------------------------------------------
typedef struct bench_args
{
    bc1_packed_context** ctxs;
    uint8_t* failures;
    const uint8_t* compressed;
    size_t compressed_size;
    uint8_t* output;
    uint32_t width, height;
} bench_args;

static void bench_decompress_task(void* pArg, struct scheduler* s, struct sched_task_partition partition, sched_uint thread_num)
{
    (void)s;
    bench_args* args = (bench_args*)pArg;
    for (uint32_t strip = partition.start; strip < partition.end; ++strip)
    {
        if (!bc1_packed_decompress(args->ctxs[thread_num], args->compressed, args->compressed_size, args->width, args->height, args->output, strip))
            args->failures[thread_num] = 1;
    }
}


//----------------------------------------------------------------------------------------------------------------------------
static inline void extract_4x4_rgba_block(const uint8_t* rgba, uint32_t width, uint32_t x, uint32_t y, uint8_t block[64])
{
    for (uint32_t j = 0; j < 4; j++)
    {
        const uint8_t* src = &rgba[(y + j) * width * 4 + x * 4];
        memcpy(&block[j * 4 * 4], src, 16);
    }
}

//----------------------------------------------------------------------------------------------------------------------------
// compresses ground.png once, then decompresses it 100 times with one sched task over the 64 strips,
// measured with sokol_time, and prints the decompression rate in MB/s (MiB/s)
//----------------------------------------------------------------------------------------------------------------------------
TEST benchmark_ground_decompression(void)
{
    int width = 0, height = 0, channels = 0;
    uint8_t* rgba = stbi_load(BENCH_IMAGE_PATH, &width, &height, &channels, 4);
    ASSERT(rgba != NULL);
    ASSERT_EQ(width, BENCH_IMAGE_WIDTH);
    ASSERT_EQ(height, BENCH_IMAGE_HEIGHT);

    const uint32_t bench_width = (uint32_t)width;
    const uint32_t bench_height = (uint32_t)height;
    const size_t image_size = (bench_width/4) * (bench_height/4) * sizeof(bc1_block);

    // load and convert to bc1
    uint8_t* original_bc1 = malloc(image_size);
    ASSERT(original_bc1 != NULL);
    uint32_t block_index = 0;
    for (uint32_t y = 0; y < bench_height/4; ++y)
    {
        for (uint32_t x = 0; x < bench_width/4; ++x)
        {
            uint8_t block[64];
            extract_4x4_rgba_block(rgba, bench_width, x*4, y*4, block);
            stb_compress_dxt_block(&original_bc1[8 * block_index], block, 0, STB_DXT_HIGHQUAL);
            block_index++;
        }
    }

    // compress once, not measured
    bc1_packed_context* compress_ctx = bc1_packed_init(NULL);
    ASSERT(compress_ctx != NULL);
    const size_t max_stream_size = bc1_packed_maxsize(bench_width, bench_height);
    uint8_t* compressed = malloc(max_stream_size);
    ASSERT(compressed != NULL);
    const size_t compressed_size = bc1_packed_compress(compress_ctx, original_bc1, bench_width, bench_height, compressed, max_stream_size);
    ASSERT(compressed_size != 0);

    // scheduler + one decompression-only context per worker thread
    struct scheduler sched;
    sched_size needed_memory = 0;
    scheduler_init(&sched, &needed_memory, SCHED_DEFAULT, NULL);
    uint8_t* sched_memory = calloc(needed_memory, 1);
    ASSERT(sched_memory != NULL);
    scheduler_start(&sched, sched_memory);

    const uint32_t num_threads = sched.threads_num;
    uint8_t* shared_hashmap = malloc(BENCH_DUMMY_HASHMAP_SIZE);
    ASSERT(shared_hashmap != NULL);
    bc1_packed_mem_interface decompress_mem = bench_decompress_mem(shared_hashmap);
    bc1_packed_context** worker_ctxs = malloc(num_threads * sizeof(*worker_ctxs));
    ASSERT(worker_ctxs != NULL);
    for (uint32_t i = 0; i < num_threads; ++i)
    {
        worker_ctxs[i] = bc1_packed_init(&decompress_mem);
        ASSERT(worker_ctxs[i] != NULL);
    }

    uint8_t* decompressed = malloc(image_size);
    ASSERT(decompressed != NULL);
    uint8_t* failures = calloc(num_threads, 1);
    ASSERT(failures != NULL);

    bench_args args = {0};
    args.ctxs = worker_ctxs;
    args.failures = failures;
    args.compressed = compressed;
    args.compressed_size = compressed_size;
    args.output = decompressed;
    args.width = bench_width;
    args.height = bench_height;

    // unmeasured roundtrip, validates the parallel decompression
    struct sched_task task;
    scheduler_add(&sched, &task, bench_decompress_task, &args, BC1_PACKED_NUM_STRIPS, 1);
    scheduler_join(&sched, &task);
    for (uint32_t i = 0; i < num_threads; ++i)
    {
        ASSERT_EQ(failures[i], 0);
    }
    for (size_t i = 0; i < image_size; ++i)
    {
        ASSERT_EQ(original_bc1[i], decompressed[i]);
    }

    // measured: 1000 full image decompressions
    stm_setup();
    const uint64_t start = stm_now();
    for (int run = 0; run < BENCH_DECOMPRESS_RUNS; ++run)
    {
        scheduler_add(&sched, &task, bench_decompress_task, &args, BC1_PACKED_NUM_STRIPS, 1);
        scheduler_join(&sched, &task);
    }
    const double seconds = stm_sec(stm_now() - start);

    const double megabytes = (double)(image_size * BENCH_DECOMPRESS_RUNS) / (1024.0 * 1024.0);
    const double mb_per_sec = seconds > 0.0 ? megabytes / seconds : 0.0;
    printf("\n==> benchmark %s %ux%u : %u threads, %u KiB image (%u bytes compressed), %d decompress runs in %.3f s, %.1f MB/s\n\n",
           BENCH_IMAGE_PATH, bench_width, bench_height, num_threads, (unsigned)(image_size/1024),
           (unsigned)compressed_size, BENCH_DECOMPRESS_RUNS, seconds, mb_per_sec);

    // teardown
    scheduler_stop(&sched, 1);
    free(sched_memory);
    for (uint32_t i = 0; i < num_threads; ++i)
    {
        bc1_packed_terminate(worker_ctxs[i]);
    }
    free(worker_ctxs);
    free(shared_hashmap);
    free(decompressed);
    free(failures);
    free(compressed);
    bc1_packed_terminate(compress_ctx);
    free(original_bc1);
    stbi_image_free(rgba);

    PASS();
}


SUITE(suite_benchmark)
{
    RUN_TEST(benchmark_ground_decompression);
}
