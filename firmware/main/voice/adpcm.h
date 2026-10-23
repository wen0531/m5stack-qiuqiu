/* IMA-ADPCM (4-bit) encoder/decoder — mono 16-bit PCM */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;
    int8_t step_index;
} adpcm_state_t;

void adpcm_reset(adpcm_state_t *st);

/* Encode n samples (n even count preferred) into n/2 bytes (low nibble first). */
void adpcm_encode_block(adpcm_state_t *st, const int16_t *in, size_t n, uint8_t *out);

/* Decode n/2 bytes back into n samples. */
void adpcm_decode_block(adpcm_state_t *st, const uint8_t *in, size_t n, int16_t *out);

#ifdef __cplusplus
}
#endif
