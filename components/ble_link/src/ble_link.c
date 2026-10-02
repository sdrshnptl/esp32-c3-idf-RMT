/*
 * ble_link — NimBLE peripheral link for the Web Bluetooth dashboard.
 *
 * Named "link", not "transport": NimBLE reserves the `ble_transport_*` symbol prefix for its own
 * HCI transport layer, and reusing it makes the two headers collide.
 *
 * Deliberately protocol-agnostic: it moves framed byte buffers and knows nothing about JSON.
 * The protocol component registers an RX handler and formats the payloads.
 *
 * GATT layout (base UUID a1e9xxxx-6c2b-4f1a-9d3e-b1c2d3e4f5a6):
 *   0000  service (primary)
 *   0001  CMD     write / write-no-rsp   requests, chunked
 *   0002  RSP     notify                 responses and events, chunked
 *   0003  RAW     write-no-rsp / notify  reserved for bulk frame data
 *   0004  STATUS  read / notify          short device info
 *
 * Framing: every chunk is [u16 total][u16 offset] little-endian followed by payload bytes.
 */

#include "ble_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_link";

#define FRAMING_HEADER_LEN 4
#define ATT_NOTIFY_OVERHEAD 3
#define POOL_SLOTS 2
#define SERVICE_UUID_STR "a1e90000-6c2b-4f1a-9d3e-b1c2d3e4f5a6"

/* NimBLE stores 128-bit UUIDs little-endian, so these byte arrays are the canonical UUIDs
 * read right-to-left. */
static const ble_uuid128_t s_uuid_svc =
    BLE_UUID128_INIT(0xa6, 0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0x3e, 0x9d, 0x1a, 0x4f, 0x2b, 0x6c, 0x00,
                     0x00, 0xe9, 0xa1);
static const ble_uuid128_t s_uuid_cmd =
    BLE_UUID128_INIT(0xa6, 0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0x3e, 0x9d, 0x1a, 0x4f, 0x2b, 0x6c, 0x01,
                     0x00, 0xe9, 0xa1);
static const ble_uuid128_t s_uuid_rsp =
    BLE_UUID128_INIT(0xa6, 0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0x3e, 0x9d, 0x1a, 0x4f, 0x2b, 0x6c, 0x02,
                     0x00, 0xe9, 0xa1);
static const ble_uuid128_t s_uuid_raw =
    BLE_UUID128_INIT(0xa6, 0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0x3e, 0x9d, 0x1a, 0x4f, 0x2b, 0x6c, 0x03,
                     0x00, 0xe9, 0xa1);
static const ble_uuid128_t s_uuid_status =
    BLE_UUID128_INIT(0xa6, 0xf5, 0xe4, 0xd3, 0xc2, 0xb1, 0x3e, 0x9d, 0x1a, 0x4f, 0x2b, 0x6c, 0x04,
                     0x00, 0xe9, 0xa1);

typedef struct {
    uint8_t slot;
    uint16_t len;
} rx_msg_t;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_cmd;
static uint16_t s_h_rsp;
static uint16_t s_h_raw;
static uint16_t s_h_status;
static uint8_t s_own_addr_type;
static bool s_initialized;
static bool s_connected;

static char s_device_name[24];

/* Reassembly of inbound chunks. Only touched from the NimBLE host task. */
static uint8_t s_asm[CONFIG_BLE_LINK_RX_MAX];
static size_t s_asm_len;
static size_t s_asm_total;
static uint8_t s_chunk[CONFIG_BLE_LINK_RX_MAX];

