#include "mac_voice_link.h"

#include "mac_voice_app.h"

#include "esp_bt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static const char *TAG = "mac_link";
static const char *DEVICE_NAME = "MacVoice";

/* UUID strings, least-significant byte first, matching NimBLE BLE_UUID128_INIT.
 * Service F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E6F
 * Event   F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E70
 * Text    F7A1C3E0-5B24-4D91-8C6E-1A2B3C4D5E71
 */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x6F, 0x5E, 0x4D, 0x3C, 0x2B, 0x1A, 0x6E, 0x8C,
                     0x91, 0x4D, 0x24, 0x5B, 0xE0, 0xC3, 0xA1, 0xF7);
static const ble_uuid128_t s_event_uuid =
    BLE_UUID128_INIT(0x70, 0x5E, 0x4D, 0x3C, 0x2B, 0x1A, 0x6E, 0x8C,
                     0x91, 0x4D, 0x24, 0x5B, 0xE0, 0xC3, 0xA1, 0xF7);
static const ble_uuid128_t s_text_uuid =
    BLE_UUID128_INIT(0x71, 0x5E, 0x4D, 0x3C, 0x2B, 0x1A, 0x6E, 0x8C,
                     0x91, 0x4D, 0x24, 0x5B, 0xE0, 0xC3, 0xA1, 0xF7);

static uint16_t s_event_handle;
static uint16_t s_text_handle;
static uint8_t s_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify;
static SemaphoreHandle_t s_tx;
static int gap_event(struct ble_gap_event *event, void *arg);

static int access_cb(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    mac_voice_app_event_t event;
    uint16_t copied = 0;
    (void)conn_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_text_handle) return 0;
    memset(&event, 0, sizeof(event));
    event.kind = MAC_VOICE_APP_WRITE;
    if (ble_hs_mbuf_to_flat(ctxt->om, event.bytes, sizeof(event.bytes), &copied) != 0) return 0;
    event.len = copied;
    mac_voice_app_post(&event);
    return 0;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_event_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_event_handle,
    },
    {
        .uuid = &s_text_uuid.u,
        .access_cb = access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_text_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_hs_adv_fields response = { 0 };
    struct ble_gap_adv_params params = { 0 };
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    response.uuids128 = &s_svc_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) return rc;

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 0.625 ms units. A zero interval falls back to NimBLE's 30 ms fast
     * advertising and keeps the radio hot while waiting for a Mac. */
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(800);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(1200);
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void connection_params(bool busy, struct ble_gap_upd_params *params)
{
    if (busy) {
        params->itvl_min = 16;
        params->itvl_max = 24;
        params->latency = 0;
        params->supervision_timeout = 200;
    } else {
        params->itvl_min = 80;
        params->itvl_max = 120;
        params->latency = 4;
        params->supervision_timeout = 400;
    }
    params->min_ce_len = 0;
    params->max_ce_len = 0;
}

static void post_notify(bool on)
{
    mac_voice_app_event_t event = { 0 };
    event.kind = MAC_VOICE_APP_NOTIFY;
    event.flags = on ? 1 : 0;
    mac_voice_app_post(&event);
}

static void post_mtu(uint16_t mtu)
{
    mac_voice_app_event_t event = { 0 };
    event.kind = MAC_VOICE_APP_MTU;
    event.mtu = mtu;
    mac_voice_app_post(&event);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            struct ble_gap_upd_params params;
            s_conn = event->connect.conn_handle;
            s_notify = false;
            connection_params(false, &params);
            ble_gap_update_params(s_conn, &params);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify = false;
        post_notify(false);
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_event_handle) {
            s_notify = event->subscribe.cur_notify;
            post_notify(s_notify);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        post_mtu(event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_notify = false;
    post_notify(false);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) rc = advertise();
    if (rc != 0) ESP_LOGE(TAG, "BLE advertise failed: %d", rc);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

void mac_voice_link_set_busy(bool busy)
{
    struct ble_gap_upd_params params;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    connection_params(busy, &params);
    ble_gap_update_params(s_conn, &params);
}

bool mac_voice_link_send(const uint8_t *data, size_t len)
{
    struct os_mbuf *om;
    int rc;
    if (!data || len == 0 || len > 180 || !s_tx) return false;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify) return false;
    if (xSemaphoreTake(s_tx, pdMS_TO_TICKS(40)) != pdTRUE) return false;
    om = ble_hs_mbuf_from_flat(data, len);
    rc = om ? ble_gatts_notify_custom(s_conn, s_event_handle, om) : BLE_HS_ENOMEM;
    xSemaphoreGive(s_tx);
    return rc == 0;
}

esp_err_t mac_voice_link_start(void)
{
    esp_err_t err;
    int rc;
    s_tx = xSemaphoreCreateMutex();
    if (!s_tx) return ESP_ERR_NO_MEM;

    err = nimble_port_init();
    if (err != ESP_OK) return err;
    /* 0 dBm is enough across a desk and avoids the default +9 dBm heat. */
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N0);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N0);
    ble_att_set_preferred_mtu(247);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc == 0) rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "NimBLE GATT setup failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    if (xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE,
                               NULL, configMAX_PRIORITIES - 4, NULL,
                               NIMBLE_CORE) != pdPASS) {
        nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
