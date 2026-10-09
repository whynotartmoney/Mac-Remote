#include "mac_voice_ui.h"

#include "lvgl.h"

#include <stdio.h>

#define MV_INK 0x070B12
#define MV_CYAN 0x2EE7FF
#define MV_LIME 0xC6F54A
#define MV_TEXT 0xF4F8FB
#define MV_DIM 0x8EA4B0
#define MV_CARD 0x0C1420
#define MV_CARD_LINE 0x1C7C96
#define MV_BAR_COUNT 15

extern const lv_font_t mv_font_16;

static const uint32_t k_bar_color[MV_BAR_COUNT] = {
    0x2EE7FF, 0x3AE8F0, 0x4AE8D4, 0x62EEA8, 0x86F478,
    0xA8F85C, 0xC6F54A, 0xD2F644, 0xC6F54A, 0xA8F85C,
    0x86F478, 0x62EEA8, 0x4AE8D4, 0x3AE8F0, 0x2EE7FF
};

static lv_obj_t *s_clock;
static lv_obj_t *s_battery;
static lv_obj_t *s_live_dot;
static lv_obj_t *s_live;
static lv_obj_t *s_title;
static lv_obj_t *s_bars[MV_BAR_COUNT];
static lv_obj_t *s_ring;
static lv_obj_t *s_quote;
static lv_obj_t *s_foot;
static lv_timer_t *s_wave;
static uint8_t s_phase;

static void style_label(lv_obj_t *label, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void paint_bars(bool live)
{
    int i;

    for (i = 0; i < MV_BAR_COUNT; i++) {
        lv_obj_set_height(s_bars[i], mac_voice_bar_height((unsigned)i, s_phase, live));
        lv_obj_set_style_bg_opa(s_bars[i], live ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_obj_align(s_bars[i], LV_ALIGN_BOTTOM_LEFT, 4 + i * 11, 0);
    }
}

static void wave_cb(lv_timer_t *timer)
{
    (void)timer;
    s_phase = (uint8_t)((s_phase + 1u) & 7u);
    paint_bars(true);
}

static const char *headline(const mac_voice_ui_state_t *state)
{
    size_t length = 0;

    if (state->listening) return "LISTENING";
    if (!state->status) return "";
    while (state->status[length] != '\0') length++;
    if (length > 14) return "READY";
    return state->status;
}

void mac_voice_ui_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *wave;
    lv_obj_t *glow;
    lv_obj_t *card;
    lv_obj_t *kicker;
    lv_obj_t *mic_head;
    lv_obj_t *mic_stem;
    lv_obj_t *mic_base;
    int i;

    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(MV_INK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_clock = lv_label_create(scr);
    lv_label_set_text(s_clock, "0");
    style_label(s_clock, &mv_font_16, MV_TEXT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, 18, 16);
    lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);

    s_battery = lv_label_create(scr);
    lv_label_set_text(s_battery, "");
    style_label(s_battery, &mv_font_16, MV_DIM);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -18, 16);

    s_live_dot = lv_obj_create(scr);
    lv_obj_remove_flag(s_live_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_live_dot, 8, 8);
    lv_obj_set_style_radius(s_live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_live_dot, lv_color_hex(MV_LIME), 0);
    lv_obj_set_style_bg_opa(s_live_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_live_dot, 0, 0);
    lv_obj_set_style_pad_all(s_live_dot, 0, 0);
    lv_obj_align(s_live_dot, LV_ALIGN_TOP_RIGHT, -62, 22);
    lv_obj_add_flag(s_live_dot, LV_OBJ_FLAG_HIDDEN);

    s_live = lv_label_create(scr);
    lv_label_set_text(s_live, "LIVE");
    style_label(s_live, &mv_font_16, MV_LIME);
    lv_obj_set_style_text_letter_space(s_live, 1, 0);
    lv_obj_align(s_live, LV_ALIGN_TOP_RIGHT, -18, 16);
    lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "HOLD OK");
    style_label(s_title, &mv_font_16, MV_CYAN);
    lv_obj_set_style_text_letter_space(s_title, 2, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 44);

    wave = lv_obj_create(scr);
    lv_obj_remove_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(wave, 168, 62);
    lv_obj_align(wave, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave, 0, 0);
    lv_obj_set_style_pad_all(wave, 0, 0);
    for (i = 0; i < MV_BAR_COUNT; i++) {
        s_bars[i] = lv_obj_create(wave);
        lv_obj_remove_flag(s_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(s_bars[i], 5);
        lv_obj_set_style_radius(s_bars[i], 3, 0);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_hex(k_bar_color[i]), 0);
        lv_obj_set_style_border_width(s_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_bars[i], 0, 0);
    }
    paint_bars(false);

    glow = lv_obj_create(scr);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(glow, 78, 78);
    lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(MV_CYAN), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_set_style_pad_all(glow, 0, 0);

    s_ring = lv_obj_create(scr);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ring, 58, 58);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(0x0A1218), 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ring, 2, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(MV_CYAN), 0);
    lv_obj_set_style_pad_all(s_ring, 0, 0);

    mic_head = lv_obj_create(s_ring);
    lv_obj_remove_flag(mic_head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mic_head, 12, 16);
    lv_obj_align(mic_head, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(mic_head, 6, 0);
    lv_obj_set_style_bg_opa(mic_head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mic_head, 2, 0);
    lv_obj_set_style_border_color(mic_head, lv_color_hex(MV_CYAN), 0);
    lv_obj_set_style_pad_all(mic_head, 0, 0);

    mic_stem = lv_obj_create(s_ring);
    lv_obj_remove_flag(mic_stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mic_stem, 2, 8);
    lv_obj_align(mic_stem, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(mic_stem, 1, 0);
    lv_obj_set_style_bg_color(mic_stem, lv_color_hex(MV_CYAN), 0);
    lv_obj_set_style_bg_opa(mic_stem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mic_stem, 0, 0);
    lv_obj_set_style_pad_all(mic_stem, 0, 0);

    mic_base = lv_obj_create(s_ring);
    lv_obj_remove_flag(mic_base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mic_base, 16, 2);
    lv_obj_align(mic_base, LV_ALIGN_CENTER, 0, 13);
    lv_obj_set_style_radius(mic_base, 1, 0);
    lv_obj_set_style_bg_color(mic_base, lv_color_hex(MV_CYAN), 0);
    lv_obj_set_style_bg_opa(mic_base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mic_base, 0, 0);
    lv_obj_set_style_pad_all(mic_base, 0, 0);

    card = lv_obj_create(scr);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(card, 208, 100);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(MV_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(MV_CARD_LINE), 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    kicker = lv_label_create(card);
    lv_label_set_text(kicker, "TRANSCRIPT");
    style_label(kicker, &mv_font_16, MV_LIME);
    lv_obj_set_style_text_letter_space(kicker, 1, 0);
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 10, 8);

    s_quote = lv_label_create(card);
    lv_label_set_text(s_quote, "Hold OK to speak.");
    lv_obj_set_width(s_quote, 188);
    lv_label_set_long_mode(s_quote, LV_LABEL_LONG_WRAP);
    style_label(s_quote, &mv_font_16, MV_TEXT);
    lv_obj_align(s_quote, LV_ALIGN_TOP_LEFT, 10, 30);

    s_foot = lv_label_create(card);
    lv_label_set_text(s_foot, "OPEN MACVOICE");
    lv_obj_set_width(s_foot, 188);
    lv_label_set_long_mode(s_foot, LV_LABEL_LONG_DOT);
    style_label(s_foot, &mv_font_16, MV_CYAN);
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_LEFT, 10, -8);

    s_wave = lv_timer_create(wave_cb, 140, NULL);
    lv_timer_pause(s_wave);
    lv_screen_load(scr);
}

