#pragma once

#include "mac_voice_logic.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *status;
    const char *preview;
    bool listening;
    bool linked;
    int battery;
} mac_voice_ui_state_t;

void mac_voice_ui_create(void);
void mac_voice_ui_apply(const mac_voice_ui_state_t *state);
void mac_voice_ui_set_seconds(uint32_t seconds);
