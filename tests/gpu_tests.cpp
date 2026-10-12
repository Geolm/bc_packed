// GPU roundtrip + throughput tests: load a real image, convert it to the standard BC? texture
// with stb_dxt, compress it on the CPU with bcp_compress_bc?, then decompress it on the GPU
// with the bcp_decompress_bc? compute kernels of src/bc_decompress.metal and verify the decoded
// image is bit-identical to the original texture, while measuring the GPU throughput over
// GPU_DECODE_RUNS tiled runs (each run reads a distinct stream region, exercising DRAM
// bandwidth instead of staying in L2).
//
// Every Metal object is checked to be created without error (the Metal compiler reports
// failures via the NS::Error out-argument; its warnings go to the os_log stream and are not
// retrievable through the runtime API), and the final byte comparison catches any miscompile.

#include "greatest.h"
#include "test_helpers.h"

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "stb_image.h"
#include "stb_dxt.h"

#define SHADER_FILE ("src/bc_decompress.metal")
#define BC1_IMAGE_FILE ("images/brick1.png")
#define BC4_IMAGE_FILE ("images/bc4/red_brick_ao_1k.png")
#define BC5_IMAGE_FILE ("images/bc5/red_brick_nor_dx_1k.png")
#define GPU_DECODE_RUNS (1000)


//-----------------------------------------------------------------------------------------------------------------------------
// mirror of decode_args_t from src/bc_decompress.metal: the 8-byte aligned
// input_length pads the struct to 16 bytes, so the layout matches on both sides of the API
typedef struct decode_args
{
    uint32_t width;
    uint32_t height;
    uint64_t input_length;
} decode_args;


// the Metal objects of a roundtrip: the device, the library compiled from [SHADER_FILE],
// the compute pipeline of one kernel, and the command queue
typedef struct metal_state
{
    MTL::Device* device;
    MTL::Library* library;
    MTL::CommandQueue* queue;
    MTL::ComputePipelineState* pipeline;
    NS::String* source_string;
    MTL::CompileOptions* options;
} metal_state;


// bcp_compress_bc1 / bcp_compress_bc4 / bcp_compress_bc5 all share this signature
typedef size_t (*bcp_compress_fn)(bcp_context*, const void*, uint32_t, uint32_t, uint8_t*, size_t, uint32_t*);

// packed_bc1_maxsize / packed_bc4_maxsize / packed_bc5_maxsize all share this signature
typedef size_t (*packed_maxsize_fn)(uint32_t, uint32_t);

// builds the standard texture of one format from an RGBA image, one 4x4 block at a time
typedef void (*build_texture_fn)(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* bc_texture);


//-----------------------------------------------------------------------------------------------------------------------------
// reads [path] into a malloc'd, NUL-terminated buffer, returns nullptr on failure
static char* read_file(const char* path, size_t* out_length)
{
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
        return nullptr;

    size_t size = 0;
    if (fseek(file, 0, SEEK_END) != 0 || ftell(file) < 0)
    {
        fclose(file);
        return nullptr;
    }
    size = (size_t)ftell(file);
    fseek(file, 0, SEEK_SET);

    char* data = (char*)malloc(size + 1);
    if (data == nullptr)
    {
        fclose(file);
        return nullptr;
    }

    size_t read = fread(data, 1, size, file);
    fclose(file);

    if (read != size)
    {
        free(data);
        return nullptr;
    }

    data[size] = '\0';
    if (out_length != nullptr)
        *out_length = size;

    return data;
}

//-----------------------------------------------------------------------------------------------------------------------------
// logs a Metal creation error to stderr for diagnostics
static void print_metal_error(const char* what, NS::Error* error)
{
    if (error == nullptr)
    {
        fprintf(stderr, "==> %s failed, no error reported\n", what);
        return;
    }

    NS::String* message = error->localizedDescription();
    fprintf(stderr, "==> %s failed: %s\n", what, (message != nullptr) ? message->utf8String() : "(no message)");
}

