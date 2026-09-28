#ifndef LITE_DECODING_METAL_H
#define LITE_DECODING_METAL_H

#include <metal_stdlib>

using namespace metal;

#define LE_ALPHABET_SIZE (256U)
#define LE_K_TREND_THRESHOLD (12)
#define LE_Q_ESCAPE_SIZE (10)

constant uchar q_escape_for_k[LE_Q_ESCAPE_SIZE] = {16, 10, 4, 6, 255, 255, 255, 255, 255, 255};

struct le_stream
{
    const device uint32_t* buffer;
    size_t word_position;

    uint64_t bit_reservoir;
    uint32_t bits_available;
};

struct le_model
{
    uchar alphabet[LE_ALPHABET_SIZE];
    uint32_t k;
    int32_t k_trend;
    uint32_t num_symbols;
    bool is_static;
};

//----------------------------------------------------------------------------------------------------------------------------
inline void le_refill(thread le_stream& s)
{
    if (s.bits_available <= 32)
    {
        s.bit_reservoir |= ((uint64_t)s.buffer[s.word_position++]) << s.bits_available;
        s.bits_available += 32;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
inline void le_begin_decode(thread le_stream& s, const device uint32_t* buffer)
{
    s.buffer = buffer;
    s.word_position = 0;
    s.bit_reservoir = 0;
    s.bits_available = 0;
    le_refill(s);
}

//----------------------------------------------------------------------------------------------------------------------------
inline uchar le_read_byte(thread le_stream& s)
{
    if (s.bits_available < 8)
        le_refill(s);

    uchar value = (uchar)(s.bit_reservoir & 0xFF);
    s.bit_reservoir >>= 8;
    s.bits_available -= 8;
    return value;
}

//----------------------------------------------------------------------------------------------------------------------------
inline uchar rice_decode(thread le_stream& s, uchar k)
{
    k = (k < LE_Q_ESCAPE_SIZE) ? k : (LE_Q_ESCAPE_SIZE - 1);
    if (s.bits_available <= 24)
        le_refill(s);

    uint32_t q = ctz(~s.bit_reservoir);
    uint32_t q_limit = q_escape_for_k[k];

    if (q >= q_limit)
    {
        s.bit_reservoir >>= (q_limit + 1);
        s.bits_available -= (q_limit + 1);
        return le_read_byte(s);
    }

    uint32_t total_bits = q + 1 + k;
    uint32_t val = s.bit_reservoir;

    s.bit_reservoir >>= total_bits;
    s.bits_available -= total_bits;

    uint32_t r = (val >> (q + 1)) & ((1U << k) - 1U);
    return (uchar)((q << k) | r);
}

//----------------------------------------------------------------------------------------------------------------------------
inline void le_model_update_k(thread le_model& model, uchar value)
{
    if (value < (1U << model.k) && model.k > 0)
        model.k_trend--;
    else if (value > (3U << model.k) && model.k < 7)
        model.k_trend++;

    if (model.k_trend > LE_K_TREND_THRESHOLD)
    {
        model.k++;
        model.k_trend = 0;
    }
    else if (model.k_trend < -LE_K_TREND_THRESHOLD)
    {
        model.k--;
        model.k_trend = 0;
    }
}

//----------------------------------------------------------------------------------------------------------------------------
inline void le_model_promote(thread le_model& model, uint32_t index)
{
    if (index == 0 || model.k >= 6)
        return;

    uint32_t target = index / 2;
    uchar value = model.alphabet[index];

    for (uint32_t i = index; i > target; --i)
    {
        model.alphabet[i] = model.alphabet[i - 1];
    }

    model.alphabet[target] = value;
}

//----------------------------------------------------------------------------------------------------------------------------
inline void le_dynamic_model_init(thread le_model& model)
{
    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
        model.index[i] = (uchar)i;

    model.k = 2;
    model.k_trend = 0;
    model.is_static = false;
    model.num_symbols = LE_ALPHABET_SIZE;
}

//----------------------------------------------------------------------------------------------------------------------------
inline void le_static_model_load(thread le_model& model, const device uchar* alphabet, uint32_t num_symbols, uint32_t k)
{
    for (uint32_t i = 0; i < LE_ALPHABET_SIZE; ++i)
        model.alphabet[i] = (i < num_symbols) ? alphabet[i] : 0;

    model.is_static = true;
    model.k = k;
    model.k_trend = 0;
    model.num_symbols = num_symbols;
}

//----------------------------------------------------------------------------------------------------------------------------
inline uchar le_decode_symbol(thread le_stream& s, thread le_model& model)
{
    uchar index = rice_decode(s, model.k);
    uchar value = model.alphabet[index];

    if (!model.is_static)
    {
        le_model_promote(model, index);
        le_model_update_k(model, index);
    }

    return value;
}

//----------------------------------------------------------------------------------------------------------------------------
inline char zigzag8_decode(uchar v)
{
    return (char)((v >> 1) ^ -(char)(v & 1));
}

//----------------------------------------------------------------------------------------------------------------------------
inline uchar le_decode_literal(thread le_stream& s, thread le_model& model)
{
    uchar value = rice_decode(s, model.k);
    le_model_update_k(model, value);
    return value;
}

//----------------------------------------------------------------------------------------------------------------------------
inline char le_decode_delta(thread le_stream& s, thread le_model& model)
{
    uchar zz = rice_decode(s, model.k);
    le_model_update_k(model, zz);
    return zigzag8_decode(zz);
}

#endif