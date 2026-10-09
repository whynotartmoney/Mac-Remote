#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAC_VOICE_TEXT_MAX 480
#define MAC_VOICE_SHAPE_COUNT 5

typedef enum {
    MAC_VOICE_PHASE_IDLE = 0,
    MAC_VOICE_PHASE_LISTENING,
    MAC_VOICE_PHASE_TRANSCRIBING,
    MAC_VOICE_PHASE_REVIEW,
} mac_voice_phase_t;

typedef enum {
    MAC_VOICE_SHAPE_ARC = 0,
    MAC_VOICE_SHAPE_SQUARE,
    MAC_VOICE_SHAPE_TRIANGLE,
    MAC_VOICE_SHAPE_RECT,
    MAC_VOICE_SHAPE_PILL,
} mac_voice_shape_t;

typedef enum {
    MAC_VOICE_STATUS_HOLD = 0,
    MAC_VOICE_STATUS_NO_LINK,
    MAC_VOICE_STATUS_WAIT,
    MAC_VOICE_STATUS_NO_MIC,
    MAC_VOICE_STATUS_LISTENING,
    MAC_VOICE_STATUS_WORKING,
    MAC_VOICE_STATUS_REVIEW,
    MAC_VOICE_STATUS_TRY_AGAIN,
    MAC_VOICE_STATUS_SENT,
} mac_voice_status_t;

typedef enum {
    MAC_VOICE_CMD_AUDIO_START = 1,
    MAC_VOICE_CMD_AUDIO_STOP,
    MAC_VOICE_CMD_DELETE,
    MAC_VOICE_CMD_SEND,
    MAC_VOICE_CMD_CLEAR,
} mac_voice_cmd_t;

typedef enum {
    MAC_VOICE_IN_OK_DOWN = 1,
    MAC_VOICE_IN_OK_UP,
    MAC_VOICE_IN_UP_CLICK,
    MAC_VOICE_IN_UP_DOUBLE,
    MAC_VOICE_IN_DOWN_CLICK,
    MAC_VOICE_IN_TRANSCRIPT,
    MAC_VOICE_IN_FAIL,
} mac_voice_input_t;

typedef struct {
    uint8_t count;
    mac_voice_cmd_t items[4];
    uint16_t clear_count;
} mac_voice_actions_t;

typedef struct {
    mac_voice_phase_t phase;
    mac_voice_shape_t shape;
    mac_voice_status_t status;
    bool mic_ok;
    bool link_ready;
    bool shape_dirty;
    char text[MAC_VOICE_TEXT_MAX + 1];
} mac_voice_model_t;

void mac_voice_init(mac_voice_model_t *model, mac_voice_shape_t shape);
void mac_voice_set_mic(mac_voice_model_t *model, bool mic_ok);
void mac_voice_set_link(mac_voice_model_t *model, bool ready, mac_voice_actions_t *out);
void mac_voice_handle(mac_voice_model_t *model, mac_voice_input_t input,
                      const char *transcript, mac_voice_actions_t *out);

const char *mac_voice_status_text(mac_voice_status_t status);
const char *mac_voice_shape_name(mac_voice_shape_t shape);
void mac_voice_preview(const mac_voice_model_t *model, char *out, size_t out_len);
void mac_voice_format_elapsed(uint32_t elapsed_ms, char *out, size_t out_len);
int mac_voice_bar_height(unsigned index, unsigned phase, bool live);
uint16_t mac_voice_spin_degree(uint32_t elapsed_ms);
bool mac_voice_text_is_latin(const char *text);
size_t mac_voice_utf8_count(const char *text);
size_t mac_voice_utf8_drop_last(char *text);