//-----------------------------------------------------------------------------------------------------------------------------
// releases every non-null object of [state] and resets it to null (nullable-safe on failure paths)
static void metal_state_release(metal_state* state)
{
    if (state->queue != nullptr)
        state->queue->release();
    if (state->pipeline != nullptr)
        state->pipeline->release();
    if (state->library != nullptr)
        state->library->release();
    if (state->source_string != nullptr)
        state->source_string->release();
    if (state->options != nullptr)
        state->options->release();
    if (state->device != nullptr)
        state->device->release();

    state->queue = nullptr;
    state->pipeline = nullptr;
    state->library = nullptr;
    state->source_string = nullptr;
    state->options = nullptr;
    state->device = nullptr;
}

//-----------------------------------------------------------------------------------------------------------------------------
// creates the device, compiles [SHADER_FILE], verifies that [kernel_name] exists, creates the
// compute pipeline of [kernel_name] and the command queue. [state] must be initialized to all
// null; on failure it prints the diagnostic and releases whatever was created, returning false
static bool metal_state_init(metal_state* state, const char* kernel_name)
{
    state->device = MTL::CreateSystemDefaultDevice();
    if (state->device == nullptr)
    {
        fprintf(stderr, "==> no default Metal device available\n");
        return false;
    }

    size_t source_length = 0;
    char* source = read_file(SHADER_FILE, &source_length);
    if (source == nullptr)
    {
        fprintf(stderr, "==> unable to read %s, run the test from the project root\n", SHADER_FILE);
        metal_state_release(state);
        return false;
    }

    state->options = MTL::CompileOptions::alloc()->init();
    state->source_string = NS::String::alloc()->init(source, (NS::UInteger)source_length, NS::UTF8StringEncoding, false);
    free(source);
    if (state->options == nullptr || state->source_string == nullptr)
    {
        fprintf(stderr, "==> unable to allocate the shader source\n");
        metal_state_release(state);
        return false;
    }

    NS::Error* error = nullptr;
    state->library = state->device->newLibrary(state->source_string, state->options, &error);
    if (state->library == nullptr)
        print_metal_error("shader compilation", error);

    NS::String* function_name = NS::String::string(kernel_name, NS::UTF8StringEncoding);
    MTL::Function* function = (state->library != nullptr && function_name != nullptr) ? state->library->newFunction(function_name) : nullptr;
    if (function_name != nullptr)
        function_name->release();
    if (function == nullptr)
        fprintf(stderr, "==> function %s not found in %s\n", kernel_name, SHADER_FILE);
    if (function != nullptr)
        function->release();

    if (state->library != nullptr && function != nullptr)
    {
        error = nullptr;
        state->pipeline = state->device->newComputePipelineState(function, &error);
        if (state->pipeline == nullptr)
            print_metal_error("compute pipeline creation", error);
    }

    if (state->pipeline == nullptr)
    {
        metal_state_release(state);
        return false;
    }

    state->queue = state->device->newCommandQueue();
    if (state->queue == nullptr)
    {
        fprintf(stderr, "==> unable to create the command queue\n");
        metal_state_release(state);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// stb_dxt builders of the standard textures, one 4x4 block at a time

// BC1: the 16 RGBA bytes of the block, high quality
static void build_bc1_texture(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* bc1)
{
    for (uint32_t y = 0; y < height / 4; ++y)
    {
        for (uint32_t x = 0; x < width / 4; ++x)
        {
            uint8_t block[64];
            for (uint32_t j = 0; j < 4; ++j)
                memcpy(&block[j * 16], &rgba[(y * 4 + j) * width * 4 + x * 4], 16);

            stb_compress_dxt_block(&bc1[8 * (y * (width / 4) + x)], block, 0, STB_DXT_HIGHQUAL);
        }
    }
}

// BC4: the 16 red-channel bytes of the block
static void build_bc4_texture(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* bc4)
{
    for (uint32_t y = 0; y < height / 4; ++y)
    {
        for (uint32_t x = 0; x < width / 4; ++x)
        {
            uint8_t block[16];
            for (uint32_t j = 0; j < 4; ++j)
            {
                for (uint32_t k = 0; k < 4; ++k)
                    block[j * 4 + k] = rgba[((y * 4 + j) * width + (x * 4 + k)) * 4];
            }

            stb_compress_bc4_block(&bc4[8 * (y * (width / 4) + x)], block);
        }
    }
}

// BC5: the 32 interleaved red + green bytes of the block
static void build_bc5_texture(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* bc5)
{
    for (uint32_t y = 0; y < height / 4; ++y)
    {
        for (uint32_t x = 0; x < width / 4; ++x)
        {
            uint8_t block[32];
            for (uint32_t j = 0; j < 4; ++j)
            {
                for (uint32_t k = 0; k < 4; ++k)
                {
                    const uint8_t* px = &rgba[((y * 4 + j) * width + (x * 4 + k)) * 4];
                    uint8_t* dst = &block[(j * 4 + k) * 2];
                    dst[0] = px[0]; // red
                    dst[1] = px[1]; // green
                }
            }

            stb_compress_bc5_block(&bc5[16 * (y * (width / 4) + x)], block);
        }
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// Full GPU roundtrip + throughput measurement of one image with one format: load the image,
// build the standard texture, compress it on the CPU, decode it on the GPU (64 threads, one per
// strip, over GPU_DECODE_RUNS tiled runs), verify the decoded image is byte-identical to the
// original texture, and print the measured GPU rate in MiB/s. Returns greatest's result.
static greatest_test_res metal_gpu_decompression(const char* image_file, const char* format_name,
                                                 const char* kernel_name, size_t block_bytes,
                                                 packed_maxsize_fn maxsize, bcp_compress_fn compress,
                                                 build_texture_fn build)
{
    // load the image and build the original standard texture
    uint32_t width = 0, height = 0, channels = 0;
    uint8_t* rgba = stbi_load(image_file, (int*)&width, (int*)&height, (int*)&channels, 4);
    if (rgba == nullptr)
    {
        fprintf(stderr, "==> unable to load %s, run the test from the project root\n", image_file);
        FAIL();
    }

    // constraints enforced by both the compressors and the compute kernels
    ASSERT(width >= 16 && height >= 256 && (width & 3) == 0 && (height & 3) == 0);

    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const size_t image_size = (size_t)width_blocks * height_blocks * block_bytes;

    uint8_t* original = (uint8_t*)malloc(image_size);
    ASSERT(original != nullptr);
    build(rgba, width, height, original);
    stbi_image_free(rgba);

    // compress the standard texture on the CPU
    bcp_context* ctx = bcp_init(nullptr);
    ASSERT(ctx != nullptr);

    const size_t compressed_size = maxsize(width, height);
    uint8_t* compressed = (uint8_t*)malloc(compressed_size);
    ASSERT(compressed != nullptr);

    uint32_t num_strips = 0;
    const size_t compressed_length = compress(ctx, original, width, height, compressed, compressed_size, &num_strips);
    ASSERT(compressed_length != 0);
    ASSERT(compressed_length <= compressed_size);

    fprintf(stdout, "==> %s %ux%u : %u KiB %s, %zu bytes compressed\n",
            image_file, width, height, (uint32_t)(image_size / 1024), format_name, compressed_length);

    // create the Metal objects: the device, the compiled shader, the pipeline, the queue
    metal_state state = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    ASSERT(metal_state_init(&state, kernel_name));

    // allocate the output buffer for all runs
    const size_t total_output_size = image_size * (size_t)GPU_DECODE_RUNS;
    MTL::Buffer* output_buffer = state.device->newBuffer((NS::UInteger)total_output_size, MTL::ResourceStorageModeShared);
    ASSERT(output_buffer != nullptr);
    ASSERT(output_buffer->length() == (NS::UInteger)total_output_size);

    // upload the compressed stream tiled GPU_DECODE_RUNS times so each run reads a distinct region
    const size_t total_input_size = compressed_length * (size_t)GPU_DECODE_RUNS;
    MTL::Buffer* stream_buffer = state.device->newBuffer((NS::UInteger)total_input_size, MTL::ResourceStorageModeShared);
    ASSERT(stream_buffer != nullptr);
    ASSERT(stream_buffer->length() == (NS::UInteger)total_input_size);
    uint8_t* stream_contents = (uint8_t*)stream_buffer->contents();
    for (int run = 0; run < GPU_DECODE_RUNS; ++run)
        memcpy(stream_contents + run * compressed_length, compressed, compressed_length);
    stream_buffer->didModifyRange(NS::Range(0, (NS::UInteger)total_input_size));

    // dispatch: 64 threads (one per strip) x GPU_DECODE_RUNS runs in the Y dimension.
    // each Y row offsets both input and output by run_id, so every run reads from and
    // writes to a distinct memory region, exercising DRAM bandwidth
    MTL::CommandBuffer* command_buffer = state.queue->commandBuffer();
    ASSERT(command_buffer != nullptr);

    MTL::ComputeCommandEncoder* encoder = command_buffer->computeCommandEncoder();
    ASSERT(encoder != nullptr);

    decode_args args = { width, height, (uint64_t)compressed_length };
    encoder->setComputePipelineState(state.pipeline);
    encoder->setBytes(&args, sizeof(args), 2);
    encoder->setBuffer(stream_buffer, 0, 0);
    encoder->setBuffer(output_buffer, 0, 1);
    encoder->dispatchThreads(MTL::Size(num_strips, GPU_DECODE_RUNS, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();

    command_buffer->commit();
    command_buffer->waitUntilCompleted();
    ASSERT(command_buffer->status() == MTL::CommandBufferStatusCompleted);
    ASSERT(command_buffer->error() == nullptr);

    const double gpu_ms = (command_buffer->GPUEndTime() - command_buffer->GPUStartTime()) * 1000.0;

    // read the first run's output (at offset 0) and compare it byte by byte
    // against the original texture; all runs produce identical data
    uint8_t* decoded = (uint8_t*)malloc(image_size);
    ASSERT(decoded != nullptr);
    memcpy(decoded, output_buffer->contents(), image_size);

    for (size_t i = 0; i < image_size; ++i)
        ASSERT_EQ(decoded[i], original[i]);

    const double total_mib = (double)(image_size * (size_t)GPU_DECODE_RUNS) / (1024.0 * 1024.0);
    const double gpu_mib_per_s = (gpu_ms > 0.0) ? total_mib / (gpu_ms / 1000.0) : 0.0;

    fprintf(stdout, "==> GPU : %d decompress runs in %.3f ms, %.1f MiB/s\n", GPU_DECODE_RUNS, gpu_ms, gpu_mib_per_s);

    // release everything
    command_buffer->release();
    output_buffer->release();
    stream_buffer->release();
    metal_state_release(&state);

    free(decoded);
    free(compressed);
    bcp_terminate(ctx);
    free(original);

    PASS();
}


//-----------------------------------------------------------------------------------------------------------------------------
// the three GPU roundtrips: the brick content of images/brick1 (BC1) and the brick AO / normal
// maps of images/bc4 / images/bc5 (BC4 / BC5), each measured over GPU_DECODE_RUNS runs

TEST metal_gpu_decompression_bc1(void)
{
    return metal_gpu_decompression(BC1_IMAGE_FILE, "BC1", "bcp_decompress_bc1", 8,
                                   packed_bc1_maxsize, bcp_compress_bc1, build_bc1_texture);
}

TEST metal_gpu_decompression_bc4(void)
{
    return metal_gpu_decompression(BC4_IMAGE_FILE, "BC4", "bcp_decompress_bc4", 8,
                                   packed_bc4_maxsize, bcp_compress_bc4, build_bc4_texture);
}

TEST metal_gpu_decompression_bc5(void)
{
    return metal_gpu_decompression(BC5_IMAGE_FILE, "BC5", "bcp_decompress_bc5", 16,
                                   packed_bc5_maxsize, bcp_compress_bc5, build_bc5_texture);
}

extern "C" {

SUITE(suite_gpu)
{
    RUN_TEST(metal_gpu_decompression_bc1);
    RUN_TEST(metal_gpu_decompression_bc4);
    RUN_TEST(metal_gpu_decompression_bc5);
}

}
