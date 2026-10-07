// GPU roundtrip test: load a real image, convert it to BC1 with stb_dxt, compress it on the
// CPU with bc1_packed_compress, then decompress it on the GPU with the bc1_packed_decompress
// compute kernel and verify the decoded image is bit-identical to the original BC1 texture.
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

#define SHADER_FILE ("src/bc1_packed_decompress.metal")
#define SHADER_FUNCTION ("bc1_packed_decompress")
#define IMAGE_FILE ("images/brick1.png")


//-----------------------------------------------------------------------------------------------------------------------------
// mirror of bc1_decode_args_t from src/bc1_packed_decompress.metal: the 8-byte aligned
// input_length pads the struct to 16 bytes, so the layout matches on both sides of the API
typedef struct bc1_decode_args
{
    uint32_t width;
    uint32_t height;
    uint64_t input_length;
} bc1_decode_args;


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
TEST metal_gpu_decompression(void)
{
    //-------------------------------------------------------------------------------------------------------------------------
    // load the image and build the original BC1 texture with stb_dxt
    uint32_t width = 0, height = 0, channels = 0;
    uint8_t* rgba = stbi_load(IMAGE_FILE, (int*)&width, (int*)&height, (int*)&channels, 4);
    if (rgba == nullptr)
    {
        fprintf(stderr, "==> unable to load %s, run the test from the project root\n", IMAGE_FILE);
        FAIL();
    }

    // constraints enforced by both bc1_packed_compress and the compute kernel
    ASSERT(width >= 16 && height >= 256 && (width & 3) == 0 && (height & 3) == 0);

    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const size_t num_blocks = (size_t)width_blocks * height_blocks;
    const size_t image_size = num_blocks * sizeof(bc1_block);

    uint8_t* original_bc1 = (uint8_t*)malloc(image_size);
    ASSERT(original_bc1 != nullptr);

    for (uint32_t y = 0; y < height_blocks; ++y)
    {
        for (uint32_t x = 0; x < width_blocks; ++x)
        {
            uint8_t block[64];
            for (uint32_t j = 0; j < 4; ++j)
                memcpy(&block[j * 16], &rgba[(y * 4 + j) * width * 4 + x * 4], 16);

            stb_compress_dxt_block(&original_bc1[8 * (y * width_blocks + x)], block, 0, STB_DXT_HIGHQUAL);
        }
    }

    stbi_image_free(rgba);

    //-------------------------------------------------------------------------------------------------------------------------
    // compress the BC1 texture on the CPU
    bc1_packed_context* ctx = bc1_packed_init(nullptr);
    ASSERT(ctx != nullptr);

    size_t compressed_size = bc1_packed_maxsize(width, height);
    uint8_t* compressed = (uint8_t*)malloc(compressed_size);
    ASSERT(compressed != nullptr);

    size_t compressed_length = bc1_packed_compress(ctx, original_bc1, width, height, compressed, compressed_size);
    ASSERT(compressed_length != 0);

    fprintf(stdout, "==> %s %ux%u : %u KiB BC1, %zu bytes compressed\n",
            IMAGE_FILE, width, height, (uint32_t)(image_size / 1024), compressed_length);

    //-------------------------------------------------------------------------------------------------------------------------
    // create the Metal device, compile the shader from source, build the pipeline state
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    if (device == nullptr)
    {
        fprintf(stderr, "==> no default Metal device available\n");
        FAIL();
    }

    size_t source_length = 0;
    char* source = read_file(SHADER_FILE, &source_length);
    if (source == nullptr)
    {
        fprintf(stderr, "==> unable to read %s, run the test from the project root\n", SHADER_FILE);
        FAIL();
    }

    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    ASSERT(options != nullptr);

    NS::String* source_string = NS::String::alloc()->init(source, (NS::UInteger)source_length, NS::UTF8StringEncoding, false);
    ASSERT(source_string != nullptr);

    NS::Error* error = nullptr;
    MTL::Library* library = device->newLibrary(source_string, options, &error);
    if (library == nullptr)
        print_metal_error("shader compilation", error);
    ASSERT(library != nullptr);

    MTL::Function* function = library->newFunction(MTLSTR("bc1_packed_decompress"));
    if (function == nullptr)
        fprintf(stderr, "==> function %s not found in %s\n", SHADER_FUNCTION, SHADER_FILE);
    ASSERT(function != nullptr);

    error = nullptr;
    MTL::ComputePipelineState* pipeline = device->newComputePipelineState(function, &error);
    if (pipeline == nullptr)
        print_metal_error("compute pipeline creation", error);
    ASSERT(pipeline != nullptr);

    MTL::CommandQueue* queue = device->newCommandQueue();
    ASSERT(queue != nullptr);

    free(source);

    //-------------------------------------------------------------------------------------------------------------------------
    // upload the compressed stream, allocate the output buffer for the decoded BC1 blocks
    MTL::Buffer* stream_buffer = device->newBuffer((NS::UInteger)compressed_length, MTL::ResourceStorageModeShared);
    ASSERT(stream_buffer != nullptr);
    ASSERT(stream_buffer->length() == (NS::UInteger)compressed_length);
    memcpy(stream_buffer->contents(), compressed, compressed_length);
    stream_buffer->didModifyRange(NS::Range(0, (NS::UInteger)compressed_length));

    MTL::Buffer* output_buffer = device->newBuffer((NS::UInteger)image_size, MTL::ResourceStorageModeShared);
    ASSERT(output_buffer != nullptr);
    ASSERT(output_buffer->length() == (NS::UInteger)image_size);

    //-------------------------------------------------------------------------------------------------------------------------
    // dispatch one thread per strip
    MTL::CommandBuffer* command_buffer = queue->commandBuffer();
    ASSERT(command_buffer != nullptr);

    MTL::ComputeCommandEncoder* encoder = command_buffer->computeCommandEncoder();
    ASSERT(encoder != nullptr);

    bc1_decode_args args = { width, height, (uint64_t)compressed_length };
    encoder->setComputePipelineState(pipeline);
    encoder->setBytes(&args, sizeof(args), 2);
    encoder->setBuffer(stream_buffer, 0, 0);
    encoder->setBuffer(output_buffer, 0, 1);
    encoder->dispatchThreads(MTL::Size(BC1_PACKED_NUM_STRIPS, 1, 1), MTL::Size(1, 1, 1));
    encoder->endEncoding();

    command_buffer->commit();
    command_buffer->waitUntilCompleted();
    ASSERT(command_buffer->status() == MTL::CommandBufferStatusCompleted);
    ASSERT(command_buffer->error() == nullptr);

    //-------------------------------------------------------------------------------------------------------------------------
    // read the output back from the shared memory and compare it block by block
    // against the original BC1 texture
    uint8_t* gpu_bc1 = (uint8_t*)malloc(image_size);
    ASSERT(gpu_bc1 != nullptr);
    memcpy(gpu_bc1, output_buffer->contents(), image_size);

    const bc1_block* original_blocks = (const bc1_block*)original_bc1;
    const bc1_block* gpu_blocks = (const bc1_block*)gpu_bc1;

    for (size_t i = 0; i < num_blocks; ++i)
    {
        ASSERT_EQ(gpu_blocks[i].color[0], original_blocks[i].color[0]);
        ASSERT_EQ(gpu_blocks[i].color[1], original_blocks[i].color[1]);
        ASSERT_EQ(gpu_blocks[i].indices, original_blocks[i].indices);
    }

    //-------------------------------------------------------------------------------------------------------------------------
    // release everything
    command_buffer->release();
    output_buffer->release();
    stream_buffer->release();
    queue->release();
    pipeline->release();
    function->release();
    library->release();
    source_string->release();
    options->release();
    device->release();

    free(gpu_bc1);
    free(compressed);
    bc1_packed_terminate(ctx);
    free(original_bc1);

    PASS();
}

extern "C" {

SUITE(suite_gpu)
{
    RUN_TEST(metal_gpu_decompression);
}

}
