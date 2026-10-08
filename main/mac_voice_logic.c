#include "mac_voice_logic.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void actions_clear(mac_voice_actions_t *out)
{
    if (!out) return;
    out->count = 0;
    out->clear_count = 0;
}

static void actions_add(mac_voice_actions_t *out, mac_voice_cmd_t cmd)
{
    if (!out || out->count >= 4) return;
    out->items[out->count++] = cmd;
}

static void refresh_idle_status(mac_voice_model_t *model)
{
    if (model->phase != MAC_VOICE_PHASE_IDLE) return;
    if (model->status == MAC_VOICE_STATUS_SENT ||
        model->status == MAC_VOICE_STATUS_TRY_AGAIN) {
        return;
    }
    if (!model->mic_ok) model->status = MAC_VOICE_STATUS_NO_MIC;
    else if (!model->link_ready) model->status = MAC_VOICE_STATUS_NO_LINK;
    else model->status = MAC_VOICE_STATUS_HOLD;
}

void mac_voice_init(mac_voice_model_t *model, mac_voice_shape_t shape)
{
    memset(model, 0, sizeof(*model));
    if ((unsigned)shape >= MAC_VOICE_SHAPE_COUNT) shape = MAC_VOICE_SHAPE_ARC;
    model->shape = shape;
    model->phase = MAC_VOICE_PHASE_IDLE;
    model->status = MAC_VOICE_STATUS_NO_LINK;
}

void mac_voice_set_mic(mac_voice_model_t *model, bool mic_ok)
{
    model->mic_ok = mic_ok;
    refresh_idle_status(model);
}

void mac_voice_set_link(mac_voice_model_t *model, bool ready, mac_voice_actions_t *out)
{
    actions_clear(out);
    bool was = model->link_ready;
    model->link_ready = ready;
    if (was && !ready && model->phase == MAC_VOICE_PHASE_LISTENING) {
        model->phase = MAC_VOICE_PHASE_IDLE;
        model->status = MAC_VOICE_STATUS_NO_LINK;
        actions_add(out, MAC_VOICE_CMD_AUDIO_STOP);
        return;
    }
    if (was && !ready && model->phase == MAC_VOICE_PHASE_TRANSCRIBING) {
        model->phase = MAC_VOICE_PHASE_IDLE;
        model->status = MAC_VOICE_STATUS_NO_LINK;
        model->text[0] = '\0';
        return;
    }
    if (!ready && model->phase == MAC_VOICE_PHASE_REVIEW) {
        model->status = MAC_VOICE_STATUS_NO_LINK;
        return;
    }
    if (ready && model->phase == MAC_VOICE_PHASE_REVIEW) {
        model->status = MAC_VOICE_STATUS_REVIEW;
        return;
    }
    if (ready && model->phase == MAC_VOICE_PHASE_IDLE &&
        (model->status == MAC_VOICE_STATUS_NO_LINK ||
         model->status == MAC_VOICE_STATUS_WAIT ||
         model->status == MAC_VOICE_STATUS_HOLD ||
         model->status == MAC_VOICE_STATUS_NO_MIC)) {
        refresh_idle_status(model);
    }
}

