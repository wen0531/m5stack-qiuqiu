/* IMA-ADPCM (4-bit) encoder/decoder — standard IMA algorithm. */
#include "adpcm.h"

static const int8_t s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
    13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

void adpcm_reset(adpcm_state_t *st)
{
    st->predictor = 0;
    st->step_index = 0;
}

static uint8_t encode_nibble(adpcm_state_t *st, int16_t sample)
{
    int32_t step = s_step_table[st->step_index];
    int32_t diff = (int32_t)sample - st->predictor;
    uint8_t code = 0;

    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    step >>= 2;
    if (diff >= step) {
        code |= 2;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
    }

    /* Reconstruct decoder predictor */
    step = s_step_table[st->step_index];
    int32_t vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;
    if (code & 8) {
        st->predictor = (int16_t)(st->predictor - vpdiff);
    } else {
        st->predictor = (int16_t)(st->predictor + vpdiff);
    }

    st->step_index = (int8_t)(st->step_index + s_index_table[code]);
    if (st->step_index < 0) st->step_index = 0;
    if (st->step_index > 88) st->step_index = 88;

    return code;
}

void adpcm_encode_block(adpcm_state_t *st, const int16_t *in, size_t n, uint8_t *out)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        uint8_t lo = encode_nibble(st, in[i]);
        uint8_t hi = encode_nibble(st, in[i + 1]);
        out[i / 2] = (uint8_t)((hi << 4) | (lo & 0x0F));
    }
    if (i < n) {
        uint8_t lo = encode_nibble(st, in[i]);
        out[i / 2] = (uint8_t)(lo & 0x0F);
    }
}

static int16_t decode_nibble(adpcm_state_t *st, uint8_t code)
{
    int32_t step = s_step_table[st->step_index];
    int32_t vpdiff = step >> 3;

    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;

    if (code & 8) {
        st->predictor = (int16_t)(st->predictor - vpdiff);
    } else {
        st->predictor = (int16_t)(st->predictor + vpdiff);
    }

    st->step_index = (int8_t)(st->step_index + s_index_table[code & 0x07]);
    if (st->step_index < 0) st->step_index = 0;
    if (st->step_index > 88) st->step_index = 88;
    return st->predictor;
}

void adpcm_decode_block(adpcm_state_t *st, const uint8_t *in, size_t n, int16_t *out)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) {
        uint8_t v = in[i / 2];
        out[i] = decode_nibble(st, (uint8_t)(v & 0x0F));
        out[i + 1] = decode_nibble(st, (uint8_t)((v >> 4) & 0x0F));
    }
    if (i < n) {
        out[i] = decode_nibble(st, (uint8_t)(in[i / 2] & 0x0F));
    }
}
