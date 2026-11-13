// standalone inspector: compresses an image with bcp_compress_bc4 / bcp_compress_bc5 and prints
// the per-channel header flags (flat-index mode, endpoint MTF-8 enable) of the resulting stream.
// the stream is also decoded to make sure it is a valid stream of the current format
#include "bc_packed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

static size_t read_model(const uint8_t* stream, size_t length, size_t pos)
{
    if (pos + 3 > length)
        return 0;

    uint32_t num_symbols = stream[pos] + 1;
    pos += 3;

    if (pos + num_symbols > length)
        return 0;

    return pos + num_symbols;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <image-file> <width> <height> [bc4|bc5]\n", argv[0]);
        return 2;
    }

    const char* path = argv[1];
    uint32_t width = (uint32_t)atoi(argv[2]);
    uint32_t height = (uint32_t)atoi(argv[3]);
    uint32_t num_channels = (argc >= 5 && strcmp(argv[4], "bc5") == 0) ? 2 : 1;

    int w = 0, h = 0, channels = 0;
    uint8_t* rgba = stbi_load(path, &w, &h, &channels, 4);
    if (!rgba || (uint32_t)w != width || (uint32_t)h != height)
    {
        fprintf(stderr, "unable to load %s as %ux%u\n", path, width, height);
        return 1;
    }

    const uint32_t width_blocks = width / 4;
    const uint32_t height_blocks = height / 4;
    const uint32_t num_blocks = width_blocks * height_blocks;
    const size_t block_bytes = num_channels == 1 ? 8 : 16;

    uint8_t* bc = malloc(num_blocks * block_bytes);
    if (!bc)
        return 1;

    for (uint32_t by = 0; by < height_blocks; ++by)
    {
        for (uint32_t bx = 0; bx < width_blocks; ++bx)
        {
            uint8_t block[32];
            for (uint32_t j = 0; j < 4; ++j)
            {
                for (uint32_t k = 0; k < 4; ++k)
                {
                    const uint8_t* px = &rgba[((by * 4 + j) * width + (bx * 4 + k)) * 4];
                    uint8_t* dst = &block[(j * 4 + k) * num_channels];
                    dst[0] = px[0];
                    if (num_channels == 2)
                        dst[1] = px[1];
                }
            }

            if (num_channels == 2)
                stb_compress_bc5_block(&bc[(by * width_blocks + bx) * 16], block);
            else
                stb_compress_bc4_block(&bc[(by * width_blocks + bx) * 8], block);
        }
    }

    bcp_context* ctx = bcp_init(NULL);
    if (!ctx)
        return 1;

    const size_t maxsize = num_channels == 1 ? packed_bc4_maxsize(width, height) : packed_bc5_maxsize(width, height);
    uint8_t* compressed = malloc(maxsize);
    uint8_t* decoded = malloc(num_blocks * block_bytes);
    if (!compressed || !decoded)
        return 1;

    uint32_t num_strips = 0;
    size_t compressed_length = num_channels == 1 ? bcp_compress_bc4(ctx, bc, width, height, compressed, maxsize, &num_strips)
                                                 : bcp_compress_bc5(ctx, bc, width, height, compressed, maxsize, &num_strips);
    if (compressed_length == 0)
    {
        fprintf(stderr, "compression failed\n");
        return 1;
    }

    for (uint32_t s = 0; s < num_strips; ++s)
    {
        bool ok = num_channels == 1 ? bcp_decompress_bc4(ctx, compressed, compressed_length, width, height, num_strips, decoded, s)
                                    : bcp_decompress_bc5(ctx, compressed, compressed_length, width, height, num_strips, decoded, s);
        if (!ok)
        {
            fprintf(stderr, "strip %u failed to decode\n", s);
            return 1;
        }
    }

    size_t pos = 0;
    for (uint32_t c = 0; c < num_channels; ++c)
    {
        for (uint32_t i = 0; i < 68; ++i) // 2 endpoint + MTF + first-index + 64 index models
        {
            pos = read_model(compressed, compressed_length, pos);
            if (pos == 0)
            {
                fprintf(stderr, "model %u of channel %u does not parse\n", i, c);
                return 1;
            }
        }
    }

    pos += 2 * num_channels; // tile seeds
    if (pos + num_channels > compressed_length)
    {
        fprintf(stderr, "header truncated\n");
        return 1;
    }

    printf("%s: stream %zu bytes", path, compressed_length);
    for (uint32_t c = 0; c < num_channels; ++c)
    {
        const uint8_t b = compressed[pos + c];
        printf(" | channel %u: flat mode %u (value %u), MTF-8 %s", c, b & 1, (b >> 1) & 7, (b >> 4) & 1 ? "on" : "off");
    }
    printf("\n");

    free(decoded);
    free(compressed);
    free(bc);
    bcp_terminate(ctx);
    stbi_image_free(rgba);
    return 0;
}
