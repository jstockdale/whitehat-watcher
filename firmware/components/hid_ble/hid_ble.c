/* NimBLE composite HID device: keyboard (report 1) + mouse (report 2) +
 * consumer control (report 3), plus battery service. Just-Works bonding,
 * bonds persisted in NVS.
 *
 * Hosts pair with the device name (default "Watcher-HID", see
 * hid_ble_device_name()). Reports are sent with notify; reads return
 * the last-sent value. */

#include "hid_ble.h"
#include "led.h"
#include "task_psram.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "store/config/ble_store_config.h"

/* Not declared in any public header; ESP-IDF's own NimBLE test app uses the
 * same extern declaration (components/bt/test_apps/memory_release). */
extern void ble_store_config_init(void);

static const char *TAG = "hid_ble";

/* v0.4.6: keepalive task runs on PSRAM stack (16KB freed from internal RAM). */
static task_psram_handle_t *keepalive_psram_ = NULL;

/* Device name: Bluetooth advertising/GAP name, boot "ready" name field,
 * and on-screen title. Runtime-settable via hid_ble_set_device_name()
 * (serial {"t":"set_name"}); persisted in the "dev" NVS namespace under
 * "dev_name" and loaded here at init, before any BLE use. The name is
 * not a secret, so it lives outside the seal namespace and is written
 * directly (rare, single-writer: only set_name touches this key). */
#define HID_BLE_NAME_DEFAULT "Watcher-HID"
#define DEV_NVS_NS "dev"
#define DEV_NVS_NAME_KEY "dev_name"
static char s_device_name[HID_BLE_NAME_MAX + 1] = HID_BLE_NAME_DEFAULT;

const char *hid_ble_device_name(void)
{
    return s_device_name;
}

