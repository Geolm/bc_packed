//-----------------------------------------------------------------------------------------------------------------------------
// bc1_packed_decompress.metal
//
// GPU mirror of bc1_packed_decompress (src/bc1_packed.c): one thread decompresses one strip of a
// bc1_packed stream. The compressed stream is read from a device buffer, the decoded BC1 blocks
// (two 16-bit 565 endpoint colors followed by the 32-bit indices, 8 bytes per block) are written
// to an output device buffer laid out as a row-major grid of width/4 x height/4 blocks.
//
// Dispatch 64 threads, one per strip:
//
//     bc1_decode_args_t args = { width, height, (uint64_t)stream_length };
//     encoder.setComputePipelineState(pipeline);   // pipeline built from the "bc1_packed_decompress" kernel
//     encoder.setBytes(&args, sizeof(args), 2);
//     encoder.setBuffer(stream_buffer, NULL, 0, 0);
//     encoder.setBuffer(output_buffer, NULL, 1, 0);
//     encoder.dispatchThreads(threadgrid(64, 1, 1));
//
// buffer(0): device const uint8_t*  the compressed stream (output of bc1_packed_compress)
// buffer(1): device uint8_t*        output BC1 blocks, width/4 * height/4 * 8 bytes
// buffer(2): constant bc1_decode_args_t
//
// The stream layout parsed here mirrors bc1_packed_compress / bc1_packed_decompress:
//   [17 static models][top table size u8][4-align][strip seed u32][strip predictor u64][strip dict enable u64]
//   [top table u32s][2-align][63 strip offsets u16, deltas in dword units][4-align][strip bitstreams]
//
// On a corrupted stream the decode runs to completion and the output is garbage, mirroring the
// CPU version which only reports the failure after the whole strip was decoded. A valid stream
// always decodes bit-identically to the CPU version.
//-----------------------------------------------------------------------------------------------------------------------------

#include <metal_stdlib>
using namespace metal;

constant uint BC1_NUM_STRIPS = 64;
constant uint TOP_TABLE_SIZE = 256;
constant uint LE_ALPHABET_SIZE = 256;
constant uint LE_MAX_K = 8;
constant int COLOR_DELTA_OFFSET = 1 << 6; // 64: bias of the signed color deltas

typedef struct
{
    uint32_t width;
    uint32_t height;
    uint64_t input_length;
} bc1_decode_args_t;

// mirror of le_model: the decoder only needs the rank table, k and q_escape (no symbol index)
typedef struct
{
    uint8_t alphabet[LE_ALPHABET_SIZE];
    uint8_t k;          // bit 7 = raw mode flag, lower bits = raw bit width or rice k
    uint8_t q_escape;
    uint16_t num_symbols;
} model_t;

// mirror of le_stream: a 64-bit reservoir buffers bits to keep I/O at byte granularity,
// least significant bit read first
typedef struct
{
    const device uint8_t* buffer;
    uint64_t size;
    uint64_t position;
    uint64_t bit_reservoir;
    uint32_t bits_available;
    bool overrun;
} bit_stream_t;


//-----------------------------------------------------------------------------------------------------------------------------
// pull bytes into the reservoir until it's full enough for any standard read
void bs_refill(thread bit_stream_t* s)
{
    while (s->bits_available <= 56 && s->position < s->size)
    {
        s->bit_reservoir |= (uint64_t)s->buffer[s->position] << s->bits_available;
        s->bits_available += 8;
        s->position++;
    }
}

//-----------------------------------------------------------------------------------------------------------------------------
// reads [num_bits] (1 to 8) least significant bits of the reservoir
uint8_t bs_read_bits(thread bit_stream_t* s, uint32_t num_bits)
{
    if (s->bits_available < num_bits)
    {
        bs_refill(s);
        if (s->bits_available < num_bits)
        {
            s->overrun = true;
            return 0;
        }
    }

    uint32_t value = (uint32_t)(s->bit_reservoir & (((uint64_t)1 << num_bits) - 1));
    s->bit_reservoir >>= num_bits;
    s->bits_available -= num_bits;
    return (uint8_t)value;
}

