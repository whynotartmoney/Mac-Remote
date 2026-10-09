#include "mac_voice_logic.h"
#include "mac_voice_adpcm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_talk_delete_send(void)
{
    mac_voice_model_t model;
    mac_voice_actions_t act;
    char preview[64];

    mac_voice_init(&model, MAC_VOICE_SHAPE_ARC);
    mac_voice_set_mic(&model, true);
    mac_voice_set_link(&model, true, &act);
    assert(model.status == MAC_VOICE_STATUS_HOLD);
    assert(strcmp(mac_voice_status_text(model.status), "HOLD OK") == 0);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_LISTENING);
    assert(act.count == 1);
    assert(act.items[0] == MAC_VOICE_CMD_AUDIO_START);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_TRANSCRIBING);
    assert(act.items[0] == MAC_VOICE_CMD_AUDIO_STOP);
    assert(strcmp(mac_voice_status_text(model.status), "WORKING") == 0);

    mac_voice_handle(&model, MAC_VOICE_IN_TRANSCRIPT, "Hello", &act);
    assert(model.phase == MAC_VOICE_PHASE_REVIEW);
    mac_voice_preview(&model, preview, sizeof(preview));
    assert(strcmp(preview, "Hello") == 0);
    assert(strcmp(mac_voice_status_text(model.status), "UP DEL    DOWN SEND") == 0);

    mac_voice_handle(&model, MAC_VOICE_IN_UP_CLICK, NULL, &act);
    assert(strcmp(model.text, "Hell") == 0);
    assert(act.items[0] == MAC_VOICE_CMD_DELETE);

    mac_voice_handle(&model, MAC_VOICE_IN_UP_DOUBLE, NULL, &act);
    assert(model.shape == MAC_VOICE_SHAPE_ARC);
    assert(!model.shape_dirty);
    assert(strcmp(model.text, "Hell") == 0);
    assert(act.count == 0);

    mac_voice_handle(&model, MAC_VOICE_IN_DOWN_CLICK, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_IDLE);
    assert(model.text[0] == '\0');
    assert(act.items[0] == MAC_VOICE_CMD_SEND);
    assert(model.status == MAC_VOICE_STATUS_SENT);
}

static void test_gates_and_replace(void)
{
    mac_voice_model_t model;
    mac_voice_actions_t act;
    char preview[32];

    mac_voice_init(&model, MAC_VOICE_SHAPE_PILL);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_IDLE);
    assert(model.status == MAC_VOICE_STATUS_NO_MIC);
    assert(act.count == 0);

    mac_voice_set_mic(&model, true);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    assert(model.status == MAC_VOICE_STATUS_NO_LINK);

    mac_voice_set_link(&model, true, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_TRANSCRIPT, "Ab", &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_LISTENING);
    assert(strcmp(model.text, "Ab") == 0);
    assert(act.clear_count == 0);
    assert(act.count == 1);
    assert(act.items[0] == MAC_VOICE_CMD_AUDIO_START);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_TRANSCRIPT, "c", &act);
    assert(model.phase == MAC_VOICE_PHASE_REVIEW);
    assert(strcmp(model.text, "Abc") == 0);
    mac_voice_handle(&model, MAC_VOICE_IN_UP_CLICK, NULL, &act);
    assert(strcmp(model.text, "Ab") == 0);
    assert(act.items[0] == MAC_VOICE_CMD_DELETE);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_set_link(&model, false, &act);
    assert(model.phase == MAC_VOICE_PHASE_REVIEW);
    assert(strcmp(model.text, "Ab") == 0);
    assert(act.items[0] == MAC_VOICE_CMD_AUDIO_STOP);
    assert(model.status == MAC_VOICE_STATUS_NO_LINK);

    mac_voice_set_link(&model, true, &act);
    assert(model.status == MAC_VOICE_STATUS_REVIEW);
    mac_voice_handle(&model, MAC_VOICE_IN_DOWN_CLICK, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_IDLE);
    assert(model.text[0] == '\0');
    assert(act.items[0] == MAC_VOICE_CMD_SEND);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_set_link(&model, false, &act);
    assert(model.phase == MAC_VOICE_PHASE_IDLE);
    assert(act.items[0] == MAC_VOICE_CMD_AUDIO_STOP);
    mac_voice_set_link(&model, true, &act);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_TRANSCRIPT, "   ", &act);
    assert(model.status == MAC_VOICE_STATUS_TRY_AGAIN);
    mac_voice_handle(&model, MAC_VOICE_IN_DOWN_CLICK, NULL, &act);
    assert(act.count == 0);

    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_FAIL, NULL, &act);
    assert(model.status == MAC_VOICE_STATUS_TRY_AGAIN);

    mac_voice_set_link(&model, true, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_TRANSCRIPT, "\xE4\xBD\xA0", &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_DOWN, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_OK_UP, NULL, &act);
    mac_voice_handle(&model, MAC_VOICE_IN_FAIL, NULL, &act);
    assert(model.phase == MAC_VOICE_PHASE_REVIEW);
    assert(strcmp(model.text, "\xE4\xBD\xA0") == 0);
    assert(!mac_voice_text_is_latin(model.text));
    mac_voice_preview(&model, preview, sizeof(preview));
    assert(strcmp(preview, "1 CHARS") == 0);
    assert(mac_voice_utf8_drop_last(model.text) == 1);
    assert(model.text[0] == '\0');
}

