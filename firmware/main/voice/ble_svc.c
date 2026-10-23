/*
 * StopWatch BLE voice M1 — NimBLE peripheral with custom GATT service.
 */
#include "ble_svc.h"

#include <string.h>

#include <esp_log.h>
#include <esp_err.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* 128-bit UUIDs (little-endian byte order as required by NimBLE) */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x5a, 0x7e, 0x9c,
    0x8a, 0x4d, 0x3b, 0x1e, 0x01, 0x00, 0x5a, 0x4f);
static const ble_uuid128_t s_ctrl_uuid = BLE_UUID128_INIT(
    0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x5a, 0x7e, 0x9c,
    0x8a, 0x4d, 0x3b, 0x1e, 0x02, 0x00, 0x5a, 0x4f);
static const ble_uuid128_t s_audio_uuid = BLE_UUID128_INIT(
    0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x5a, 0x7e, 0x9c,
    0x8a, 0x4d, 0x3b, 0x1e, 0x03, 0x00, 0x5a, 0x4f);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(
    0x5f, 0x4e, 0x3d, 0x2c, 0x1b, 0x5a, 0x7e, 0x9c,
    0x8a, 0x4d, 0x3b, 0x1e, 0x04, 0x00, 0x5a, 0x4f);

static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed = false;
static volatile bool s_status_subscribed = false;
static volatile bool s_remote_talk = false;
static volatile uint32_t s_tx_packets = 0;
static volatile uint32_t s_tx_errors = 0;
static volatile uint32_t s_activity = 0;
static volatile uint8_t s_pending_cmd = 0;
static volatile uint8_t s_pending_val = 0;
static uint16_t s_audio_handle = 0;

/* downlink audio ring (PC -> device) */
#define DL_SLOTS 64
static uint8_t s_dl_buf[DL_SLOTS][512];
static volatile size_t s_dl_len[DL_SLOTS];
static volatile uint8_t s_dl_head = 0, s_dl_tail = 0;

bool ble_svc_take_downlink(uint8_t *pkt, size_t max_len, size_t *out_len)
{
    if (s_dl_tail == s_dl_head) return false;
    size_t n = s_dl_len[s_dl_tail];
    if (n > max_len) n = max_len;
    memcpy(pkt, s_dl_buf[s_dl_tail], n);
    s_dl_len[s_dl_tail] = 0;
    s_dl_tail = (uint8_t)((s_dl_tail + 1) % DL_SLOTS);
    if (out_len) *out_len = n;
    return true;
}
static uint16_t s_ctrl_handle = 0;
static uint16_t s_status_handle = 0;

/* Push a one-byte voice event to the phone: 0x10 talk start, 0x11 talk stop,
 * 0x12 playback start, 0x13 playback end. */
/* Audio needs a short interval; while merely idle a long one (with slave
   latency) lets the controller sleep between events. */
void ble_svc_set_audio_mode(bool fast)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_upd_params p;
    if (fast) {
        p = (struct ble_gap_upd_params){.itvl_min = 24, .itvl_max = 40, .latency = 0, .supervision_timeout = 500};
    } else {
        p = (struct ble_gap_upd_params){.itvl_min = 80, .itvl_max = 120, .latency = 4, .supervision_timeout = 500};
    }
    int rc = ble_gap_update_params(s_conn, &p);
    ESP_LOGI(TAG, "conn mode %s rc=%d", fast ? "audio" : "idle", rc);
}

/* Voice events on the AUDIO characteristic (codec 0x7E): the status
   characteristic's notifications are unreliable on some phones. */
void ble_svc_event_send(uint8_t ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    uint8_t pkt[4] = {0, 0, 0x7E, ev};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    if (om) ble_gatts_notify_custom(s_conn, s_audio_handle, om);
}

void ble_svc_status_notify(uint8_t ev)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_status_subscribed) return;
    uint8_t buf[2] = {ev, 0};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (om) ble_gatts_notify_custom(s_conn, s_status_handle, om);
}