static void copy_transcript(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0) return;
    while (src[n] != '\0') {
        unsigned char lead = (unsigned char)src[n];
        size_t need = 1;
        if ((lead & 0x80) == 0) need = 1;
        else if ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        else break;
        if (n + need >= cap) break;
        bool ok = true;
        for (size_t k = 1; k < need; k++) {
            if (((unsigned char)src[n + k] & 0xC0) != 0x80) ok = false;
        }
        if (!ok || src[n + need - 1] == '\0') break;
        n += need;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void begin_listen(mac_voice_model_t *model, mac_voice_actions_t *out)
{
    size_t existing = mac_voice_utf8_count(model->text);
    if (existing > 0) {
        if (out) out->clear_count = (uint16_t)existing;
        actions_add(out, MAC_VOICE_CMD_CLEAR);
        model->text[0] = '\0';
    }
    model->phase = MAC_VOICE_PHASE_LISTENING;
    model->status = MAC_VOICE_STATUS_LISTENING;
    actions_add(out, MAC_VOICE_CMD_AUDIO_START);
}

void mac_voice_handle(mac_voice_model_t *model, mac_voice_input_t input,
                      const char *transcript, mac_voice_actions_t *out)
{
    actions_clear(out);

    switch (input) {
    case MAC_VOICE_IN_OK_DOWN:
        if (model->phase == MAC_VOICE_PHASE_TRANSCRIBING) return;
        if (!model->mic_ok) {
            model->status = MAC_VOICE_STATUS_NO_MIC;
            return;
        }
        if (!model->link_ready) {
            model->status = MAC_VOICE_STATUS_NO_LINK;
            return;
        }
        if (model->phase == MAC_VOICE_PHASE_LISTENING) return;
        begin_listen(model, out);
        return;

    case MAC_VOICE_IN_OK_UP:
        if (model->phase != MAC_VOICE_PHASE_LISTENING) return;
        model->phase = MAC_VOICE_PHASE_TRANSCRIBING;
        model->status = MAC_VOICE_STATUS_WORKING;
        actions_add(out, MAC_VOICE_CMD_AUDIO_STOP);
        return;

    case MAC_VOICE_IN_UP_CLICK:
        if (model->phase != MAC_VOICE_PHASE_REVIEW) return;
        if (mac_voice_utf8_drop_last(model->text) == 0) return;
        actions_add(out, MAC_VOICE_CMD_DELETE);
        return;

    case MAC_VOICE_IN_UP_DOUBLE:
        return;

    case MAC_VOICE_IN_DOWN_CLICK:
        if (model->phase != MAC_VOICE_PHASE_REVIEW || model->text[0] == '\0') return;
        if (!model->link_ready) {
            model->status = MAC_VOICE_STATUS_NO_LINK;
            return;
        }
        model->text[0] = '\0';
        model->phase = MAC_VOICE_PHASE_IDLE;
        model->status = MAC_VOICE_STATUS_SENT;
        actions_add(out, MAC_VOICE_CMD_SEND);
        return;

    case MAC_VOICE_IN_TRANSCRIPT:
        if (model->phase != MAC_VOICE_PHASE_TRANSCRIBING) return;
        if (!transcript || transcript[0] == '\0' || strspn(transcript, " \t\r\n") == strlen(transcript)) {
            model->text[0] = '\0';
            model->phase = MAC_VOICE_PHASE_IDLE;
            model->status = MAC_VOICE_STATUS_TRY_AGAIN;
            return;
        }
        copy_transcript(model->text, sizeof(model->text), transcript);
        model->phase = MAC_VOICE_PHASE_REVIEW;
        model->status = model->link_ready ? MAC_VOICE_STATUS_REVIEW : MAC_VOICE_STATUS_NO_LINK;
        return;

    case MAC_VOICE_IN_FAIL:
        if (model->phase != MAC_VOICE_PHASE_TRANSCRIBING &&
            model->phase != MAC_VOICE_PHASE_LISTENING) {
            return;
        }
        bool listening = model->phase == MAC_VOICE_PHASE_LISTENING;
        model->text[0] = '\0';
        model->phase = MAC_VOICE_PHASE_IDLE;
        model->status = MAC_VOICE_STATUS_TRY_AGAIN;
        if (listening) actions_add(out, MAC_VOICE_CMD_AUDIO_STOP);
        return;
    }
}

const char *mac_voice_status_text(mac_voice_status_t status)
{
    switch (status) {
    case MAC_VOICE_STATUS_HOLD: return "HOLD OK";
    case MAC_VOICE_STATUS_NO_LINK: return "OPEN MACVOICE";
    case MAC_VOICE_STATUS_WAIT: return "WAIT";
    case MAC_VOICE_STATUS_NO_MIC: return "NO MIC";
    case MAC_VOICE_STATUS_LISTENING: return "LISTENING";
    case MAC_VOICE_STATUS_WORKING: return "WORKING";
    case MAC_VOICE_STATUS_REVIEW: return "UP DEL    DOWN SEND";
    case MAC_VOICE_STATUS_TRY_AGAIN: return "TRY AGAIN";
    case MAC_VOICE_STATUS_SENT: return "SENT";
    }
    return "HOLD OK";
}

const char *mac_voice_shape_name(mac_voice_shape_t shape)
{
    switch (shape) {
    case MAC_VOICE_SHAPE_ARC: return "ARC";
    case MAC_VOICE_SHAPE_SQUARE: return "SQUARE";
    case MAC_VOICE_SHAPE_TRIANGLE: return "TRIANGLE";
    case MAC_VOICE_SHAPE_RECT: return "RECT";
    case MAC_VOICE_SHAPE_PILL: return "PILL";
    }
    return "ARC";
}

void mac_voice_preview(const mac_voice_model_t *model, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (model->phase != MAC_VOICE_PHASE_REVIEW || model->text[0] == '\0') return;
    if (!mac_voice_text_is_latin(model->text)) {
        snprintf(out, out_len, "%u CHARS", (unsigned)mac_voice_utf8_count(model->text));
        return;
    }
    snprintf(out, out_len, "%s", model->text);
}

void mac_voice_format_elapsed(uint32_t elapsed_ms, char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    elapsed_ms /= 1000u;
    if (elapsed_ms > 9999u) elapsed_ms = 9999u;
    snprintf(out, out_len, "%lu", (unsigned long)elapsed_ms);
}

uint16_t mac_voice_spin_degree(uint32_t elapsed_ms)
{
    return (uint16_t)((elapsed_ms % 2400u) * 360u / 2400u);
}

bool mac_voice_text_is_latin(const char *text)
{
    if (!text) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < 0x20 || *p > 0x7E) return false;
    }
    return true;
}

size_t mac_voice_utf8_count(const char *text)
{
    size_t count = 0;
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xC0) != 0x80) count++;
    }
    return count;
}

size_t mac_voice_utf8_drop_last(char *text)
{
    size_t n;
    size_t i;
    if (!text || text[0] == '\0') return 0;
    n = strlen(text);
    i = n - 1;
    while (i > 0 && ((unsigned char)text[i] & 0xC0) == 0x80) i--;
    text[i] = '\0';
    return 1;
}