static void test_shapes_time_and_utf8(void)
{
    mac_voice_model_t model;
    mac_voice_actions_t act;
    char elapsed[16];
    char text[16];

    mac_voice_init(&model, MAC_VOICE_SHAPE_RECT);
    assert(strcmp(mac_voice_shape_name(model.shape), "RECT") == 0);
    for (int i = 0; i < 5; i++) {
        mac_voice_handle(&model, MAC_VOICE_IN_UP_DOUBLE, NULL, &act);
    }
    assert(model.shape == MAC_VOICE_SHAPE_RECT);

    mac_voice_format_elapsed(0, elapsed, sizeof(elapsed));
    assert(strcmp(elapsed, "0") == 0);
    mac_voice_format_elapsed(1084, elapsed, sizeof(elapsed));
    assert(strcmp(elapsed, "1") == 0);
    mac_voice_format_elapsed(62050, elapsed, sizeof(elapsed));
    assert(strcmp(elapsed, "62") == 0);
    assert(mac_voice_spin_degree(0) == 0);
    assert(mac_voice_spin_degree(1200) == 180);
    assert(mac_voice_spin_degree(2400) == 0);

    memcpy(text, "A\xE4\xBD\xA0", 5);
    assert(mac_voice_utf8_count(text) == 2);
    assert(mac_voice_utf8_drop_last(text) == 1);
    assert(strcmp(text, "A") == 0);
    assert(strcmp(mac_voice_shape_name(MAC_VOICE_SHAPE_PILL), "PILL") == 0);
    assert(strcmp(mac_voice_shape_name(MAC_VOICE_SHAPE_TRIANGLE), "TRIANGLE") == 0);
    assert(strcmp(mac_voice_shape_name(MAC_VOICE_SHAPE_ARC), "ARC") == 0);
}

static void test_adpcm_and_frames(void)
{
    mac_voice_adpcm_t enc = { 0, 0 };
    mac_voice_adpcm_t dec = { 0, 0 };
    int16_t pcm[64];
    int16_t back[64];
    uint8_t bytes[32];
    uint8_t frame[48];
    int max_err = 0;

    for (int i = 0; i < 64; i++) pcm[i] = (int16_t)(i * 300);
    assert(mac_voice_adpcm_encode(&enc, pcm, 64, bytes) == 32);
    assert(mac_voice_adpcm_decode(&dec, bytes, 64, back) == 64);
    for (int i = 0; i < 64; i++) {
        int err = (int)pcm[i] - (int)back[i];
        if (err < 0) err = -err;
        if (err > max_err) max_err = err;
    }
    assert(max_err < 2500);

    memset(pcm, 0, sizeof(pcm));
    enc = (mac_voice_adpcm_t){ 0, 0 };
    dec = (mac_voice_adpcm_t){ 0, 0 };
    assert(mac_voice_adpcm_encode(&enc, pcm, 4, bytes) == 2);
    assert(bytes[0] == 0 && bytes[1] == 0);
    mac_voice_adpcm_decode(&dec, bytes, 4, back);
    assert(back[0] == 0 && back[3] == 0);

    assert(mac_voice_pack_start(frame, sizeof(frame)) == 3);
    assert(frame[0] == MAC_VOICE_FRAME_AUDIO_START);
    assert(frame[1] == 0x80 && frame[2] == 0x3E);
    enc = (mac_voice_adpcm_t){ -12, 4 };
    assert(mac_voice_pack_audio(frame, sizeof(frame), 7, enc, 4, bytes, 2) == 10);
    assert(frame[0] == MAC_VOICE_FRAME_AUDIO);
    assert(frame[1] == 7 && frame[2] == 0);
    assert(frame[5] == 4);
    assert(frame[6] == 4 && frame[7] == 0);
    assert(mac_voice_pack_end(frame, sizeof(frame), MAC_VOICE_END_GAP) == 2);
    assert(frame[0] == MAC_VOICE_FRAME_AUDIO_END && frame[1] == MAC_VOICE_END_GAP);
    assert(mac_voice_pack_clear(frame, sizeof(frame), 0x0201) == 3);
    assert(frame[0] == MAC_VOICE_FRAME_CLEAR && frame[1] == 0x01 && frame[2] == 0x02);
    assert(mac_voice_pack_simple(frame, sizeof(frame), MAC_VOICE_FRAME_SEND) == 1);
    assert(frame[0] == MAC_VOICE_FRAME_SEND);
    assert(mac_voice_pack_simple(frame, sizeof(frame), MAC_VOICE_FRAME_DELETE) == 1);
}

int main(void)
{
    test_talk_delete_send();
    test_gates_and_replace();
    test_shapes_time_and_utf8();
    test_adpcm_and_frames();
    puts("mac voice tests: PASS");
    return 0;
}