/* Hand-off to the link task so the host task never blocks. */
static uint8_t s_pool[POOL_SLOTS][CONFIG_BLE_LINK_RX_MAX];
static volatile bool s_pool_busy[POOL_SLOTS];
static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[4 * sizeof(rx_msg_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_buf;
static StackType_t s_task_stack[CONFIG_BLE_LINK_TASK_STACK];

static ble_link_rx_cb_t s_rx_cb;
static void *s_rx_ctx;
static ble_link_conn_cb_t s_conn_cb;
static void *s_conn_cb_ctx;

/* ---- GATT ---------------------------------------------------------------- */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_uuid_cmd.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_uuid_rsp.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_uuid_raw.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &s_uuid_status.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static void deliver(const uint8_t *data, size_t len)
{
    for (int i = 0; i < POOL_SLOTS; i++) {
        if (s_pool_busy[i]) {
            continue;
        }
        memcpy(s_pool[i], data, len);
        s_pool_busy[i] = true;

        const rx_msg_t msg = { .slot = (uint8_t)i, .len = (uint16_t)len };
        if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
            s_pool_busy[i] = false;
            continue;
        }
        return;
    }
    ESP_LOGW(TAG, "dropping a %u-byte message: link is busy", (unsigned)len);
}

/**
 * @brief Reassemble one inbound chunk and deliver the message once it is complete.
 *
 * Runs in the NimBLE host task, so it only does bounded memcpy work and then queues the
 * message for the link task.
 */
static int handle_write(struct ble_gatt_access_ctxt *ctxt)
{
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < FRAMING_HEADER_LEN) {
        ESP_LOGW(TAG, "chunk too short (%u bytes)", (unsigned)len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_chunk, sizeof(s_chunk), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (copied != len) {
        ESP_LOGW(TAG, "chunk truncated: %u of %u bytes", (unsigned)copied, (unsigned)len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const uint16_t total = (uint16_t)(s_chunk[0] | (s_chunk[1] << 8));
    const uint16_t offset = (uint16_t)(s_chunk[2] | (s_chunk[3] << 8));
    const size_t payload = len - FRAMING_HEADER_LEN;

    if (total == 0 || total > sizeof(s_asm)) {
        ESP_LOGW(TAG, "message size %u out of range (max %u)", (unsigned)total,
                 (unsigned)sizeof(s_asm));
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if ((size_t)offset + payload > total) {
        ESP_LOGW(TAG, "chunk overruns the message (%u+%u > %u)", (unsigned)offset,
                 (unsigned)payload, (unsigned)total);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (offset == 0) {
        s_asm_len = 0;
        s_asm_total = total;
    } else if (offset != s_asm_len || total != s_asm_total) {
        /* Chunks must arrive in order; a restart is a single-chunk message. */
        ESP_LOGW(TAG, "out-of-order chunk (offset %u, expected %u)", (unsigned)offset,
                 (unsigned)s_asm_len);
        s_asm_len = 0;
        s_asm_total = 0;
        return BLE_ATT_ERR_UNLIKELY;
    }

    memcpy(s_asm + offset, s_chunk + FRAMING_HEADER_LEN, payload);
    s_asm_len += payload;

    if (s_asm_len == s_asm_total) {
        deliver(s_asm, s_asm_len);
        s_asm_len = 0;
        s_asm_total = 0;
    }
    return 0;
}

static int handle_read(struct ble_gatt_access_ctxt *ctxt)
{
    char info[96];
    const int n = snprintf(info, sizeof(info), "{\"name\":\"%s\",\"mtu\":%u}", s_device_name,
                           (unsigned)ble_att_mtu(s_conn_handle));
    if (n <= 0 || n >= (int)sizeof(info)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, info, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        return handle_write(ctxt);
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return handle_read(ctxt);
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* ---- advertising --------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields adv = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };
    struct ble_gap_adv_params params = { 0 };
    int rc;

    /* A 128-bit service UUID takes 18 of the 31 advertising bytes, so the name goes in the
     * scan response. Web Bluetooth can still filter on the service because it is advertised. */
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.tx_pwr_lvl_is_present = 1;
    adv.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv.uuids128 = &s_uuid_svc;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields failed: %d", rc);
        return;
    }

    rsp.name = (uint8_t *)s_device_name;
    rsp.name_len = (uint8_t)strlen(s_device_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response fields failed: %d", rc);
        return;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\" with service %s", s_device_name, SERVICE_UUID_STR);
}

/* ---- GAP events ---------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_asm_len = 0;
            s_asm_total = 0;
            ESP_LOGI(TAG, "central connected (handle %d)", s_conn_handle);
            if (s_conn_cb != NULL) {
                s_conn_cb(true, s_conn_cb_ctx);
            }
        } else {
            ESP_LOGW(TAG, "connect failed (status %d), advertising again", event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason %d)", event->disconnect.reason);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_asm_len = 0;
        s_asm_total = 0;
        for (int i = 0; i < POOL_SLOTS; i++) {
            s_pool_busy[i] = false;
        }
        if (s_conn_cb != NULL) {
            s_conn_cb(false, s_conn_cb_ctx);
        }
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated to %u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr %u notify=%d", (unsigned)event->subscribe.attr_handle,
                 (int)event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Convenience over security: drop the stale bond and pair again. */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

/* ---- notifications ------------------------------------------------------- */

static esp_err_t notify_handle(uint16_t val_handle, const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(s_connected, ESP_ERR_INVALID_STATE, TAG, "no central connected");
    ESP_RETURN_ON_FALSE(val_handle != 0, ESP_ERR_INVALID_STATE, TAG, "characteristic not found");
    ESP_RETURN_ON_FALSE(data != NULL || len == 0, ESP_ERR_INVALID_ARG, TAG, "bad payload");

    const uint16_t mtu = ble_att_mtu(s_conn_handle);
    size_t chunk = 20;
    if (mtu > (ATT_NOTIFY_OVERHEAD + FRAMING_HEADER_LEN)) {
        chunk = mtu - ATT_NOTIFY_OVERHEAD - FRAMING_HEADER_LEN;
    }

    size_t offset = 0;
    do {
        const size_t remaining = len - offset;
        const size_t n = (remaining < chunk) ? remaining : chunk;

        uint8_t header[FRAMING_HEADER_LEN] = {
            (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff),
            (uint8_t)(offset & 0xff), (uint8_t)((offset >> 8) & 0xff),
        };

        struct os_mbuf *om = os_msys_get_pkthdr(FRAMING_HEADER_LEN + n, 0);
        ESP_RETURN_ON_FALSE(om != NULL, ESP_ERR_NO_MEM, TAG, "no mbuf available");

        if (os_mbuf_append(om, header, sizeof(header)) != 0 ||
            (n > 0 && os_mbuf_append(om, data + offset, n) != 0)) {
            os_mbuf_free_chain(om);
            ESP_LOGE(TAG, "mbuf append failed");
            return ESP_FAIL;
        }

        const int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
        if (rc != 0) {
            /* ble_gatts_notify_custom consumes the mbuf even on failure. */
            ESP_LOGE(TAG, "notify failed: %d", rc);
            return ESP_FAIL;
        }
        offset += n;
    } while (offset < len);

    return ESP_OK;
}

esp_err_t ble_link_notify(const uint8_t *data, size_t len)
{
    return notify_handle(s_h_rsp, data, len);
}

esp_err_t ble_link_notify_status(const uint8_t *data, size_t len)
{
    return notify_handle(s_h_status, data, len);
}

/* ---- host plumbing ------------------------------------------------------- */

/**
 * @brief Capture the characteristic value handles as the host registers the service.
 *
 * This is the only reliable place to learn them: ble_gatts_add_svcs() merely queues the
 * definitions, and the attribute database does not exist until the host syncs, so calling
 * ble_gatts_find_chr() from ble_link_init() finds nothing.
 */
static void on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR) {
        return;
    }

    const ble_uuid_t *uuid = ctxt->chr.chr_def->uuid;
    const uint16_t handle = ctxt->chr.val_handle;

    if (ble_uuid_cmp(uuid, &s_uuid_cmd.u) == 0) {
        s_h_cmd = handle;
    } else if (ble_uuid_cmp(uuid, &s_uuid_rsp.u) == 0) {
        s_h_rsp = handle;
    } else if (ble_uuid_cmp(uuid, &s_uuid_raw.u) == 0) {
        s_h_raw = handle;
    } else if (ble_uuid_cmp(uuid, &s_uuid_status.u) == 0) {
        s_h_status = handle;
    }
}

static void on_sync(void)
{
    /* Make sure an identity address exists, then find out which one to advertise with. */
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no usable identity address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "could not infer own address type");
        return;
    }
    ESP_LOGI(TAG, "host synced, preferred MTU %u", (unsigned)ble_att_preferred_mtu());
    ESP_LOGI(TAG, "GATT handles: CMD=%u RSP=%u RAW=%u STATUS=%u", (unsigned)s_h_cmd,
             (unsigned)s_h_rsp, (unsigned)s_h_raw, (unsigned)s_h_status);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void link_task(void *arg)
{
    (void)arg;
    rx_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_rx_cb != NULL) {
            s_rx_cb(s_pool[msg.slot], msg.len, s_rx_ctx);
        }
        s_pool_busy[msg.slot] = false;
    }
}