static bool name_valid(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    size_t n = strlen(name);
    if (n > HID_BLE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

/* v0.4.3: the NVS task lives in the seal component, which
 * PRIV_REQUIRES hid_ble — so hid_ble cannot REQUIRES seal (circular).
 * Declared extern instead, following seal_dev.c's own precedent for
 * the link reply helpers. The link-time dependency is one-directional.
 * ns="" would select the seal namespace; we pass "dev" explicitly.
 * v0.4.7: reads also go through the nvs task. */
extern esp_err_t seal_dev_nvs_write_str_sync(const char *ns, const char *key,
                                             const char *val,
                                             uint32_t timeout_ms);
extern esp_err_t seal_dev_nvs_read_str_sync(const char *ns, const char *key,
                                            char *out, size_t *inout_len,
                                            uint32_t timeout_ms);

static void device_name_load(void)
{
    /* v0.4.7: via the nvs task. The task is the ONLY code that calls
     * nvs_open — never open NVS handles directly. */
    size_t len = sizeof(s_device_name);
    if (seal_dev_nvs_read_str_sync(DEV_NVS_NS, DEV_NVS_NAME_KEY,
                                   s_device_name, &len,
                                   5000) != ESP_OK ||
        !name_valid(s_device_name)) {
        strncpy(s_device_name, HID_BLE_NAME_DEFAULT, sizeof(s_device_name));
    }
}

esp_err_t hid_ble_set_device_name(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* RAM state first: safe from any task (no flash). The advertising
     * payload (rebuilt on every start_advertising) and the on-screen
     * title pick it up on the next advertising cycle / reboot. */
    strncpy(s_device_name, name, sizeof(s_device_name));
    s_device_name[sizeof(s_device_name) - 1] = '\0';
    (void)ble_svc_gap_device_name_set(s_device_name);
    /* Persist via the nvs task (internal-RAM stack). NEVER
     * nvs_set_* / nvs_commit here: this runs on the link worker, whose
     * stack lives in PSRAM — a flash write disables the CPU cache and
     * panics ("Cache disabled but cached memory region accessed").
     * v0.4.1 did the write inline and rebooted on every set_name. */
    esp_err_t err = seal_dev_nvs_write_str_sync(DEV_NVS_NS, DEV_NVS_NAME_KEY,
                                                s_device_name,
                                                10000 /* ms */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device name persist failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "device name set to '%s'", s_device_name);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Report map: keyboard (ID 1), mouse (ID 2), consumer (ID 3)           */
/* ------------------------------------------------------------------ */

static const uint8_t hid_report_map[] = {
    // Keyboard (report ID 1)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute) ; modifiers
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant) ; reserved
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) ; keycodes
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x91, 0x02,        //   Output (Data, Variable, Absolute) ; LED report
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x01,        //   Output (Constant)
    0xC0,              // End Collection
    // Mouse (report ID 2)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (1)
    0x29, 0x03,        //     Usage Maximum (3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data, Variable, Absolute) ; buttons
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x05,        //     Report Size (5)
    0x81, 0x01,        //     Input (Constant)
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0,              // End Collection
    // Consumer control (report ID 3): single 16-bit usage value
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0x3C, 0x02,  //   Usage Maximum (0x23C)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0x3C, 0x02,  //   Logical Maximum (0x23C)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0,              // End Collection
};

/* HID information: bcdHID 1.11, no country code, remote-wake + connectable */
static const uint8_t hid_info[] = { 0x11, 0x01, 0x00, 0x03 };

/* ------------------------------------------------------------------ */
/* GATT database                                                       */
/* ------------------------------------------------------------------ */

enum chr_id {
    CHR_HID_INFO,
    CHR_REPORT_MAP,
    CHR_CTRL_POINT,
    CHR_PROTO_MODE,
    CHR_REP_KBD_IN,
    CHR_REP_KBD_OUT,
    CHR_REP_MOUSE_IN,
    CHR_REP_CONS_IN,
    CHR_BOOT_KBD_IN,
    CHR_BOOT_MOUSE_IN,
    CHR_BATT_LEVEL,
};

/* Report Reference descriptor values: {report ID, report type} */
static const uint8_t rep_ref_kbd_in[2]   = { 1, 1 }; /* input  */
static const uint8_t rep_ref_kbd_out[2]  = { 1, 2 }; /* output */
static const uint8_t rep_ref_mouse_in[2] = { 2, 1 };
static const uint8_t rep_ref_cons_in[2]  = { 3, 1 };

/* Live state */
static uint16_t conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
static bool advertising_ = false;

/* Firmware-side BLE HID keepalive: when keepalive_interval_s_ > 0,
 * keepalive_task_fn() sends a zero-movement mouse report every interval so
 * the host keeps the BLE HID connection alive without the cursor moving.
 * Always runs on the configured schedule (default 60s); set to 0 to
 * disable. No user-facing indicator. */
static volatile int keepalive_interval_s_ = 60;
static void keepalive_task_fn(void *arg);

/* NOTE: Host OS jiggler moved to the console page (v0.4.29+). The page
 * sends periodic mouse_move commands; firmware no longer implements it. */
/* Declared later in this file (glide section);
 * never fires mid-glide. */
static volatile bool glide_active_;

/* Held-button mask, ORed into every mouse report so a drag keeps the
 * button down across move/scroll reports (the host treats buttons=0 as
 * a release). */
static uint8_t mouse_buttons_ = 0;
static uint8_t own_addr_type_;
static hid_ble_event_cb_t event_cb_ = NULL;
static bool hid_drop_warned_ = false;  /* once-per-connection */

/* NCP-1 revB pairing/consent state. Generation-guarded: prompts and
 * teardowns bump pair_gen_; a user answer carrying a stale gen is dropped.
 * Host task writes; UI answers via the public calls. */
static volatile uint32_t pair_gen_ = 0;
static uint16_t pair_conn_ = BLE_HS_CONN_HANDLE_NONE;
static volatile bool pair_waiting_ = false;  /* NC: inject_io owed */
/* [revB] consent gate */
static volatile bool conn_approved_ = false;   /* HID TX permitted */
static volatile bool consent_pending_ = false;
static bool had_bond_at_connect_ = false;
static ble_addr_t peer_id_addr_;  /* cached at CONNECT: needed for bond
                                   * delete after the conn object is gone */
static hid_ble_pair_ui_cb_t pair_ui_cb_ = NULL;
static uint8_t protocol_mode_ = 1; /* 1 = report protocol */
static uint8_t kbd_leds_ = 0;
static uint8_t battery_level_ = 95;

/* Last-sent reports, returned on host reads */
static uint8_t last_kbd_[8];
static uint8_t last_mouse_[4];
static uint8_t last_cons_[2];

/* Characteristic value handles (filled by the stack) */
static uint16_t h_hid_info, h_report_map, h_ctrl_pt, h_proto_mode;
static uint16_t h_rep_kbd_in, h_rep_kbd_out, h_rep_mouse_in, h_rep_cons_in;
static uint16_t h_boot_kbd_in, h_boot_mouse_in, h_batt_level;

/* Notify subscription flags, one per notifiable characteristic */
static uint16_t sub_kbd_in, sub_mouse_in, sub_cons_in, sub_batt;
static uint16_t sub_boot_kbd, sub_boot_mouse;

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static int dsc_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint8_t *ref = (const uint8_t *)arg;
    uint16_t uuid16 = ble_uuid_u16(ctxt->dsc->uuid);

    if (uuid16 == 0x2908) { /* Report Reference */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
            return os_mbuf_append(ctxt->om, ref, 2);
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (uuid16 == 0x2902) { /* CCCD */
        uint16_t *sub = (uint16_t *)arg;
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
            return os_mbuf_append(ctxt->om, sub, 2);
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
            uint16_t val = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, &val, sizeof(val), NULL);
            if (rc == 0) {
                *sub = val;
            }
            return rc;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define REP_IN_DSCS(ref, sub) \
    (const struct ble_gatt_dsc_def[]) { \
        { .uuid = BLE_UUID16_DECLARE(0x2908), \
          .att_flags = BLE_ATT_F_READ, \
          .access_cb = dsc_access_cb, .arg = (void *)(ref) }, \
        { .uuid = BLE_UUID16_DECLARE(0x2902), \
          .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE, \
          .access_cb = dsc_access_cb, .arg = (void *)(sub) }, \
        { 0 }, \
    }

#define REP_REF_DSC(ref) \
    (const struct ble_gatt_dsc_def[]) { \
        { .uuid = BLE_UUID16_DECLARE(0x2908), \
          .att_flags = BLE_ATT_F_READ, \
          .access_cb = dsc_access_cb, .arg = (void *)(ref) }, \
        { 0 }, \
    }

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    enum chr_id id = (enum chr_id)(intptr_t)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (id) {
        case CHR_HID_INFO:
            return os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info));
        case CHR_REPORT_MAP:
            return os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map));
        case CHR_PROTO_MODE:
            return os_mbuf_append(ctxt->om, &protocol_mode_, 1);
        case CHR_REP_KBD_IN:
            return os_mbuf_append(ctxt->om, last_kbd_, sizeof(last_kbd_));
        case CHR_REP_KBD_OUT:
            return os_mbuf_append(ctxt->om, &kbd_leds_, 1);
        case CHR_REP_MOUSE_IN:
            return os_mbuf_append(ctxt->om, last_mouse_, sizeof(last_mouse_));
        case CHR_REP_CONS_IN:
            return os_mbuf_append(ctxt->om, last_cons_, sizeof(last_cons_));
        case CHR_BATT_LEVEL:
            return os_mbuf_append(ctxt->om, &battery_level_, 1);
        case CHR_BOOT_KBD_IN:
            return os_mbuf_append(ctxt->om, last_kbd_, sizeof(last_kbd_));
        case CHR_BOOT_MOUSE_IN:
            return os_mbuf_append(ctxt->om, last_mouse_, sizeof(last_mouse_));
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (id == CHR_PROTO_MODE) {
            uint8_t v = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, &v, 1, NULL) == 0) {
                protocol_mode_ = v;
            }
            return 0;
        }
        if (id == CHR_REP_KBD_OUT) {
            uint8_t v = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, &v, 1, NULL) == 0) {
                kbd_leds_ = v;
            }
            return 0;
        }
        if (id == CHR_CTRL_POINT) {
            return 0; /* suspend/exit-suspend: nothing to do */
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812), /* HIDS */
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), /* HID Information */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_HID_INFO,
              .flags = BLE_GATT_CHR_F_READ, .val_handle = &h_hid_info },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), /* Report Map */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_REPORT_MAP,
              .flags = BLE_GATT_CHR_F_READ, .val_handle = &h_report_map },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), /* HID Control Point */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_CTRL_POINT,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .val_handle = &h_ctrl_pt },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), /* Protocol Mode */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_PROTO_MODE,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .val_handle = &h_proto_mode },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), /* Report: keyboard input */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_REP_KBD_IN,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_rep_kbd_in,
              .descriptors = REP_IN_DSCS(rep_ref_kbd_in, &sub_kbd_in) },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), /* Report: keyboard output */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_REP_KBD_OUT,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                       BLE_GATT_CHR_F_WRITE_NO_RSP,
              .val_handle = &h_rep_kbd_out,
              .descriptors = REP_REF_DSC(rep_ref_kbd_out) },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), /* Report: mouse input */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_REP_MOUSE_IN,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_rep_mouse_in,
              .descriptors = REP_IN_DSCS(rep_ref_mouse_in, &sub_mouse_in) },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), /* Report: consumer input */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_REP_CONS_IN,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_rep_cons_in,
              .descriptors = REP_IN_DSCS(rep_ref_cons_in, &sub_cons_in) },
            { .uuid = BLE_UUID16_DECLARE(0x2A22), /* Boot Keyboard Input */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_BOOT_KBD_IN,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_boot_kbd_in,
              .descriptors = (const struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2902),
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    .access_cb = dsc_access_cb, .arg = (void *)&sub_boot_kbd },
                  { 0 },
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A33), /* Boot Mouse Input */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_BOOT_MOUSE_IN,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_boot_mouse_in,
              .descriptors = (const struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2902),
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    .access_cb = dsc_access_cb, .arg = (void *)&sub_boot_mouse },
                  { 0 },
              } },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F), /* Battery */
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A19), /* Battery Level */
              .access_cb = gatt_access_cb, .arg = (void *)CHR_BATT_LEVEL,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &h_batt_level,
              .descriptors = (const struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2902),
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    .access_cb = dsc_access_cb, .arg = (void *)&sub_batt },
                  { 0 },
              } },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ */
