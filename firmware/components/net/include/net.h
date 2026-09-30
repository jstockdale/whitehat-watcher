#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called on Wi-Fi station state changes. `ip` is NULL when disconnected. */
typedef void (*net_event_cb_t)(bool connected, const char *ip);

/** Start Wi-Fi station; auto-connect using NVS creds if present. */
esp_err_t net_init(void);

/** Register the connectivity event callback (optional). */
void net_set_event_cb(net_event_cb_t cb);

/** Save credentials to NVS and (re)connect. */
esp_err_t net_set_wifi(const char *ssid, const char *pass);

bool net_is_up(void);

#ifdef __cplusplus
}
#endif