void ble_svc_debug_send(const uint8_t *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return;
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 16) return;
    const size_t chunk = (size_t)mtu - 3 - 4; /* att header + our 4-byte header */
    uint16_t seq = 0;
    size_t off = 0;
    while (off < len) {
        size_t n = (len - off) > chunk ? chunk : (len - off);
        uint8_t pkt[260];
        pkt[0] = (uint8_t)(seq & 0xFF);
        pkt[1] = (uint8_t)(seq >> 8);
        pkt[2] = 0x7F;
        pkt[3] = 0;
        memcpy(pkt + 4, data + off, n);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, n + 4);
        if (om) {
            if (ble_gatts_notify_custom(s_conn, s_audio_handle, om) != 0) s_tx_errors++;
        } else {
            s_tx_errors++;
        }
        seq++;
        off += n;
    }
}

uint32_t ble_svc_tx_packets(void) { return s_tx_packets; }
uint32_t ble_svc_tx_errors(void) { return s_tx_errors; }
bool ble_svc_remote_talk(void) { return s_remote_talk; }
uint32_t ble_svc_activity(void) { return s_activity; }

/* Hand a command back for another consumer (the Qiuqiu app only handles its
   own codes; the voice task handles audio/battery/colour ones). */
void ble_svc_requeue_command(uint8_t cmd, uint8_t value)
{
    if (s_pending_cmd == 0) {
        s_pending_cmd = cmd;
        s_pending_val = value;
    }
}

bool ble_svc_take_command(uint8_t *cmd, uint8_t *value)
{
    if (s_pending_cmd == 0) return false;
    if (cmd) *cmd = s_pending_cmd;
    if (value) *value = s_pending_val;
    s_pending_cmd = 0;
    return true;
}
bool ble_svc_ready(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed; }

static int ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "audio access op=%d (not write)", (int)ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t buf[8] = {0};
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (len >= 1) {
        if (buf[0] == SW_BLE_CTRL_TALK_ON) {
            s_remote_talk = true;
            ESP_LOGI(TAG, "remote talk ON");
        } else if (buf[0] == SW_BLE_CTRL_TALK_OFF) {
            s_remote_talk = false;
            ESP_LOGI(TAG, "remote talk OFF");
        } else if (buf[0] == SW_BLE_CTRL_PING) {
            ESP_LOGI(TAG, "ping");
        } else if (buf[0] == 0x10 && len >= 2) {
            s_pending_cmd = 0x10;
            s_pending_val = buf[1];
            ESP_LOGI(TAG, "cmd brightness %u", buf[1]);
        } else if (buf[0] == 0x31 || buf[0] == 0x32 || buf[0] == 0x33 || buf[0] == 0x34 || buf[0] == 0x36 || buf[0] == 0x37) {
            s_pending_cmd = buf[0];
            s_pending_val = 0;
            ESP_LOGI(TAG, "cmd tone test %02x", buf[0]);
        } else if (buf[0] == 0x50 && len >= 2) {
            s_pending_cmd = 0x50;
            s_pending_val = buf[1];
            ESP_LOGI(TAG, "cmd agent face %u", buf[1]);
        } else if (buf[0] == 0x40) {
            s_pending_cmd = 0x40;
            s_pending_val = 0;
            ESP_LOGI(TAG, "cmd battery csv dump");
        } else if (buf[0] == 0x30) {
            s_pending_cmd = 0x30;
            s_pending_val = 0;
            ESP_LOGI(TAG, "cmd framebuffer dump");
        } else if (buf[0] == 0x20 && len >= 2) {
            s_pending_cmd = 0x20;
            s_pending_val = buf[1];
            ESP_LOGI(TAG, "cmd expression %u", buf[1]);
        }
        s_activity++;
    }
    return 0;
}

static int audio_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "audio access op=%d (not write)", (int)ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t tmp[512];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len) != 0) {
        ESP_LOGW(TAG, "audio write too long: om_len=%d", (int)OS_MBUF_PKTLEN(ctxt->om));
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t next = (uint8_t)((s_dl_head + 1) % DL_SLOTS);
    if (next == s_dl_tail) return 0; /* queue full: drop */
    memcpy(s_dl_buf[s_dl_head], tmp, len);
    s_dl_len[s_dl_head] = len;
    s_dl_head = next;
    return 0;
}