/* GAP                                                                 */
/* ------------------------------------------------------------------ */

static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = 0x03C1; /* keyboard */
    fields.appearance_is_present = 1;
    /* v0.4.18.74: Fix type bug — fields.uuids16 is const ble_uuid16_t*,
     * not uint16_t*. The old code caused rotating garbage UUIDs
     * (0x221B/0xCDD8) in the adv payload instead of 0x1812. */
    static const ble_uuid16_t adv_uuids16[] = { BLE_UUID16_INIT(0x1812) };
    fields.uuids16 = adv_uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type_, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        advertising_ = false;
    } else {
        ESP_LOGI(TAG, "advertising as '%s'", s_device_name);
        advertising_ = true;
        led_set_state(LED_STATE_BLE_ADVERTISING);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle_ = event->connect.conn_handle;
            advertising_ = false; /* controller stops advertising on connect */
            ESP_LOGI(TAG, "connected, handle=%d", conn_handle_);
            /* NCP-1 revB: Record whether a bond already exists for this peer
             * BEFORE any SMP runs. This is the §6 discriminator between
             * "fresh unauthenticated pairing" (prompt) and "reconnect of a
             * previously approved bond" (silent). */
            struct ble_gap_conn_desc d;
            had_bond_at_connect_ = false;
            conn_approved_ = false;
            consent_pending_ = false;
            hid_drop_warned_ = false;
            if (ble_gap_conn_find(event->connect.conn_handle, &d) == 0) {
                peer_id_addr_ = d.peer_id_addr;
                struct ble_store_key_sec k;
                struct ble_store_value_sec v;
                memset(&k, 0, sizeof(k));
                k.peer_addr = d.peer_id_addr;
                had_bond_at_connect_ = (ble_store_read_peer_sec(&k, &v) == 0);
            }
            /* v0.4.18.73: Do NOT touch LEDs here. led_clear_state() uses
             * portMAX_DELAY which blocks the NimBLE host task, causing SMP
             * Pairing Requests to go unanswered → 30s timeout → disconnect.
             * LED updates happen in ble_evt_task (async). */
            /* Ask the peer to pair/bond (Just Works). */
            ble_gap_security_initiate(conn_handle_);
            if (event_cb_) {
                event_cb_(true);
            }
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected: reason=%d", event->disconnect.reason);
        conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
        sub_kbd_in = sub_mouse_in = sub_cons_in = sub_batt = 0;
        sub_boot_kbd = sub_boot_mouse = 0;
        /* A dwell-to-drag (or any held button) must not survive the
         * connection: without this the peer sees a stuck button after
         * re-pairing. No report to send -- there is no peer right now. */
        mouse_buttons_ = 0;
        /* NCP-1 revB: Dismiss any in-flight pairing prompt on disconnect.
         * If consent was pending, the bond was written at key distribution
         * but never approved — delete it or the §6 invariant breaks. */
        if (consent_pending_) {
            consent_pending_ = false;
            ble_store_util_delete_peer(&peer_id_addr_);
            ESP_LOGW(TAG, "consent never given: unapproved bond deleted");
        }
        conn_approved_ = false;
        pair_waiting_ = false;
        pair_gen_++;
        if (pair_ui_cb_) {
            pair_ui_cb_(pair_gen_, HID_BLE_PAIR_UI_DISMISS, 0);
        }
        if (event_cb_) {
            event_cb_(false);
        }
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete, restarting");
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr=%d subscribed=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* v0.4.18.74: Root cause of the 30s BLE flap. When a peer we have
         * a bond for (in NVS) sends a fresh Pairing Request (its side lost
         * the bond), NimBLE raises this event and acts on our return value.
         * Return 0 (the old default) = silently ignore the Pairing Request
         * → BlueZ waits 30s (SMP timeout) → disconnect 0x13 → reconnect →
         * infinite flap. The fix: delete our stale bond and return RETRY
         * so the new pairing proceeds. MUST delete before RETRY: NimBLE
         * re-raises this event until the bond is gone; RETRY without delete
         * is an infinite loop in the host task. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGW(TAG, "repeat pairing: stale bond deleted, retrying");
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        /* NCP-1 revB: Numeric Comparison pairing ceremony. This runs in the
         * NimBLE host task — record state and queue to UI, never block. */
        const struct ble_gap_passkey_params *p = &event->passkey.params;
        switch (p->action) {
        case BLE_SM_IOACT_NUMCMP:
            /* NCP-1 revC defensive: unreachable with DISPLAY_ONLY (resp table
             * row 0 has no NUMCMP). Kept functional so an io_cap change can't
             * reopen the silent-default hole that caused the 30 s flap. */
            pair_conn_    = event->passkey.conn_handle;
            pair_gen_++;
            pair_waiting_ = true;
            ESP_LOGW(TAG, "pairing: unexpected NUMCMP with DISPLAY_ONLY io_cap");
            if (pair_ui_cb_) {
                /* Queue-only: the UI callback must not block. */
                pair_ui_cb_(pair_gen_, HID_BLE_PAIR_UI_NUMCMP, p->numcmp);
            } else {
                /* No UI registered: secure default = reject. */
                struct ble_sm_io io = {
                    .action = BLE_SM_IOACT_NUMCMP,
                    .numcmp_accept = 0,
                };
                ble_sm_inject_io(event->passkey.conn_handle, &io);
                ESP_LOGW(TAG, "pairing rejected: no pairing UI available");
            }
            break;
        case BLE_SM_IOACT_DISP: {
            /* NCP-1 revC PRIMARY CEREMONY. Normative (§0): fresh random
             * passkey on every event — never cached, never reused, never
             * logged or mirrored. A failed attempt leaks the passkey to an
             * active attacker; freshness keeps every attempt at 1e-6. */
            uint32_t pk = esp_random() % 1000000u;
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = pk,
            };
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) {
                ESP_LOGW(TAG, "passkey inject failed: %d", rc);
                break;
            }
            pair_conn_ = event->passkey.conn_handle;
            pair_gen_++;
            pair_waiting_ = false;              /* nothing to answer locally */
            ESP_LOGI(TAG, "pairing: passkey displayed (gen=%u)",
                     (unsigned)pair_gen_);      /* never log pk itself */
            if (pair_ui_cb_) {
                pair_ui_cb_(pair_gen_, HID_BLE_PAIR_UI_PASSKEY, pk);
            } else {
                /* Headless: a passkey nobody can read is unpairable anyway;
                 * terminate rather than leave a 30 s mystery window. */
                ble_gap_terminate(event->passkey.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
            break;
        }
        case BLE_SM_IOACT_INPUT: /* unreachable with DisplayYesNo in SC */
        case BLE_SM_IOACT_OOB:   /* OOB never configured */
        default:
            ESP_LOGW(TAG, "pairing: unsupported io action %d — terminating",
                     p->action);
            ble_gap_terminate(event->passkey.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        /* NCP-1 revB: Consent gate. Pairing just completed (or failed).
         * Determine if the bond is authenticated (MITM ceremony happened),
         * a previously-approved bond, or a fresh unauthenticated pairing
         * that needs user consent. */
        struct ble_gap_conn_desc d;
        bool have = (ble_gap_conn_find(event->enc_change.conn_handle, &d) == 0);
        pair_waiting_ = false;
        pair_gen_++;
        if (pair_ui_cb_) {
            pair_ui_cb_(pair_gen_, HID_BLE_PAIR_UI_DISMISS, 0);
        }

        if (event->enc_change.status != 0 || !have || !d.sec_state.encrypted) {
            ESP_LOGW(TAG, "encryption failed (status=%d)",
                     event->enc_change.status);
            return 0;
        }
        if (d.sec_state.authenticated) {
            /* NC or passkey ceremony happened: authorized by construction. */
            conn_approved_ = true;
            ESP_LOGI(TAG, "encrypted, authenticated (MITM-protected)");
        } else if (had_bond_at_connect_) {
            /* Unauthenticated bond predating this connection. Invariant:
             * such a bond only survives if the user approved it at creation,
             * so reconnects are silent. */
            conn_approved_ = true;
            ESP_LOGI(TAG, "encrypted, unauthenticated (previously approved bond)");
        } else {
            /* Fresh Just Works pairing: park HID and ask the user. */
            conn_approved_ = false;
            consent_pending_ = true;
            pair_conn_ = event->enc_change.conn_handle;
            pair_gen_++;
            ESP_LOGW(TAG, "encrypted, UNAUTHENTICATED fresh pairing: awaiting consent");
            if (pair_ui_cb_) {
                pair_ui_cb_(pair_gen_, HID_BLE_PAIR_UI_CONSENT, 0);
            } else {
                /* Headless: secure default — no screen, no consent, no bond. */
                ble_store_util_delete_peer(&peer_id_addr_);
                ble_gap_terminate(event->enc_change.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;
    }
    default:
        /* v0.4.18.74: Never silently ignore unknown GAP events again.
         * This default cost days of debugging on the REPEAT_PAIRING bug. */
        ESP_LOGI(TAG, "gap event %d (unhandled)", event->type);
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &own_addr_type_);
    if (rc != 0) {
        ESP_LOGE(TAG, "no address available");
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/* Report sending                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t notify_report(uint16_t chr_h, const uint16_t *sub,
                               const void *data, size_t len)
{
    if (conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    /* NCP-1 revB: HID TX gate. Until the connection is approved (NC ceremony
     * completed, or consent granted for JW), drop reports. The peer holds an
     * encrypted link that carries nothing. */
    if (!conn_approved_) {
        if (!hid_drop_warned_) {
            ESP_LOGW(TAG, "HID report dropped: connection not approved");
            hid_drop_warned_ = true;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if ((*sub & 0x0001) == 0) {
        /* Host hasn't enabled notifications on the CCCD; drop quietly. */
        return ESP_ERR_INVALID_STATE;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gatts_notify_custom(conn_handle_, chr_h, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_keyboard(uint8_t mods, const uint8_t keys[6])
{
    uint8_t rpt[8] = { mods, 0, keys[0], keys[1], keys[2],
                       keys[3], keys[4], keys[5] };
    memcpy(last_kbd_, rpt, sizeof(rpt));
    return notify_report(h_rep_kbd_in, &sub_kbd_in, rpt, sizeof(rpt));
}

static esp_err_t send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    uint8_t rpt[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
    memcpy(last_mouse_, rpt, sizeof(rpt));
    return notify_report(h_rep_mouse_in, &sub_mouse_in, rpt, sizeof(rpt));
}

static esp_err_t send_consumer(uint16_t usage)
{
    uint8_t rpt[2] = { usage & 0xFF, (usage >> 8) & 0xFF };
    memcpy(last_cons_, rpt, sizeof(rpt));
    return notify_report(h_rep_cons_in, &sub_cons_in, rpt, sizeof(rpt));
}

/* ------------------------------------------------------------------ */
/* Keycode tables (US layout)                                          */
/* ------------------------------------------------------------------ */

static bool ascii_to_hid(char c, uint8_t *usage, bool *shift)
{
    *shift = false;
    if (c >= 'a' && c <= 'z') { *usage = 0x04 + (uint8_t)(c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (uint8_t)(c - 'A'); *shift = true; return true; }
    if (c >= '1' && c <= '9') { *usage = 0x1E + (uint8_t)(c - '1'); return true; }
    switch (c) {
    case ' ':  *usage = 0x2C; return true;
    case '0':  *usage = 0x27; return true;
    case '\n': *usage = 0x28; return true;  /* Enter */
    case '\t': *usage = 0x2B; return true; /* Tab */
    case '\r': return false;                /* skip CR in CRLF */
    case '!':  *usage = 0x1E; *shift = true; return true;
    case '@':  *usage = 0x1F; *shift = true; return true;
    case '#':  *usage = 0x20; *shift = true; return true;
    case '$':  *usage = 0x21; *shift = true; return true;
    case '%':  *usage = 0x22; *shift = true; return true;
    case '^':  *usage = 0x23; *shift = true; return true;
    case '&':  *usage = 0x24; *shift = true; return true;
    case '*':  *usage = 0x25; *shift = true; return true;
    case '(':  *usage = 0x26; *shift = true; return true;
    case ')':  *usage = 0x27; *shift = true; return true;
    case '-':  *usage = 0x2D; return true;
    case '_':  *usage = 0x2D; *shift = true; return true;
    case '=':  *usage = 0x2E; return true;
    case '+':  *usage = 0x2E; *shift = true; return true;
    case '[':  *usage = 0x2F; return true;
    case '{':  *usage = 0x2F; *shift = true; return true;
    case ']':  *usage = 0x30; return true;
    case '}':  *usage = 0x30; *shift = true; return true;
    case '\\': *usage = 0x31; return true;
    case '|':  *usage = 0x31; *shift = true; return true;
    case ';':  *usage = 0x33; return true;
    case ':':  *usage = 0x33; *shift = true; return true;
    case '\'': *usage = 0x34; return true;
    case '"':  *usage = 0x34; *shift = true; return true;
    case '`':  *usage = 0x35; return true;
    case '~':  *usage = 0x35; *shift = true; return true;
    case ',':  *usage = 0x36; return true;
    case '<':  *usage = 0x36; *shift = true; return true;
    case '.':  *usage = 0x37; return true;
    case '>':  *usage = 0x37; *shift = true; return true;
    case '/':  *usage = 0x38; return true;
    case '?':  *usage = 0x38; *shift = true; return true;
    default:   return false;
    }
}

static const struct { const char *name; uint8_t usage; } named_keys[] = {
    { "enter", 0x28 }, { "esc", 0x29 }, { "backspace", 0x2A },
    { "tab", 0x2B }, { "space", 0x2C },
    { "f1", 0x3A }, { "f2", 0x3B }, { "f3", 0x3C }, { "f4", 0x3D },
    { "f5", 0x3E }, { "f6", 0x3F }, { "f7", 0x40 }, { "f8", 0x41 },
    { "f9", 0x42 }, { "f10", 0x43 }, { "f11", 0x44 }, { "f12", 0x45 },
    { "insert", 0x49 }, { "home", 0x4A }, { "pgup", 0x4B },
    { "delete", 0x4C }, { "end", 0x4D }, { "pgdn", 0x4E },
    { "right", 0x4F }, { "left", 0x50 }, { "down", 0x51 }, { "up", 0x52 },
    { NULL, 0 },
};

static uint8_t parse_mods(const char *mod)
{
    uint8_t mods = 0;
    if (!mod || !*mod) {
        return 0;
    }
    char buf[32];
    strncpy(buf, mod, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        if (strcmp(tok, "ctrl") == 0 || strcmp(tok, "control") == 0) mods |= 0x01;
        else if (strcmp(tok, "shift") == 0) mods |= 0x02;
        else if (strcmp(tok, "alt") == 0 || strcmp(tok, "option") == 0) mods |= 0x04;
        else if (strcmp(tok, "gui") == 0 || strcmp(tok, "cmd") == 0 ||
                 strcmp(tok, "win") == 0 || strcmp(tok, "super") == 0) mods |= 0x08;
    }
    return mods;
}

static bool named_key_usage(const char *code, uint8_t *usage)
{
    for (int i = 0; named_keys[i].name; i++) {
        if (strcmp(code, named_keys[i].name) == 0) {
            *usage = named_keys[i].usage;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t hid_ble_init(void)
{
    int rc;

    device_name_load(); /* NVS name (if any) before any BLE use */

    rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* NCP-1 revC: Passkey Entry with Watcher as display (primary ceremony).
     * DISPLAY_ONLY + sm_mitm=1 → resp table row 0: KeyboardDisplay hosts
     * get DISP (we show 6 digits, user types on host). */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "gap name set failed: %d", rc);
        return ESP_FAIL;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Persist bonds/CCCDs in NVS so re-pairing isn't needed after reboot.
     * ble_store_config_init() MUST be called: it restores persisted bonds
     * from NVS into RAM via ble_nvs_restore_sec_keys(). The ESP-IDF NimBLE
     * port does not call it automatically (Mynewt sysinit isn't run), and
     * without it pairing succeeds (writes go straight to NVS) but bonds
     * are never loaded at boot, so reconnects fail with BLE_HS_EAUTHEN. */
    ble_store_config_init();
    ble_hs_cfg.store_read_cb = ble_store_config_read;
    ble_hs_cfg.store_write_cb = ble_store_config_write;
    ble_hs_cfg.store_delete_cb = ble_store_config_delete;
    /* v0.4.18.74: Handle full bond store by deleting oldest peer (standard
     * NimBLE pattern). Without this, a full store fails key-exchange writes
     * mid-pairing — a different way to not finish pairing. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(host_task);

    /* Firmware-side BLE keepalive task (always runs on its interval).
     * v0.4.6: PSRAM stack (was 16KB internal). */
    keepalive_psram_ = task_psram_create(keepalive_task_fn, "hid_keepalive",
                                        4096 * 4, NULL, 3, tskNO_AFFINITY);
    if (!keepalive_psram_) {
        ESP_LOGE(TAG, "keepalive task create failed");
    }
    /* NOTE: Host OS jiggler is implemented in the console page via
     * periodic mouse_move commands, not in firmware. See console v0.4.29+. */

    ESP_LOGI(TAG, "HID device ready");
    return ESP_OK;
}

bool hid_ble_connected(void)
{
    return conn_handle_ != BLE_HS_CONN_HANDLE_NONE;
}

/* Background BLE keepalive: once a second, count up; every interval
 * while enabled and connected, emit a zero-movement mouse report. The
 * 1s granularity keeps interval changes responsive; the task otherwise
 * sleeps and never touches glide/mouse state. */
static void keepalive_task_fn(void *arg)
{
    (void)arg;
    int elapsed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int interval = keepalive_interval_s_;
        if (interval <= 0) {
            elapsed = 0;
            continue;
        }
        if (++elapsed < interval) continue;
        elapsed = 0;
        if (hid_ble_connected()) {
            /* buttons preserved, dx=dy=wheel=0: no cursor movement, but the
             * host's HID stack sees activity and keeps the link alive. */
            send_mouse(mouse_buttons_, 0, 0, 0);
        }
    }
}

void hid_ble_keepalive_set_interval(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 3600) seconds = 3600;
    keepalive_interval_s_ = seconds;
    ESP_LOGI(TAG, "ble keepalive interval %ds (%s)", seconds,
             seconds > 0 ? "enabled" : "disabled");
}

int hid_ble_keepalive_get_interval(void)
{
    return keepalive_interval_s_;
}

bool hid_ble_keepalive_enabled(void)
{
    return keepalive_interval_s_ > 0;
}

esp_err_t hid_ble_advertise(bool start)
{
    if (start) {
        if (advertising_) {
            return ESP_OK;
        }
        start_advertising();
        return advertising_ ? ESP_OK : ESP_FAIL;
    }
    int rc = ble_gap_adv_stop();
    if (rc == 0) {
        advertising_ = false;
        ESP_LOGI(TAG, "advertising stopped by command");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "adv_stop failed: %d", rc);
    return ESP_FAIL;
}

bool hid_ble_advertising(void)
{
    return advertising_;
}

esp_err_t hid_ble_unbond(void)
{
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_store_clear failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "all BLE bonds cleared from NVS");
    return ESP_OK;
}

void hid_ble_set_event_cb(hid_ble_event_cb_t cb)
{
    event_cb_ = cb;
}

void hid_ble_set_pair_ui_cb(hid_ble_pair_ui_cb_t cb)
{
    pair_ui_cb_ = cb;
}

esp_err_t hid_ble_pair_confirm(uint32_t gen, bool accept)
{
    /* NCP-1 revB: Dispatch on which prompt is live. */
    if (pair_waiting_ && gen == pair_gen_) {
        /* Numeric Comparison: inject the user's decision into SMP. */
        pair_waiting_ = false;
        struct ble_sm_io io = {
            .action = BLE_SM_IOACT_NUMCMP,
            .numcmp_accept = accept ? 1 : 0,
        };
        int rc = ble_sm_inject_io(pair_conn_, &io);
        if (rc != 0) {
            ESP_LOGW(TAG, "pair_confirm(NC): inject_io failed: %d", rc);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "pair_confirm(NC): %s (gen=%u)",
                 accept ? "accepted" : "rejected", (unsigned)gen);
        return ESP_OK;
    }
    if (consent_pending_ && gen == pair_gen_) {
        /* [revB] Consent gate: accept keeps the bond (approval = keeping);
         * reject deletes the just-created bond and drops the link. */
        consent_pending_ = false;
        if (accept) {
            conn_approved_ = true;
            ESP_LOGI(TAG, "consent: approved (gen=%u)", (unsigned)gen);
        } else {
            ble_store_util_delete_peer(&peer_id_addr_);
            ble_gap_terminate(pair_conn_, BLE_ERR_REM_USER_CONN_TERM);
            ESP_LOGW(TAG, "consent: rejected, bond deleted (gen=%u)",
                     (unsigned)gen);
        }
        return ESP_OK;
    }
    /* Stale gen or nothing pending: late tap after timeout/disconnect.
     * Normal, ignore silently. */
    return ESP_ERR_INVALID_STATE;
}

bool hid_ble_conn_approved(void)
{
    return conn_approved_;
}

void hid_ble_abort_pairing(void)
{
    /* Terminate the connection. The DISCONNECT handler cleans up pairing
     * state (deletes unapproved bond if consent was pending) and dismisses
     * the modal via the UI callback. */
    if (conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "pairing aborted by user");
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
}

esp_err_t hid_ble_type_text(const char *text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t keys[6] = { 0 };
    for (const char *p = text; *p; p++) {
        uint8_t usage;
        bool shift;
        if (!ascii_to_hid(*p, &usage, &shift)) {
            continue; /* skip unmappable chars */
        }
        keys[0] = usage;
        esp_err_t err = send_keyboard(shift ? 0x02 : 0x00, keys);
        if (err != ESP_OK) {
            return err;
        }
        /* Press-to-release gap: long enough for the host HID driver
         * to register a distinct keypress. */
        vTaskDelay(pdMS_TO_TICKS(15));
        keys[0] = 0;
        err = send_keyboard(0, keys);
        if (err != ESP_OK) {
            return err;
        }
        /* Inter-keystroke gap: pace to the BLE connection interval so
         * notifications drain faster than we queue them. 30ms covers
         * the typical 30ms HID interval with margin. The old 4ms gap
         * sent ~166 notifies/sec — far above what a 30-50ms interval
         * can carry — exhausting the 36-mbuf pool on long strings. */
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_OK;
}

esp_err_t hid_ble_key(const char *code, const char *mod)
{
    if (!code || !*code) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t usage;
    bool shift = false;
    if (strlen(code) == 1) {
        if (!ascii_to_hid(code[0], &usage, &shift)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (!named_key_usage(code, &usage)) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mods = parse_mods(mod) | (shift ? 0x02 : 0x00);
    uint8_t keys[6] = { usage, 0, 0, 0, 0, 0 };
    esp_err_t err = send_keyboard(mods, keys);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(keys, 0, sizeof(keys));
    return send_keyboard(0, keys);
}

esp_err_t hid_ble_mouse_move(int dx, int dy)
{
    while (dx != 0 || dy != 0) {
        int8_t sx = dx > 127 ? 127 : (dx < -127 ? -127 : (int8_t)dx);
        int8_t sy = dy > 127 ? 127 : (dy < -127 ? -127 : (int8_t)dy);
        esp_err_t err = send_mouse(mouse_buttons_, sx, sy, 0);
        if (err != ESP_OK) {
            return err;
        }
        dx -= sx;
        dy -= sy;
        if (dx != 0 || dy != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return ESP_OK;
}

static volatile bool glide_active_;

/**
 * Internal glide worker. Moves dx,dy over dur_ms.
 * Returns ESP_OK on completion, error on BLE failure.
 * Runs synchronously in the caller (the link worker task); there is no
 * background glide, so no abort mechanism exists in this build.
 */
static esp_err_t glide_worker(int dx, int dy, int dur_ms)
{
    // Smooth glide: interpolate into small steps at ~125Hz (8ms),
    // mimicking real mouse hardware for consistent host quantization.
    const int interval_ms = 8;
    if (dur_ms < 8) dur_ms = 8;
    if (dur_ms > 10000) dur_ms = 10000;  // cap at 10 seconds
    int steps = dur_ms / interval_ms;
    if (steps < 1) steps = 1;

    float fdx = (float)dx / steps;
    float fdy = (float)dy / steps;
    float acc_x = 0.0f, acc_y = 0.0f;
    int sent_x = 0, sent_y = 0;

    for (int i = 0; i < steps; i++) {
        acc_x += fdx;
        acc_y += fdy;
        // Extract integer part, keep fractional remainder
        int8_t sx = (int8_t)acc_x;  // truncates toward zero
        int8_t sy = (int8_t)acc_y;
        // Handle negative truncation correctly: (int8_t)(-0.7) = 0, want to keep -0.7
        // So only subtract what we actually send
        acc_x -= sx;
        acc_y -= sy;
        sent_x += sx;
        sent_y += sy;

        if (sx != 0 || sy != 0) {
            esp_err_t err = send_mouse(mouse_buttons_, sx, sy, 0);
            if (err != ESP_OK) {
                return err;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
    // Send any remaining fractional accumulation as integers
    int rx = dx - sent_x;
    int ry = dy - sent_y;
    while (rx != 0 || ry != 0) {
        int8_t sx = rx > 127 ? 127 : (rx < -127 ? -127 : (int8_t)rx);
        int8_t sy = ry > 127 ? 127 : (ry < -127 ? -127 : (int8_t)ry);
        esp_err_t err = send_mouse(mouse_buttons_, sx, sy, 0);
        if (err != ESP_OK) {
            return err;
        }
        rx -= sx;
        ry -= sy;
        if (rx != 0 || ry != 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return ESP_OK;
}

esp_err_t hid_ble_mouse_glide(int dx, int dy, int dur_ms)
{
    // Synchronous wrapper: reentrancy guard, then run the worker inline.
    if (glide_active_) return ESP_ERR_INVALID_STATE;
    glide_active_ = true;
    esp_err_t err = glide_worker(dx, dy, dur_ms);
    glide_active_ = false;
    return err;
}

esp_err_t hid_ble_mouse_wheel(int delta)
{
    while (delta != 0) {
        int8_t s = delta > 127 ? 127 : (delta < -127 ? -127 : (int8_t)delta);
        esp_err_t err = send_mouse(mouse_buttons_, 0, 0, s);
        if (err != ESP_OK) {
            return err;
        }
        delta -= s;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

static uint8_t mouse_button_mask(const char *button)
{
    if (!button || strcmp(button, "left") == 0) {
        return 0x01;
    } else if (strcmp(button, "right") == 0) {
        return 0x02;
    } else if (strcmp(button, "middle") == 0) {
        return 0x04;
    }
    return 0x00; /* unknown */
}

/* Held-button mask lives with the other module state near the top of
 * this file (mouse_buttons_); it is ORed into every mouse report. */

esp_err_t hid_ble_mouse_button(const char *button, bool pressed)
{
    uint8_t btn = mouse_button_mask(button);
    if (btn == 0x00) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pressed) {
        mouse_buttons_ |= btn;
    } else {
        mouse_buttons_ &= (uint8_t)~btn;
    }
    return send_mouse(mouse_buttons_, 0, 0, 0);
}

esp_err_t hid_ble_mouse_click(const char *button)
{
    uint8_t btn = mouse_button_mask(button);
    if (btn == 0x00) {
        return ESP_ERR_INVALID_ARG;
    }
    mouse_buttons_ |= btn;
    esp_err_t err = send_mouse(mouse_buttons_, 0, 0, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    mouse_buttons_ &= (uint8_t)~btn;
    return send_mouse(mouse_buttons_, 0, 0, 0);
}

esp_err_t hid_ble_mouse_dblclick(const char *button)
{
    esp_err_t err = hid_ble_mouse_click(button);
    if (err != ESP_OK) {
        return err;
    }
    /* Inter-click gap: comfortably inside the OS double-click window
     * (typically 500ms) but long enough for the host to see two distinct
     * press/release pairs. */
    vTaskDelay(pdMS_TO_TICKS(120));
    return hid_ble_mouse_click(button);
}

esp_err_t hid_ble_consumer(const char *code)
{
    static const struct { const char *name; uint16_t usage; } table[] = {
        { "vol_up", 0xE9 }, { "vol_down", 0xEA }, { "mute", 0xE2 },
        { "play_pause", 0xCD }, { "next", 0xB5 }, { "prev", 0xB6 },
        { NULL, 0 },
    };
    if (!code) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t usage = 0;
    for (int i = 0; table[i].name; i++) {
        if (strcmp(code, table[i].name) == 0) {
            usage = table[i].usage;
            break;
        }
    }
    if (usage == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = send_consumer(usage);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    return send_consumer(0);
}