void mac_voice_ui_apply(const mac_voice_ui_state_t *state)
{
    const char *preview = state->preview ? state->preview : "";
    const char *status = state->status ? state->status : "";
    char quoted[80];
    size_t length = 0;

    while (preview[length] != '\0') length++;
    if (length > 32) preview += length - 32;

    lv_label_set_text(s_title, headline(state));
    lv_label_set_text(s_foot, status);
    if (preview[0] == '\0') {
        lv_label_set_text(s_quote, state->listening ? "..." : "Hold OK to speak.");
    } else {
        snprintf(quoted, sizeof(quoted), "\"%s\"", preview);
        lv_label_set_text(s_quote, quoted);
    }

    if (state->battery < 0) {
        lv_label_set_text(s_battery, "");
    } else {
        lv_label_set_text_fmt(s_battery, "%d", state->battery);
        lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, state->listening ? -78 : -18, 16);
    }

    if (state->listening) {
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_live, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_live_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
        lv_timer_resume(s_wave);
    } else {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_live, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_live_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_opa(s_ring, LV_OPA_50, 0);
        s_phase = 0;
        paint_bars(false);
        lv_timer_pause(s_wave);
    }
}

void mac_voice_ui_set_seconds(uint32_t seconds)
{
    char text[8];

    if (!s_clock) return;
    mac_voice_format_elapsed(seconds * 1000u, text, sizeof(text));
    lv_label_set_text(s_clock, text);
}