static int status_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t st[8] = {0};
    st[0] = (s_conn != BLE_HS_CONN_HANDLE_NONE) ? 1 : 0;
    st[1] = s_subscribed ? 1 : 0;
    st[2] = s_remote_talk ? 1 : 0;
    uint32_t pkts = s_tx_packets;
    st[3] = (uint8_t)(pkts & 0xFF);
    st[4] = (uint8_t)((pkts >> 8) & 0xFF);
    st[5] = (uint8_t)((pkts >> 16) & 0xFF);
    st[6] = (uint8_t)((pkts >> 24) & 0xFF);
    st[7] = (uint8_t)(s_tx_errors > 255 ? 255 : s_tx_errors);

    int rc = os_mbuf_append(ctxt->om, st, sizeof(st));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_ctrl_uuid.u,
                .access_cb = ctrl_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_ctrl_handle,
            },
            {
                .uuid = &s_audio_uuid.u,
                .access_cb = audio_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_audio_handle,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = status_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg);

static esp_timer_handle_t s_adv_slow_timer = NULL;
static bool s_adv_slow_mode = false;

static void advertise(void);

static void advertise_slow(void* arg)
{
    (void)arg;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return;   /* already connected */
    s_adv_slow_mode = true;
    ble_gap_adv_stop();
    advertise();   /* restarts with the 1 s interval */
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    static const char *name = "StopWatch-BLE";
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params advp = {0};
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* Fast for the first 30 s after boot/disconnect so the phone reconnects
       immediately, then fall back to 1 s to cut radio duty cycle. */
    advp.itvl_min = s_adv_slow_mode ? 1600 : 160;   /* 1 s after 30 s, else 100 ms */
    advp.itvl_max = advp.itvl_min;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as %s (%s)", name, s_adv_slow_mode ? "1s" : "100ms");
        if (!s_adv_slow_mode) {
            if (s_adv_slow_timer == NULL) {
                const esp_timer_create_args_t args = {
                    .callback = advertise_slow,
                    .arg = NULL,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "adv_slow",
                };
                esp_timer_create(&args, &s_adv_slow_timer);
            }
            esp_timer_stop(s_adv_slow_timer);
            esp_timer_start_once(s_adv_slow_timer, 30ULL * 1000 * 1000);
        }
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_adv_slow_mode = false;
            s_subscribed = false;
            s_remote_talk = false;
            s_activity++;
            ESP_LOGI(TAG, "connected, conn=%u", s_conn);
            ble_att_set_preferred_mtu(400);
            {
                /* 30-50 ms connection interval: fewer, more reliable packets for
                   audio downlink (16.7 packets/s fits comfortably). */
                struct ble_gap_upd_params p = {
                    .itvl_min = 24,
                    .itvl_max = 40,
                    .latency = 0,
                    .supervision_timeout = 500,
                };
                int rc = ble_gap_update_params(s_conn, &p);
                ESP_LOGI(TAG, "conn param update rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
        }
        advertise();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_remote_talk = false;
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_status_handle) {
            s_status_subscribed = event->subscribe.cur_notify != 0;
            s_activity++;
            ESP_LOGI(TAG, "status notify %s", s_status_subscribed ? "ON" : "OFF");
        } else if (event->subscribe.attr_handle == s_audio_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            s_activity++;
            ESP_LOGI(TAG, "audio notify %s", s_subscribed ? "ON" : "OFF");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu updated: %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_svc_start(void)
{
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("StopWatch-BLE");

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    ble_att_set_preferred_mtu(400);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE service started");
    return ESP_OK;
}

void ble_svc_send_audio(const uint8_t *pkt, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
        return;
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu == 0 || mtu < len + 3) {
        s_tx_errors++;
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, len);
    if (om == NULL) {
        s_tx_errors++;
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn, s_audio_handle, om);
    if (rc != 0) {
        s_tx_errors++;
    } else {
        s_tx_packets++;
    }
}
