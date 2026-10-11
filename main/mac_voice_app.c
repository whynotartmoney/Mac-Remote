#include "mac_voice_app.h"

#include "mac_voice_adpcm.h"
#include "mac_voice_link.h"
#include "mac_voice_logic.h"
#include "mac_voice_ui.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdint.h>
#include <string.h>

static const char *TAG = "mac_voice";
static const uint16_t k_windows[][2] = BSP_BTN_MV_TABLE;

static QueueHandle_t s_queue;
static mac_voice_model_t s_model;
static TaskHandle_t s_audio_task;
static SemaphoreHandle_t s_audio_done;
static volatile bool s_audio_cancel;
static volatile bool s_audio_error;
static volatile bool s_gap;
static bool s_capture;
static uint16_t s_seq;
static bool s_notify;
static bool s_hello;
static uint16_t s_mtu;
static int s_soc = -1;
static bool s_battery_ok;
static int64_t s_battery_us;
static int64_t s_origin_us;
static int64_t s_deadline_us;
static int s_release_hits;
static uint32_t s_shown_seconds = UINT32_MAX;
static char s_rx[MAC_VOICE_TEXT_MAX + 1];
static size_t s_rx_len;

static void run_actions(const mac_voice_actions_t *actions);
static void present(void);

static void present(void)
{
    char preview[96];
    mac_voice_ui_state_t view;
    mac_voice_preview(&s_model, preview, sizeof(preview));
    view.status = mac_voice_status_text(s_model.status);
    view.preview = preview;
    view.listening = s_model.phase == MAC_VOICE_PHASE_LISTENING;
    view.linked = s_model.link_ready;
    view.battery = s_soc;
    if (!bsp_lvgl_lock(250)) return;
    mac_voice_ui_apply(&view);
    if (view.listening && s_shown_seconds == UINT32_MAX) {
        s_shown_seconds = 0;
        mac_voice_ui_set_seconds(0);
    }
    bsp_lvgl_unlock();
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        int16_t pcm[320];
        mac_voice_adpcm_t state = { 0, 0 };
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (bsp_audio_wake() != ESP_OK || bsp_audio_set_format(16000, 16, 1) != ESP_OK) {
            s_audio_error = true;
            bsp_audio_sleep();
            xSemaphoreGive(s_audio_done);
            continue;
        }
        while (!s_audio_cancel) {
            mac_voice_adpcm_t snapshot;
            uint8_t nibbles[160];
            uint8_t frame[180];
            size_t packed;
            if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
                s_audio_error = true;
                break;
            }
            if (s_audio_cancel) break;
            snapshot = state;
            mac_voice_adpcm_encode(&state, pcm, 320, nibbles);
            packed = mac_voice_pack_audio(frame, sizeof(frame), s_seq, snapshot, 320, nibbles, 160);
            s_seq++;
            if (!mac_voice_link_send(frame, packed)) s_gap = true;
        }
        bsp_audio_sleep();
        xSemaphoreGive(s_audio_done);
    }
}

static void start_capture(void)
{
    uint8_t frame[8];
    size_t n = mac_voice_pack_start(frame, sizeof(frame));
    s_seq = 0;
    s_gap = false;
    s_audio_error = false;
    s_audio_cancel = false;
    s_origin_us = esp_timer_get_time();
    s_shown_seconds = UINT32_MAX;
    mac_voice_link_set_busy(true);
    if (s_audio_done) {
        while (xSemaphoreTake(s_audio_done, 0) == pdTRUE) { }
    }
    if (!s_audio_task || !mac_voice_link_send(frame, n)) {
        mac_voice_actions_t extra;
        mac_voice_link_set_busy(false);
        mac_voice_handle(&s_model, MAC_VOICE_IN_FAIL, NULL, &extra);
        run_actions(&extra);
        return;
    }
    s_capture = true;
    xTaskNotifyGive(s_audio_task);
}

static void stop_capture(void)
{
    uint8_t frame[4];
    uint8_t flags = 0;
    if (!s_capture) return;
    s_audio_cancel = true;
    if (!s_audio_done ||
        xSemaphoreTake(s_audio_done, pdMS_TO_TICKS(800)) != pdTRUE) {
        ESP_LOGE(TAG, "audio capture stop timed out");
        s_audio_error = true;
    }
    s_capture = false;
    if (s_gap) flags |= MAC_VOICE_END_GAP;
    if (s_audio_error) flags |= MAC_VOICE_END_FAIL;
    mac_voice_link_send(frame, mac_voice_pack_end(frame, sizeof(frame), flags));
    mac_voice_link_set_busy(false);
    s_deadline_us = esp_timer_get_time() + 10000000;
    if (s_audio_error) {
        mac_voice_actions_t extra;
        mac_voice_handle(&s_model, MAC_VOICE_IN_FAIL, NULL, &extra);
        run_actions(&extra);
    }
}

