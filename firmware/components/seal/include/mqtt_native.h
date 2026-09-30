/* SPDX-License-Identifier: MIT
 * Phase 2 native MQTT: esp-mqtt + TLS to the broker.
 *
 * The device MUST stay in bridged mode gracefully when unprovisioned:
 * mqtt_native_init() never blocks boot, never crash-loops; without
 * WiFi or broker creds the device simply reports mqtt_mode="bridged"
 * and keeps serving the USB-serial link.
 *
 * Dual-mode arbitration: native mode = the device is the MQTT
 * endpoint (subscribes watcher/command, verifies kind-2 itself,
 * publishes kind-1 telemetry). The page auto-disables forwarding
 * when it sees mqtt_mode="native" in status.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Safe to call at boot in every configuration. Starts the MQTT
 * client only when WiFi is up AND broker creds are provisioned.
 * v0.4.3 (R6): the 6144 B native task + queues are created lazily —
 * at boot only if broker creds are already provisioned, otherwise on
 * the first provisioning command (see mqtt_native_ensure_running).
 * The device still reports mqtt_mode="bridged" until native connects. */
esp_err_t mqtt_native_init(void);

/* v0.4.3 (R6): idempotent lazy creation of the native task + queues.
 * Called by mqtt_native_init() when creds are already provisioned,
 * and by every provisioning command below. */
esp_err_t mqtt_native_ensure_running(void);

/* True once the native client is connected to the broker. */
bool mqtt_native_is_up(void);

/* "native" | "bridged" — for the status/heartbeat serial contract. */
const char *seal_mqtt_mode(void);

/* Serial provisioning: {"t":"mqtt_cred","user":"..","pass":".."}.
 * Stores to NVS (namespace "seal": mqtt_user/mqtt_pass) via the
 * single NVS writer task; triggers (re)connect when WiFi is up. */
int mqtt_native_cmd_cred(const char *user, const char *pass);

/* Serial provisioning: {"t":"wifi_cred","ssid":"..","pass":".."}.
 * Thin wrapper over the net component (kept here so all seal/mqtt
 * provisioning commands live together). */
int mqtt_native_cmd_wifi(const char *ssid, const char *pass);

/* Serial provisioning: {"t":"mqtt_broker","url":"mqtts://host:8883"}.
 * mqtts:// or mqtt:// (plaintext warns loudly); persisted to NVS
 * (namespace "seal": mqtt_uri) via the single writer task; the native
 * client reconnects to the new broker. */
int mqtt_native_cmd_broker(const char *url);

/* Serial provisioning: {"t":"mqtt_topic","prefix":"watcher"}.
 * One topic level [A-Za-z0-9_-]{1,32}; persisted to NVS (mqtt_prefix);
 * the native client resubscribes as <prefix>/command and publishes to
 * <prefix>/telemetry|heartbeat|status. */
int mqtt_native_cmd_topic(const char *prefix);

#ifdef __cplusplus
}
#endif
