#include "mac_voice_adpcm.h"

static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t k_index_adjust[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static int16_t clamp16(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static uint8_t encode_nibble(mac_voice_adpcm_t *state, int16_t sample)
{
    int diff = (int)sample - (int)state->predictor;
    uint8_t nibble = 0;
    int step;
    int delta;
    int index;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    index = state->index;
    if (index < 0) index = 0;
    if (index > MAC_VOICE_ADPCM_INDEX_MAX) index = MAC_VOICE_ADPCM_INDEX_MAX;
    step = k_step[index];
    delta = step >> 3;
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 2;
        diff -= step;
        delta += step;
    }
    step >>= 1;
    if (diff >= step) {
        nibble |= 1;
        delta += step;
    }
    if (nibble & 8) state->predictor = clamp16((int)state->predictor - delta);
    else state->predictor = clamp16((int)state->predictor + delta);
    index += k_index_adjust[nibble & 0x0F];
    if (index < 0) index = 0;
    if (index > MAC_VOICE_ADPCM_INDEX_MAX) index = MAC_VOICE_ADPCM_INDEX_MAX;
    state->index = (int8_t)index;
    return (uint8_t)(nibble & 0x0F);
}

static int16_t decode_nibble(mac_voice_adpcm_t *state, uint8_t nibble)
{
    int step;
    int delta;
    int index = state->index;

    nibble &= 0x0F;
    if (index < 0) index = 0;
    if (index > MAC_VOICE_ADPCM_INDEX_MAX) index = MAC_VOICE_ADPCM_INDEX_MAX;
    step = k_step[index];
    delta = step >> 3;
    if (nibble & 4) delta += step;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 8) state->predictor = clamp16((int)state->predictor - delta);
    else state->predictor = clamp16((int)state->predictor + delta);
    index += k_index_adjust[nibble];
    if (index < 0) index = 0;
    if (index > MAC_VOICE_ADPCM_INDEX_MAX) index = MAC_VOICE_ADPCM_INDEX_MAX;
    state->index = (int8_t)index;
    return state->predictor;
}

size_t mac_voice_adpcm_encode(mac_voice_adpcm_t *state, const int16_t *pcm,
                              size_t samples, uint8_t *out)
{
    size_t i;
    if (!state || !pcm || !out || (samples % 2) != 0) return 0;
    for (i = 0; i < samples; i++) {
        uint8_t nibble = encode_nibble(state, pcm[i]);
        if ((i % 2) == 0) out[i / 2] = nibble;
        else out[i / 2] |= (uint8_t)(nibble << 4);
    }
    return samples / 2;
}

size_t mac_voice_adpcm_decode(mac_voice_adpcm_t *state, const uint8_t *in,
                              size_t samples, int16_t *pcm)
{
    size_t i;
    if (!state || !in || !pcm || (samples % 2) != 0) return 0;
    for (i = 0; i < samples; i++) {
        uint8_t nibble = ((i % 2) == 0) ? (in[i / 2] & 0x0F) : (in[i / 2] >> 4);
        pcm[i] = decode_nibble(state, nibble);
    }
    return samples;
}

size_t mac_voice_pack_start(uint8_t *dst, size_t cap)
{
    if (!dst || cap < 3) return 0;
    dst[0] = MAC_VOICE_FRAME_AUDIO_START;
    dst[1] = 0x80;
    dst[2] = 0x3E;
    return 3;
}

size_t mac_voice_pack_audio(uint8_t *dst, size_t cap, uint16_t seq,
                            mac_voice_adpcm_t state, uint16_t samples,
                            const uint8_t *nibbles, size_t nbytes)
{
    if (!dst || !nibbles || cap < 8 + nbytes || samples == 0) return 0;
    dst[0] = MAC_VOICE_FRAME_AUDIO;
    dst[1] = (uint8_t)(seq & 0xFF);
    dst[2] = (uint8_t)((seq >> 8) & 0xFF);
    dst[3] = (uint8_t)(state.predictor & 0xFF);
    dst[4] = (uint8_t)((state.predictor >> 8) & 0xFF);
    dst[5] = (uint8_t)state.index;
    dst[6] = (uint8_t)(samples & 0xFF);
    dst[7] = (uint8_t)((samples >> 8) & 0xFF);
    for (size_t i = 0; i < nbytes; i++) dst[8 + i] = nibbles[i];
    return 8 + nbytes;
}

size_t mac_voice_pack_end(uint8_t *dst, size_t cap, uint8_t flags)
{
    if (!dst || cap < 2) return 0;
    dst[0] = MAC_VOICE_FRAME_AUDIO_END;
    dst[1] = flags;
    return 2;
}

size_t mac_voice_pack_clear(uint8_t *dst, size_t cap, uint16_t count)
{
    if (!dst || cap < 3) return 0;
    dst[0] = MAC_VOICE_FRAME_CLEAR;
    dst[1] = (uint8_t)(count & 0xFF);
    dst[2] = (uint8_t)((count >> 8) & 0xFF);
    return 3;
}

size_t mac_voice_pack_simple(uint8_t *dst, size_t cap, uint8_t type)
{
    if (!dst || cap < 1) return 0;
    dst[0] = type;
    return 1;
}