static void run_actions(const mac_voice_actions_t *actions)
{
    if (!actions) return;
    for (uint8_t i = 0; i < actions->count; i++) {
        uint8_t frame[8];
        size_t n = 0;
        switch (actions->items[i]) {
        case MAC_VOICE_CMD_AUDIO_START:
            start_capture();
            break;
        case MAC_VOICE_CMD_AUDIO_STOP:
            stop_capture();
            break;
        case MAC_VOICE_CMD_DELETE:
            n = mac_voice_pack_simple(frame, sizeof(frame), MAC_VOICE_FRAME_DELETE);
            mac_voice_link_send(frame, n);
            break;
        case MAC_VOICE_CMD_SEND:
            n = mac_voice_pack_simple(frame, sizeof(frame), MAC_VOICE_FRAME_SEND);
            mac_voice_link_send(frame, n);
            break;
        case MAC_VOICE_CMD_CLEAR:
            n = mac_voice_pack_clear(frame, sizeof(frame), actions->clear_count);
            mac_voice_link_send(frame, n);
            break;
        }
    }
}

static void recompute_link(void)
{
    mac_voice_actions_t actions;
    bool ready = s_notify && s_hello && s_mtu >= 180;
    mac_voice_set_link(&s_model, ready, &actions);
    run_actions(&actions);
    if (!ready && s_notify && s_model.mic_ok && s_model.phase == MAC_VOICE_PHASE_IDLE &&
        s_model.status != MAC_VOICE_STATUS_SENT &&
        s_model.status != MAC_VOICE_STATUS_TRY_AGAIN) {
        s_model.status = MAC_VOICE_STATUS_WAIT;
    }
    present();
}

static void on_write(const mac_voice_app_event_t *event)
{
    mac_voice_actions_t actions;
    uint8_t op;
    uint8_t flags;
    size_t chunk;
    if (event->len == 0) return;
    op = (uint8_t)event->bytes[0];
    if (op == 0x01 && event->len >= 4 && event->bytes[1] == 'P' &&
        event->bytes[2] == 'V' && event->bytes[3] == 1) {
        s_hello = true;
        recompute_link();
        return;
    }
    if (op == 0x11) {
        mac_voice_handle(&s_model, MAC_VOICE_IN_FAIL, NULL, &actions);
        run_actions(&actions);
        present();
        return;
    }
    if (op != 0x10 || event->len < 2) return;
    flags = (uint8_t)event->bytes[1];
    if (flags & 0x01) s_rx_len = 0;
    chunk = (size_t)event->len - 2;
    if (s_rx_len + chunk > MAC_VOICE_TEXT_MAX) chunk = MAC_VOICE_TEXT_MAX - s_rx_len;
    memcpy(s_rx + s_rx_len, event->bytes + 2, chunk);
    s_rx_len += chunk;
    s_rx[s_rx_len] = '\0';
    if (flags & 0x02) {
        mac_voice_handle(&s_model, MAC_VOICE_IN_TRANSCRIPT, s_rx, &actions);
        s_rx_len = 0;
        present();
    }
}

static void on_button(const mac_voice_app_event_t *event)
{
    mac_voice_actions_t actions;
    mac_voice_input_t input = 0;
    if (event->btn == BSP_BTN_OK && event->event == BSP_BTN_PRESS) input = MAC_VOICE_IN_OK_DOWN;
    else if (event->btn == BSP_BTN_UP && event->event == BSP_BTN_CLICK) input = MAC_VOICE_IN_UP_CLICK;
    else if (event->btn == BSP_BTN_UP && event->event == BSP_BTN_DOUBLE) input = MAC_VOICE_IN_UP_DOUBLE;
    else if (event->btn == BSP_BTN_DOWN && event->event == BSP_BTN_CLICK) input = MAC_VOICE_IN_DOWN_CLICK;
    else return;
    mac_voice_handle(&s_model, input, NULL, &actions);
    run_actions(&actions);
    present();
}