//-----------------------------------------------------------------------------------------------------------------------------
uint8_t bs_read_byte(thread bit_stream_t* s)
{
    return bs_read_bits(s, 8);
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of rice_decode: q = run of ones from the LSB (terminated by a zero), then either a
// raw rank byte (q reached q_escape) or the k remainder bits
uint8_t rice_decode(thread bit_stream_t* s, uint32_t k, uint32_t q_escape)
{
    if (s->bits_available < 32)
        bs_refill(s);

    // count the consecutive ones from the LSB: the index of the first zero. bit 63 is force
    // set in the mask so the count can never run past the 64-bit word (ctz = count trailing zeros)
    uint64_t mask = ~s->bit_reservoir | (((uint64_t)1) << 63);
    uint32_t lo = (uint32_t)mask;
    uint32_t hi = (uint32_t)(mask >> 32);

    // MSL has no 64-bit ctz: count in the low half, or 32 + the high half if the low half is all ones
    uint32_t q = (lo != 0) ? (uint32_t)ctz((int)lo) : ((uint32_t)32u + (uint32_t)ctz((int)hi));

    if (q >= q_escape)
    {
        if (s->bits_available < (q_escape + 1))
        {
            s->overrun = true;
            return 0;
        }

        s->bit_reservoir >>= (q_escape + 1);
        s->bits_available -= (q_escape + 1);
        return bs_read_byte(s);
    }

    uint32_t total_bits = q + 1 + k;
    if ((s->bits_available < total_bits) || (total_bits >= 64))
    {
        s->overrun = true;
        return 0;
    }

    uint32_t val = (uint32_t)s->bit_reservoir;

    s->bit_reservoir >>= total_bits;
    s->bits_available -= total_bits;

    uint32_t r = (val >> (q + 1)) & (((uint32_t)1 << k) - 1);
    return (uint8_t)((q << k) | r);
}

//-----------------------------------------------------------------------------------------------------------------------------
// decodes one symbol: read the rank (fixed-width in raw mode, otherwise Rice), then map it
// back through the rank table
uint8_t decode_symbol(thread bit_stream_t* s, const thread model_t* m)
{
    uint8_t index = (m->k & 0x80) ? bs_read_bits(s, m->k & 0x7f) : rice_decode(s, m->k, m->q_escape);

    return m->alphabet[index];
}

// number of bits to store a color-pair dictionary reference of the block at chain position [position]:
// the reference is the distance minus one in [0, limit - 1] with limit = min(position, 256), so it
// needs ceil(log2(limit)) bits (0 for the first block, 8 once the window of 256 is full)
uint32_t color_ref_bits(uint32_t position)
{
    uint32_t limit = (position < 256) ? position : 256;
    uint32_t bits = 0;
    uint32_t value = 1;
    while (value < limit)
    {
        value <<= 1;
        bits++;
    }
    return bits;
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of load_static_model / le_static_model_load
bool load_model(const device uint8_t* input, uint64_t length, thread uint64_t* pos, thread model_t* m)
{
    if (*pos + 3 > length)
        return false;

    uint64_t num_symbols = (uint64_t)input[*pos] + 1;
    uint8_t k = input[*pos + 1];
    uint8_t q_escape = input[*pos + 2];

    // k stores the raw-mode flag in bit 7 and a width / rice parameter (<= LE_MAX_K) in the lower bits
    if ((k & 0x7f) > LE_MAX_K)
        k = (uint8_t)((k & 0x80) | LE_MAX_K);

    *pos += 3;
    if (*pos + num_symbols > length)
        return false;

    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
        m->alphabet[i] = 0;

    for (uint64_t i = 0; i < num_symbols; ++i)
        m->alphabet[i] = input[*pos + i];

    *pos += num_symbols;
    m->k = k;
    m->q_escape = q_escape;
    m->num_symbols = (uint16_t)num_symbols;

    return true;
}

//-----------------------------------------------------------------------------------------------------------------------------
// little endian u16 / u32 reads from the stream. The stream aligns strip offsets on u16 and
// top-table / seed / predictor on u32, so read the word directly (cheaper than per-byte gather)
uint16_t read_u16(const device uint8_t* input, uint64_t pos)
{
    return ((const device uint16_t*)input)[pos >> 1];
}

uint32_t read_u32(const device uint8_t* input, uint64_t pos)
{
    return ((const device uint32_t*)input)[pos >> 2];
}

//-----------------------------------------------------------------------------------------------------------------------------
// writes one decoded block (two u16 colors + u32 indices) to the row-major output grid. A block is
// 8 bytes = the two colors packed in one u32 (color0 low word, color1 high word) + the indices u32,
// 8-aligned, so it is stored as two aligned u32 words
void write_block(device uint8_t* output, uint block_index, uint16_t color0, uint16_t color1, uint32_t indices)
{
    device uint32_t* p = (device uint32_t*)(output + block_index * 8);
    p[0] = ((uint32_t)color1 << 16) | (uint32_t)color0;
    p[1] = indices;
}

//-----------------------------------------------------------------------------------------------------------------------------
// mirror of bc1_packed_decompress: decode one strip of blocks
kernel void bc1_packed_decompress(const device uint8_t* input [[buffer(0)]],
                                  device uint8_t* output [[buffer(1)]],
                                  constant bc1_decode_args_t& args [[buffer(2)]],
                                  uint3 grid_pos [[thread_position_in_grid]])
{
    if (grid_pos.x >= BC1_NUM_STRIPS)
        return;

    if ((args.width < 16) || (args.height < 256))
        return;

    const uint strip_id = grid_pos.x;
    const uint height_blocks = args.height / 4;
    const uint width_blocks = args.width / 4;
    const uint strip_width = height_blocks / BC1_NUM_STRIPS;
    const uint64_t length = args.input_length;

    // offset both input and output by run_id so each run reads from and writes to
    // a distinct memory region, exercising DRAM bandwidth instead of staying in L2
    const uint64_t image_size = (uint64_t)width_blocks * (uint64_t)height_blocks * 8;
    input += (uint64_t)grid_pos.y * length;
    output += (uint64_t)grid_pos.y * image_size;

    //-------------------------------------------------------------------------------------------------------------------------
    // load the static models: contextual red/blue per endpoint (3 contexts each) + one uncontexted
    // green per endpoint (14 color models), then the index models
    model_t red[6], green[2], blue[6];
    model_t reference_model, mask_model, difference_model;

    thread uint64_t pos = 0;
    for (uint j = 0; j < 2; ++j)
    {
        for (uint c = 0; c < 3; ++c)
            if (!load_model(input, length, &pos, &red[j * 3 + c]))
                return;

        if (!load_model(input, length, &pos, &green[j]))
            return;

        for (uint c = 0; c < 3; ++c)
            if (!load_model(input, length, &pos, &blue[j * 3 + c]))
                return;
    }

    if (!load_model(input, length, &pos, &reference_model))
        return;
    if (!load_model(input, length, &pos, &mask_model))
        return;
    if (!load_model(input, length, &pos, &difference_model))
        return;

    //-------------------------------------------------------------------------------------------------------------------------
    // top table + strip seed + predictor flags
    if (pos >= length)
        return;

    uint top_table_size = input[pos++] + 1;
    pos = (pos + 3) & ~(uint64_t)3; // 4-byte align

    if (pos + 20 > length)
        return;

    const device uint32_t* input32 = (const device uint32_t*)input;

    uint32_t strip_seed = input32[pos >> 2];
    pos += 4;

    // the predictor is 8 bytes but only 4-aligned right after the seed, so read it as two u32
    uint64_t strip_predictor = ((uint64_t)input32[(pos >> 2) + 1] << 32) | (uint64_t)input32[pos >> 2];
    pos += 8;

    // same for the per-strip color-pair dictionary enable word
    uint64_t strip_dict_enable = ((uint64_t)input32[(pos >> 2) + 1] << 32) | (uint64_t)input32[pos >> 2];
    pos += 8;

    uint32_t top_table[TOP_TABLE_SIZE];
    for (uint i = 0; i < TOP_TABLE_SIZE; ++i)
        top_table[i] = 0;

    for (uint i = 0; i < top_table_size; ++i)
    {
        if (pos + 4 > length)
            return;

        top_table[i] = read_u32(input, pos);
        pos += 4;
    }

    //-------------------------------------------------------------------------------------------------------------------------
    // strip offsets (deltas in dword units), then the strip bitstream of this thread
    pos = (pos + 1) & ~(uint64_t)1; // 2-byte align

    if (pos + (BC1_NUM_STRIPS - 1) * 2 >= length)
        return;

    // strip 0 always starts the strip data so its offset is zero and not stored, the stored
    // deltas cover strips 1..N-1
    uint64_t strip_offset = pos + (BC1_NUM_STRIPS - 1) * 2;
    strip_offset = (strip_offset + 3) & ~(uint64_t)3; // 4-byte align, as done on encode

    for (uint i = 0; i < strip_id; ++i)
        strip_offset += (uint64_t)read_u16(input, pos + i * 2) * 4;

    if (strip_offset >= length)
        return;

    bit_stream_t compressed_stream;
    compressed_stream.buffer = input + strip_offset;
    compressed_stream.size = length - strip_offset;
    compressed_stream.position = 0;
    compressed_stream.bit_reservoir = 0;
    compressed_stream.bits_available = 0;
    compressed_stream.overrun = false;
    bs_refill(&compressed_stream);

    //-------------------------------------------------------------------------------------------------------------------------
    // decode the strip block by block, zigzag order
    // seed "previous block" of the strip: the two 16-bit 565 colors packed in strip_seed, color[0] in the low word
    uint16_t previous_color0 = (uint16_t)strip_seed;
    uint16_t previous_color1 = (uint16_t)(strip_seed >> 16);
    const bool use_top = ((strip_predictor >> strip_id) & 1) != 0;
    const bool dict_enabled = ((strip_dict_enable >> strip_id) & 1) != 0;
    const uint start_y = strip_id * strip_width;

    // the output is read back for the top prediction: the two endpoint colors of a block are packed in one u32
    const device uint32_t* output_colors32 = (const device uint32_t*)output; // already offset by run_id

    // thread-local sliding dictionary of the last 256 decoded endpoint color pairs of the zigzag chain:
    // a fixed ring buffer keyed by chain position (mirrors COLOR_DICT_SIZE in src/bc1_packed.c)
    uint32_t dict[256];
    for (uint i = 0; i < 256; ++i)
        dict[i] = 0;

    for (uint y = start_y; y < start_y + strip_width; ++y)
    {
        for (uint x = 0; x < width_blocks; ++x)
        {
            uint zigzag_x = (y & 1) ? x : width_blocks - x - 1;
            uint block_index = y * width_blocks + zigzag_x;
            uint position = (y - start_y) * width_blocks + x;

            uint16_t current_color0 = 0;
            uint16_t current_color1 = 0;

            // color payload: an enabled strip writes a 1-bit flag, then either a dictionary reference
            // (copy the two endpoint colors of the referenced block, skipping the six delta symbols)
            // or the six deltas; a disabled strip writes the six deltas directly, no flag
            uint8_t match = dict_enabled ? bs_read_bits(&compressed_stream, 1) : 0;
            if (match)
            {
                uint8_t ref = bs_read_bits(&compressed_stream, color_ref_bits(position));
                uint32_t pair = dict[(position - ref - 1) & 255];
                current_color0 = (uint16_t)pair;
                current_color1 = (uint16_t)(pair >> 16);
            }
            else
            {
                // first scanline of a strip is always predicted from the seed via the zigzag-previous chain,
                // after that a top-flagged strip is predicted from the row above, same zigzag position
                uint16_t reference_color0 = previous_color0;
                uint16_t reference_color1 = previous_color1;

                if (use_top && y > start_y)
                {
                    uint32_t previous_colors = output_colors32[((uint64_t)(y - 1) * width_blocks + zigzag_x) * 2];
                    reference_color0 = (uint16_t)previous_colors;
                    reference_color1 = (uint16_t)(previous_colors >> 16);
                }

                for (uint j = 0; j < 2; ++j)
                {
                    uint16_t reference_color = (j == 0) ? reference_color0 : reference_color1;

                    uint8_t reference_red = (uint8_t)((reference_color >> 11) & 0x1F);
                    uint8_t reference_green = (uint8_t)((reference_color >> 5) & 0x3F);
                    uint8_t reference_blue = (uint8_t)(reference_color & 0x1F);

                    uint8_t green_symbol = decode_symbol(&compressed_stream, &green[j]);

                    // red and blue delta are based on green delta
                    int dgreen_orig = (int)green_symbol - COLOR_DELTA_OFFSET;
                    uint8_t chroma_context = (dgreen_orig < 0) ? 0 : ((dgreen_orig > 0) ? 1 : 2);

                    uint8_t delta_red = decode_symbol(&compressed_stream, &red[j * 3 + chroma_context]);
                    uint8_t delta_blue = decode_symbol(&compressed_stream, &blue[j * 3 + chroma_context]);

                    int dgreen_halved = dgreen_orig / 2;

                    int current_red = (int)reference_red + ((int)delta_red - COLOR_DELTA_OFFSET) + dgreen_halved;
                    int current_green = (int)reference_green + dgreen_orig;
                    int current_blue = (int)reference_blue + ((int)delta_blue - COLOR_DELTA_OFFSET) + dgreen_halved;

                    uint16_t current_color = (uint16_t)(((current_red & 0x1F) << 11) | ((current_green & 0x3F) << 5) | (current_blue & 0x1F));

                    if (j == 0)
                        current_color0 = current_color;
                    else
                        current_color1 = current_color;
                }
            }

            // indices difference with top table
            uint8_t reference = decode_symbol(&compressed_stream, &reference_model);
            uint8_t mask = decode_symbol(&compressed_stream, &mask_model);

            uint32_t difference = 0;
            for (uint j = 0; j < 4; ++j)
                if (mask & (1 << j))
                    difference |= (uint32_t)decode_symbol(&compressed_stream, &difference_model) << (j * 8);

            uint32_t indices = difference ^ top_table[reference];

            write_block(output, block_index, current_color0, current_color1, indices);

            dict[position & 255] = ((uint32_t)current_color1 << 16) | (uint32_t)current_color0;

            // the previous block of the zigzag chain becomes the block just decoded
            previous_color0 = current_color0;
            previous_color1 = current_color1;
        }
    }
}