/* ---- public API ---------------------------------------------------------- */

esp_err_t ble_link_init(void)
{
    ESP_RETURN_ON_FALSE(!s_initialized, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    uint8_t mac[6] = { 0 };
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "read BT MAC failed");
    snprintf(s_device_name, sizeof(s_device_name), "%s-%02X%02X", CONFIG_BLE_LINK_NAME_PREFIX,
             mac[4], mac[5]);

    s_queue = xQueueCreateStatic(4, sizeof(rx_msg_t), s_queue_storage, &s_queue_buf);
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "create queue failed");

    if (xTaskCreateStatic(link_task, "ble_link_rx", CONFIG_BLE_LINK_TASK_STACK, NULL,
                          CONFIG_BLE_LINK_TASK_PRIORITY, s_task_stack, &s_task_buf) == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.gatts_register_cb = on_gatt_register;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Open GATT for the MVP: no bonding, no MITM. See the Kconfig for the hardening path. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;

    ESP_RETURN_ON_ERROR(ble_svc_gap_device_name_set(s_device_name), TAG, "set device name failed");
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_RETURN_ON_ERROR(ble_gatts_count_cfg(s_svcs), TAG, "count GATT config failed");
    ESP_RETURN_ON_ERROR(ble_gatts_add_svcs(s_svcs), TAG, "add GATT services failed");
    ESP_RETURN_ON_ERROR(ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU), TAG,
                        "set preferred MTU failed");

    /* Characteristic handles are captured by on_gatt_register() during host sync; there is
     * nothing to look up here. */
    s_initialized = true;
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "ready as \"%s\"", s_device_name);
    return ESP_OK;
}

esp_err_t ble_link_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialised");

    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    int rc = ble_gap_adv_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "adv stop returned %d", rc);
    }
    ESP_RETURN_ON_ERROR(nimble_port_stop(), TAG, "nimble stop failed");

    s_initialized = false;
    s_connected = false;
    return ESP_OK;
}

bool ble_link_is_connected(void)
{
    return s_connected;
}

uint16_t ble_link_mtu(void)
{
    return s_connected ? ble_att_mtu(s_conn_handle) : 23;
}

esp_err_t ble_link_set_rx_handler(ble_link_rx_cb_t cb, void *ctx)
{
    s_rx_cb = cb;
    s_rx_ctx = ctx;
    return ESP_OK;
}

esp_err_t ble_link_set_conn_handler(ble_link_conn_cb_t cb, void *ctx)
{
    s_conn_cb = cb;
    s_conn_cb_ctx = ctx;
    return ESP_OK;
}

const char *ble_link_service_uuid_str(void)
{
    return SERVICE_UUID_STR;
}

const char *ble_link_device_name(void)
{
    return s_device_name;
}
