idf_component_register(
    SRCS "main.c"
         "mac_voice_logic.c"
         "mac_voice_adpcm.c"
         "mac_voice_ui.c"
         "mac_voice_link.c"
         "mac_voice_app.c"
         "mv_font_16.c"
         "mv_font_28.c"
         "mv_font_48.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt nvs_flash esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
