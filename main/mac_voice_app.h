#pragma once

#include "bsp_button.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    MAC_VOICE_APP_BUTTON = 1,
    MAC_VOICE_APP_NOTIFY,
    MAC_VOICE_APP_MTU,
    MAC_VOICE_APP_WRITE,
};

typedef struct {
    uint8_t kind;
    uint8_t btn;
    uint8_t event;
    uint8_t flags;
    uint16_t mtu;
    uint16_t len;
    char bytes[200];
} mac_voice_app_event_t;

void mac_voice_app_start(void);
bool mac_voice_app_post(const mac_voice_app_event_t *event);
