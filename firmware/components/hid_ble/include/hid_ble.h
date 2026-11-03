#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up NimBLE as a composite HID device (keyboard + mouse + consumer).
 *  Advertising name: hid_ble_device_name() ("Watcher-HID" unless changed
 *  via {"t":"set_name"}). Bonds persist in NVS. */
esp_err_t hid_ble_init(void);

/** True while a host is connected over BLE. */
bool hid_ble_connected(void);

/** Optional callback fired on BLE connect/disconnect. */
typedef void (*hid_ble_event_cb_t)(bool connected);
void hid_ble_set_event_cb(hid_ble_event_cb_t cb);

/** Type printable ASCII text (US layout) as key press+release pairs. */
esp_err_t hid_ble_type_text(const char *text);

/** Press+release one key. `code` is a single char or a name: enter, esc,
 *  backspace, tab, space, f1..f12, up, down, left, right, home, end, pgup,
 *  pgdn, delete, insert. `mod` is NULL or a '+'-joined set of
 *  ctrl/shift/alt/gui, e.g. "ctrl+shift". */
esp_err_t hid_ble_key(const char *code, const char *mod);

/** Relative mouse move; large deltas are split into multiple reports. */
esp_err_t hid_ble_mouse_move(int dx, int dy);

/**
 * Smooth relative mouse glide over dur_ms milliseconds.
 * Interpolates the move into small steps at ~125Hz (8ms intervals),
 * mimicking real mouse input for consistent host-side quantization.
 * Total displacement is exact (remainder sent at the end).
 * Runs synchronously in the caller; abort is not supported.
 */
esp_err_t hid_ble_mouse_glide(int dx, int dy, int dur_ms);

/** Scroll wheel; positive = up. */
esp_err_t hid_ble_mouse_wheel(int delta);

/** Click: button is "left", "right" or "middle". */
esp_err_t hid_ble_mouse_click(const char *button);

/** Double-click: two clicks with on-device inter-click timing (~120ms),
 *  so the OS reliably sees a double-click instead of two singles. */
esp_err_t hid_ble_mouse_dblclick(const char *button);

/** Hold or release a mouse button without the click's auto-release.
 *  Used for drag gestures: press on drag-arm, release on lift. */
esp_err_t hid_ble_mouse_button(const char *button, bool pressed);

/** Consumer control: vol_up, vol_down, mute, play_pause, next, prev. */
esp_err_t hid_ble_consumer(const char *code);

/** Start or stop BLE advertising on demand. Advertising also restarts
 *  automatically after every disconnect; this is the manual override. */
esp_err_t hid_ble_advertise(bool start);

/** True while the advertising set is running. */
bool hid_ble_advertising(void);

/**
 * Firmware-side BLE HID keepalive interval, in seconds. When > 0, a
 * background task sends a zero-movement mouse report every interval so
 * the host keeps the BLE HID connection alive WITHOUT moving the cursor.
 * 0 disables it. Defaults to 60 at boot (always runs unless set to 0).
 * Set via {"t":"keepalive","interval_s":N}; the legacy {"t":"keepalive",
 * "on":1|0} maps to 60/0. Reported as keepalive_interval_s in the boot
 * "ready" event and hid_status. No user-facing indicator — it just runs.
 */
void hid_ble_keepalive_set_interval(int seconds);

/** Current BLE keepalive interval in seconds (0 = disabled). */
int hid_ble_keepalive_get_interval(void);

/** True if firmware-side BLE keepalive is currently enabled (interval > 0). */
bool hid_ble_keepalive_enabled(void);

/**
 * Host OS jiggler: when on, a background task sends a small mouse movement
 * (+px then -px, returning to the exact start position) every interval_s
 * seconds to keep the host OS from sleeping. Controlled by the console
 * keepalive pill via {"t":"jiggle","on":1|0,"px":N,"interval_s":N,
 * "subpixel":0|1}. Defaults off at boot. Never fires while a glide is
 * active. State reported in hid_status.
 */
void hid_ble_jiggle_set(bool on, int px, int interval_s, bool subpixel);

/** Read current jiggler state (any out-param may be NULL). */
/* NOTE: Host OS jiggler moved to console page (v0.4.29+). */

/** Drop every stored bond/CCCD from NVS so the next connection pairs
 *  fresh. An active connection stays up; forget the device on the host
 *  as well, then reconnect. */
esp_err_t hid_ble_unbond(void);

/** Maximum device-name length in bytes (not counting the NUL). */
#define HID_BLE_NAME_MAX 32

/** Current device name: Bluetooth advertising/GAP name, the "name" field
 *  of the boot "ready" event, and the on-screen title. Defaults to
 *  "Watcher-HID"; a name stored in NVS by an earlier set_name is loaded
 *  at hid_ble_init(). */
const char *hid_ble_device_name(void);

/** Set the device name (1..HID_BLE_NAME_MAX printable ASCII chars) and
 *  persist it to NVS ("dev" namespace). The GAP device name updates
 *  immediately; the advertising payload and on-screen title pick it up
 *  on the next advertising start / reboot. Returns ESP_OK on success,
 *  ESP_ERR_INVALID_ARG when the name is missing, too long, or contains
 *  non-printable characters. */
esp_err_t hid_ble_set_device_name(const char *name);

/* NCP-1 revB: Numeric Comparison pairing UI + Just Works consent gate.
 *
 * Pairing UI callback: invoked from the NimBLE host task — implementations
 * must only queue and return (the .72 ble_evt rule). */
typedef enum {
    HID_BLE_PAIR_UI_PASSKEY,  /* PRIMARY: show 6 digits, host types them    */
    HID_BLE_PAIR_UI_NUMCMP,   /* defensive only — cannot fire with
                               * DISPLAY_ONLY per the SC table; kept so a
                               * future io_cap change cannot reopen the
                               * silent-default hole                        */
    HID_BLE_PAIR_UI_CONSENT,  /* yes/no: unverified computer paired         */
    HID_BLE_PAIR_UI_DISMISS,  /* dismiss any prompt                        */
} hid_ble_pair_ui_t;

typedef void (*hid_ble_pair_ui_cb_t)(uint32_t gen, hid_ble_pair_ui_t what,
                                     uint32_t number /* NUMCMP/PASSKEY only */);
void hid_ble_set_pair_ui_cb(hid_ble_pair_ui_cb_t cb);

/* User's answer for BOTH prompt kinds (NC confirm and consent yes/no).
 * Safe from any task. Stale gen / nothing pending → ESP_ERR_INVALID_STATE
 * (late tap after timeout or disconnect: normal, ignore silently). */
esp_err_t hid_ble_pair_confirm(uint32_t gen, bool accept);

/* [revB] HID TX gate — report choke points check this before sending. */
bool hid_ble_conn_approved(void);

/* Abort an in-progress pairing: terminates the connection, which dismisses
 * any modal via the DISCONNECT handler. Safe from any task. */
void hid_ble_abort_pairing(void);

#ifdef __cplusplus
}
#endif
