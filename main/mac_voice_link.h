#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t mac_voice_link_start(void);
bool mac_voice_link_send(const uint8_t *data, size_t len);
void mac_voice_link_set_busy(bool busy);
