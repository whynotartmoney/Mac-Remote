#include "mac_voice_ui.h"

#include "lvgl.h"

#define MV_INK 0x07110F
#define MV_LINE 0x39F2C6
#define MV_TEXT 0xD7FFF4
#define MV_DIM 0x7E9E96

extern const lv_font_t mv_font_16;
extern const lv_font_t mv_font_28;
extern const lv_font_t mv_font_48;

static lv_obj_t *s_status;
static lv_obj_t *s_preview;
static lv_obj_t *s_timer;
static lv_obj_t *s_battery;
static lv_obj_t *s_link;

void mac_voice_ui_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *ring;
    lv_obj_t *title;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(MV_INK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "PC VOICE");
    lv_obj_set_style_text_font(title, &mv_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(MV_LINE), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    s_battery = lv_label_create(scr);
    lv_label_set_text(s_battery, "");
    lv_obj_set_style_text_font(s_battery, &mv_font_16, 0);
    lv_obj_set_style_text_color(s_battery, lv_color_hex(MV_DIM), 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -22, 22);

    ring = lv_obj_create(scr);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ring, 156, 156);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(MV_LINE), 0);
    lv_obj_set_style_pad_all(ring, 0, 0);

    s_timer = lv_label_create(scr);
    lv_label_set_text(s_timer, "0");
    lv_obj_set_style_text_font(s_timer, &mv_font_48, 0);
    lv_obj_set_style_text_color(s_timer, lv_color_hex(MV_TEXT), 0);
    lv_obj_align(s_timer, LV_ALIGN_CENTER, 0, -8);
    lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);

    s_link = lv_label_create(scr);
    lv_label_set_text(s_link, "BLE");
    lv_obj_set_style_text_font(s_link, &mv_font_28, 0);
    lv_obj_set_style_text_color(s_link, lv_color_hex(MV_TEXT), 0);
    lv_obj_set_style_text_letter_space(s_link, 4, 0);
    lv_obj_align(s_link, LV_ALIGN_CENTER, 0, -8);

    s_preview = lv_label_create(scr);
    lv_label_set_text(s_preview, "");
    lv_obj_set_width(s_preview, 196);
    lv_label_set_long_mode(s_preview, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_preview, &mv_font_16, 0);
    lv_obj_set_style_text_color(s_preview, lv_color_hex(MV_TEXT), 0);
    lv_obj_set_style_text_align(s_preview, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_preview, LV_ALIGN_BOTTOM_MID, 0, -72);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, "OPEN PCVOICE");
    lv_obj_set_width(s_status, 200);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status, &mv_font_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(MV_DIM), 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -36);

    lv_screen_load(scr);
}

void mac_voice_ui_apply(const mac_voice_ui_state_t *state)
{
    const char *preview = state->preview ? state->preview : "";
    size_t length = 0;
    while (preview[length] != '\0') length++;
    if (length > 72) preview += length - 72;

    lv_label_set_text(s_status, state->status ? state->status : "");
    lv_label_set_text(s_preview, state->listening ? "" : preview);
    if (state->battery < 0) lv_label_set_text(s_battery, "");
    else lv_label_set_text_fmt(s_battery, "%d", state->battery);

    if (state->listening) {
        lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
    } else if (!state->linked) {
        lv_label_set_text(s_link, "BLE");
        lv_obj_remove_flag(s_link, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_timer, LV_OBJ_FLAG_HIDDEN);
    }
}

void mac_voice_ui_set_seconds(uint32_t seconds)
{
    char text[8];
    if (!s_timer) return;
    mac_voice_format_elapsed(seconds * 1000u, text, sizeof(text));
    lv_label_set_text(s_timer, text);
}
