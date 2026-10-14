//-----------------------------------------------------------------------------------------------------------------------------
// bc_decompress.metal
//
// GPU mirror of the CPU decompressors of src/bc_packed.c: one thread decompresses one strip of
// a bc_packed stream. The compressed stream is read from a device buffer, the decoded standard
// blocks are written to an output device buffer laid out as a row-major grid of
// width/4 x height/4 blocks:
//   "bcp_decompress_bc1"  the BC1 blocks: two 16-bit 565 endpoint colors followed by the
//                         32-bit indices, 8 bytes per block
//   "bcp_decompress_bc4"  the standard BC4 / BC5 blocks: two 8-bit endpoints followed by the
//   "bcp_decompress_bc5"  48-bit little-endian field of the 16 3-bit indices, 8 bytes per channel
//                         block, the red channel first for BC5 (16 bytes per block)
//
// Dispatch 64 threads, one per strip:
//
//     decode_args_t args = { width, height, (uint64_t)stream_length };
//     encoder.setComputePipelineState(pipeline);   // pipeline built from the "bcp_decompress_bc?" kernel
//     encoder.setBytes(&args, sizeof(args), 2);
//     encoder.setBuffer(stream_buffer, NULL, 0, 0);
//     encoder.setBuffer(output_buffer, NULL, 1, 0);
//     encoder.dispatchThreads(threadgrid(64, 1, 1));
//
// buffer(0): device const uint8_t*  the compressed stream (output of bcp_compress_bc1 / bcp_compress_bc4 / bcp_compress_bc5)
// buffer(1): device uint8_t*        output blocks, width/4 * height/4 * 8 bytes per channel block
// buffer(2): constant decode_args_t
//
// The stream layout parsed here mirrors the CPU compressors / decompressors:
//   BC1       [17 static models][top table size u8][4-align][strip seed u32][strip predictor u64][strip dict enable u64]
//             [top table u32s][2-align][63 strip offsets u16, deltas in dword units][4-align][strip bitstreams]
//   BC4 / BC5 [2 endpoint models per channel][64 index models per channel][strip seed, 2 bytes per channel]
//             [2-align][63 strip offsets u16, deltas in dword units][4-align][strip bitstreams]
//
// On a corrupted stream the decode runs to completion and the output is garbage, mirroring the
// CPU version which only reports the failure after the whole strip was decoded. A valid stream
// always decodes bit-identically to the CPU version.
//-----------------------------------------------------------------------------------------------------------------------------

#include <metal_stdlib>
using namespace metal;

constant uint NUM_STRIPS = 64; // 64 independent strips, one decoded thread per strip, all formats
constant uint TOP_TABLE_SIZE = 256;
constant uint LE_ALPHABET_SIZE = 256;
constant uint BC4_INDEX_ALPHABET_SIZE = 8; // bc4/bc5 index models: 8 prediction values
constant uint BC4_INDEX_DIM = 8;           // bc4/bc5 index context: the 8 x 8 [left][top] matrix
constant uint BC4_INDEX_CONTEXTS = BC4_INDEX_DIM * BC4_INDEX_DIM;
constant uint LE_MAX_K = 8;
constant int COLOR_DELTA_OFFSET = 1 << 6; // 64: bias of the signed color deltas

// shared by every kernel: the stream carries no width / height / format, the caller provides them
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint64_t input_length;
} decode_args_t;

// mirror of le_model: the decoder only needs the rank table, k and q_escape (no symbol index)
typedef struct
{
    uint8_t alphabet[LE_ALPHABET_SIZE];
    uint8_t k;          // bit 7 = raw mode flag, lower bits = raw bit width or rice k
    uint8_t q_escape;
    uint16_t num_symbols;
} model_t;

// mirror of the bc4/bc5 8-symbol le_model: the rank table only needs the 8 rank entries (see model_t)
typedef struct
{
    uint8_t alphabet[BC4_INDEX_ALPHABET_SIZE];
    uint8_t k;          // bit 7 = raw mode flag, lower bits = raw bit width or rice k
    uint8_t q_escape;
    uint16_t num_symbols;
} model8_t;

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