static bool ok_held(int mv)
{
    return mv >= k_windows[BSP_BTN_OK][0] && mv < k_windows[BSP_BTN_OK][1];
}

static void housekeeping(void)
{
    int64_t now = esp_timer_get_time();
    if (s_model.phase == MAC_VOICE_PHASE_LISTENING) {
        int mv = bsp_button_read_mv();
        if (mv >= 0 && !ok_held(mv)) s_release_hits++;
        else s_release_hits = 0;
        if (s_release_hits >= 2) {
            mac_voice_actions_t actions;
            s_release_hits = 0;
            s_shown_seconds = UINT32_MAX;
            mac_voice_handle(&s_model, MAC_VOICE_IN_OK_UP, NULL, &actions);
            run_actions(&actions);
            present();
        } else {
            uint32_t elapsed = 0;
            uint32_t seconds;
            if (now > s_origin_us) elapsed = (uint32_t)((now - s_origin_us) / 1000);
            seconds = elapsed / 1000u;
            if (seconds != s_shown_seconds && bsp_lvgl_lock(40)) {
                s_shown_seconds = seconds;
                mac_voice_ui_set_seconds(seconds);
                bsp_lvgl_unlock();
            }
        }
    } else {
        s_release_hits = 0;
    }

    if (s_model.phase == MAC_VOICE_PHASE_TRANSCRIBING && s_deadline_us != 0 && now > s_deadline_us) {
        mac_voice_actions_t actions;
        s_deadline_us = 0;
        mac_voice_handle(&s_model, MAC_VOICE_IN_FAIL, NULL, &actions);
        run_actions(&actions);
        present();
    }

    if (s_battery_ok && now - s_battery_us > 5000000) {
        int soc = bsp_battery_soc();
        s_battery_us = now;
        if (soc != s_soc) {
            s_soc = soc;
            present();
        }
    }
}

static void app_task(void *arg)
{
    (void)arg;
    for (;;) {
        mac_voice_app_event_t event;
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (event.kind == MAC_VOICE_APP_BUTTON) on_button(&event);
            else if (event.kind == MAC_VOICE_APP_NOTIFY) {
                s_notify = event.flags != 0;
                if (!s_notify) {
                    s_hello = false;
                    s_mtu = 0;
                }
                recompute_link();
            } else if (event.kind == MAC_VOICE_APP_MTU) {
                s_mtu = event.mtu;
                ESP_LOGI(TAG, "ATT MTU %u", (unsigned)s_mtu);
                recompute_link();
            } else if (event.kind == MAC_VOICE_APP_WRITE) {
                on_write(&event);
            }
        }
        housekeeping();
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user)
{
    mac_voice_app_event_t posted = { 0 };
    (void)user;
    posted.kind = MAC_VOICE_APP_BUTTON;
    posted.btn = (uint8_t)btn;
    posted.event = (uint8_t)event;
    mac_voice_app_post(&posted);
}

bool mac_voice_app_post(const mac_voice_app_event_t *event)
{
    if (!s_queue || !event) return false;
    return xQueueSend(s_queue, event, 0) == pdTRUE;
}

void mac_voice_app_start(void)
{
    bool mic_ok;
    ESP_LOGI(TAG, "PC Voice starting");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
    }

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed, MOSI=%d SCLK=%d CS=%d DC=%d BL=%d",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(40);
    s_battery_ok = bsp_battery_init() == ESP_OK;
    mic_ok = bsp_audio_init() == ESP_OK;
    mac_voice_init(&s_model, MAC_VOICE_SHAPE_ARC);
    mac_voice_set_mic(&s_model, false);

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock failed before the first screen");
        return;
    }
    mac_voice_ui_create();
    bsp_lvgl_unlock();
    present();

    s_queue = xQueueCreate(16, sizeof(mac_voice_app_event_t));
    s_audio_done = xSemaphoreCreateBinary();
    if (!s_queue || !s_audio_done) {
        ESP_LOGE(TAG, "queue allocation failed");
        return;
    }
    if (xTaskCreate(app_task, "mac_voice", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "app task failed");
        return;
    }
    if (mic_ok &&
        xTaskCreate(audio_task, "mac_audio", 6144, NULL, 6, &s_audio_task) == pdPASS) {
        if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "audio idle sleep failed");
        mac_voice_set_mic(&s_model, true);
        present();
    } else if (mic_ok) {
        ESP_LOGE(TAG, "audio task failed");
    }

    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button init failed");
    }
    if (mac_voice_link_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE link failed");
    }
}
