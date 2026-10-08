#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAC_VOICE_ADPCM_INDEX_MAX 88

typedef struct {
    int16_t predictor;
    int8_t index;
} mac_voice_adpcm_t;

size_t mac_voice_adpcm_encode(mac_voice_adpcm_t *state, const int16_t *pcm,
                              size_t samples, uint8_t *out);
size_t mac_voice_adpcm_decode(mac_voice_adpcm_t *state, const uint8_t *in,
                              size_t samples, int16_t *pcm);

enum {
    MAC_VOICE_FRAME_AUDIO_START = 0x02,
    MAC_VOICE_FRAME_AUDIO = 0x03,
    MAC_VOICE_FRAME_AUDIO_END = 0x04,
    MAC_VOICE_FRAME_DELETE = 0x05,
    MAC_VOICE_FRAME_SEND = 0x06,
    MAC_VOICE_FRAME_CLEAR = 0x07,
};

enum {
    MAC_VOICE_END_GAP = 0x01,
    MAC_VOICE_END_FAIL = 0x02,
};

size_t mac_voice_pack_start(uint8_t *dst, size_t cap);
size_t mac_voice_pack_audio(uint8_t *dst, size_t cap, uint16_t seq,
                            mac_voice_adpcm_t state, uint16_t samples,
                            const uint8_t *nibbles, size_t nbytes);
size_t mac_voice_pack_end(uint8_t *dst, size_t cap, uint8_t flags);
size_t mac_voice_pack_clear(uint8_t *dst, size_t cap, uint16_t count);
size_t mac_voice_pack_simple(uint8_t *dst, size_t cap, uint8_t type);