// mirror of le_decode_symbol for the compact 8-symbol bc4/bc5 index models: the CPU's 256-entry
// rank table is zero-filled past num_symbols, so an out-of-range rank (corrupted stream) reads 0
// there; the 8-entry table cannot, so the bound check emulates it (valid streams always have
// rank < num_symbols, the encoder only writes the ranks of the symbols it fitted)
uint8_t decode_symbol8(thread bit_stream_t* s, const thread model8_t* m)
{
    uint8_t index = (m->k & 0x80) ? bs_read_bits(s, m->k & 0x7f) : rice_decode(s, m->k, m->q_escape);

    return (index < m->num_symbols) ? m->alphabet[index] : 0;
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
// mirror of load_static_model for the compact 8-symbol bc4/bc5 index models: a corrupted stream
// may claim up to 256 symbols, so only the first 8 are stored in the rank table and num_symbols
// is clamped with them; the CPU's zero-filled 256-entry table returns 0 for the same out-of-range
// ranks (decode_symbol8), so both still decode the same bytes
bool load_model8(const device uint8_t* input, uint64_t length, thread uint64_t* pos, thread model8_t* m)
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

    for (uint32_t i = 0; i < BC4_INDEX_ALPHABET_SIZE; ++i)
        m->alphabet[i] = 0;

    uint64_t stored = (num_symbols > BC4_INDEX_ALPHABET_SIZE) ? BC4_INDEX_ALPHABET_SIZE : num_symbols;

    for (uint64_t i = 0; i < stored; ++i)
        m->alphabet[i] = input[*pos + i];

    *pos += num_symbols;
    m->k = k;
    m->q_escape = q_escape;
    m->num_symbols = (uint16_t)stored;

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
// mirror of bcp_decompress_bc1: decode one strip of blocks
kernel void bcp_decompress_bc1(const device uint8_t* input [[buffer(0)]],
                                  device uint8_t* output [[buffer(1)]],
                                  constant decode_args_t& args [[buffer(2)]],
                                  uint3 grid_pos [[thread_position_in_grid]])
{
    if (grid_pos.x >= NUM_STRIPS)
        return;

    if ((args.width < 16) || (args.height < 256))
        return;

    const uint strip_id = grid_pos.x;
    const uint height_blocks = args.height / 4;
    const uint width_blocks = args.width / 4;
    const uint strip_width = height_blocks / NUM_STRIPS;
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

    if (pos + (NUM_STRIPS - 1) * 2 >= length)
        return;

    // strip 0 always starts the strip data so its offset is zero and not stored, the stored
    // deltas cover strips 1..N-1
    uint64_t strip_offset = pos + (NUM_STRIPS - 1) * 2;
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
    // a fixed ring buffer keyed by chain position (mirrors COLOR_DICT_SIZE in src/bc_packed.c)
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


//-----------------------------------------------------------------------------------------------------------------------------
// bcp_decompress_bc45: GPU mirror of bcp_decompress_bc45 (src/bc_packed.c) for the BC4 / BC5
// stream — one thread decompresses one strip, the standard block layout is: two 8-bit endpoints
// followed by the 48-bit little-endian field of the 16 3-bit indices, 8 bytes per channel block,
// the red channel first for BC5. [NumChannels] is 1 for BC4, 2 for BC5, so both kernels share
// the same body with compile-time-sized model arrays.
//
// mirror of bc4_endpoint_predict (src/bc_packed.c): the first row of a strip is predicted from
// the chain-previous block (the strip seed for the first block), a row start from the top block
// only, and every other block from the 2D predictor previous + top - top_previous
uint8_t bc4_endpoint_predict(bool first_row, bool row_start, uint8_t prev, uint8_t top, uint8_t top_prev)
{
    if (first_row)
        return prev;
    if (row_start)
        return top;

    return (uint8_t)(prev + top - top_prev);
}

// mirror of bc4_index_context (src/bc_packed.c): the 8 x 8 [left][top] matrix context, a
// missing neighbor (first row, row start) defaults to 0
uint8_t bc4_index_context(uint8_t left, uint8_t top)
{
    return (uint8_t)(left * BC4_INDEX_DIM + top);
}

template <uint NumChannels>
void bc45_decode_strip(const device uint8_t* input,
                       device uint8_t* output,
                       constant decode_args_t& args,
                       uint3 grid_pos)
{
    if ((args.width < 16) || (args.height < 256) || (grid_pos.x >= NUM_STRIPS))
        return;

    const uint strip_id = grid_pos.x;
    const uint height_blocks = args.height / 4;
    const uint width_blocks = args.width / 4;
    const uint strip_width = height_blocks / NUM_STRIPS;
    const uint64_t length = args.input_length;

    // offset both input and output by run_id so each run reads from and writes to
    // a distinct memory region, exercising DRAM bandwidth instead of staying in L2
    const uint64_t image_size = (uint64_t)width_blocks * (uint64_t)height_blocks * 8 * NumChannels;
    input += (uint64_t)grid_pos.y * length;
    output += (uint64_t)grid_pos.y * image_size;

    //-------------------------------------------------------------------------------------------------------------------------
    // load the static models: per channel, the 2 endpoint models (256 symbols) then the 64
    // compact index models (8 symbols)
    model_t endpoint[NumChannels][2];
    model8_t index[NumChannels][BC4_INDEX_CONTEXTS];

    thread uint64_t pos = 0;
    for (uint c = 0; c < NumChannels; ++c)
    {
        for (uint e = 0; e < 2; ++e)
            if (!load_model(input, length, &pos, &endpoint[c][e]))
                return;

        for (uint i = 0; i < BC4_INDEX_CONTEXTS; ++i)
            if (!load_model8(input, length, &pos, &index[c][i]))
                return;
    }

    // strip seed: 2 bytes per channel, endpoint 0 then endpoint 1
    if (pos + 2 * NumChannels > length)
        return;

    uint8_t seed[NumChannels][2];
    for (uint c = 0; c < NumChannels; ++c)
    {
        for (uint e = 0; e < 2; ++e)
            seed[c][e] = input[pos++];
    }

    //-------------------------------------------------------------------------------------------------------------------------
    // strip offsets (deltas in dword units), then the strip bitstream of this thread
    pos = (pos + 1) & ~(uint64_t)1; // 2-byte align

    if (pos + (NUM_STRIPS - 1) * 2 >= length)
        return;

    // strip 0 always starts the strip data so its offset is zero and not stored, the stored
    // deltas cover strips 1..N-1
    uint64_t strip_offset = pos + (NUM_STRIPS - 1) * 2;
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
    // seed "previous block" of the strip: one byte per channel and endpoint
    uint8_t prev_e[NumChannels][2], top_e[NumChannels][2], top_prev_e[NumChannels][2];
    for (uint c = 0; c < NumChannels; ++c)
    {
        for (uint e = 0; e < 2; ++e)
        {
            prev_e[c][e] = seed[c][e];
            top_e[c][e] = 0;
            top_prev_e[c][e] = 0;
        }
    }

    const uint start_y = strip_id * strip_width;

    // the output is read back for the top prediction: the two endpoints of a channel block
    // sit in the low bytes of one u32 (the block is 8-aligned)

    for (uint y = start_y; y < start_y + strip_width; ++y)
    {
        for (uint x = 0; x < width_blocks; ++x)
        {
            // zig-zag pattern, same chain as BC1
            uint zigzag_x = (y & 1) ? x : width_blocks - x - 1;
            const bool first_row = (y == start_y);
            const bool row_start = (x == 0);
            const uint64_t block_index = (uint64_t)y * width_blocks + zigzag_x;

            for (uint c = 0; c < NumChannels; ++c)
            {
                device uint8_t* b = output + (block_index * NumChannels + c) * 8;
                uint8_t idx[16];

                if (!first_row)
                {
                    const device uint32_t* t = (const device uint32_t*)(output + ((uint64_t)(y - 1) * width_blocks + zigzag_x) * NumChannels * 8 + c * 8);
                    uint32_t top = t[0];
                    top_e[c][0] = (uint8_t)(top & 0xFF);
                    top_e[c][1] = (uint8_t)((top >> 8) & 0xFF);
                }

                // endpoint payload: the two wrapped deltas against the strip-chain predictor
                uint8_t e[2];
                for (uint j = 0; j < 2; ++j)
                {
                    uint8_t pred = bc4_endpoint_predict(first_row, row_start, prev_e[c][j], top_e[c][j], top_prev_e[c][j]);

                    e[j] = (uint8_t)(pred + decode_symbol(&compressed_stream, &endpoint[c][j]));
                }

                // index payload: the first index in raw 3 bits, then each index xored with its
                // intra-block prediction, contextual on the 8 x 8 [left][top] matrix
                idx[0] = bs_read_bits(&compressed_stream, 3);

                for (uint i = 1; i < 16; ++i)
                {
                    // the missing neighbor of the pixel (first row, row start) defaults to 0
                    uint8_t left = ((i & 3) == 0) ? 0 : idx[i - 1];
                    uint8_t top = (i < 4) ? 0 : idx[i - 4];
                    uint8_t p = ((i & 3) == 0) ? top : left;

                    idx[i] = (uint8_t)(p ^ decode_symbol8(&compressed_stream, &index[c][bc4_index_context(left, top)]));
                }

                // the standard block: two endpoints + the 48-bit little-endian index field, as two aligned u32 words
                uint64_t v = 0;
                for (uint i = 0; i < 16; ++i)
                    v |= ((uint64_t)idx[i] & 7) << (3 * i);

                device uint32_t* p32 = (device uint32_t*)b;
                p32[0] = (uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)v << 16);
                p32[1] = (uint32_t)(v >> 16);

                if (!first_row)
                {
                    top_prev_e[c][0] = top_e[c][0];
                    top_prev_e[c][1] = top_e[c][1];
                }
                prev_e[c][0] = e[0];
                prev_e[c][1] = e[1];
            }
        }
    }
}

kernel void bcp_decompress_bc4(const device uint8_t* input [[buffer(0)]],
                               device uint8_t* output [[buffer(1)]],
                               constant decode_args_t& args [[buffer(2)]],
                               uint3 grid_pos [[thread_position_in_grid]])
{
    bc45_decode_strip<1>(input, output, args, grid_pos);
}

kernel void bcp_decompress_bc5(const device uint8_t* input [[buffer(0)]],
                               device uint8_t* output [[buffer(1)]],
                               constant decode_args_t& args [[buffer(2)]],
                               uint3 grid_pos [[thread_position_in_grid]])
{
    bc45_decode_strip<2>(input, output, args, grid_pos);
}
