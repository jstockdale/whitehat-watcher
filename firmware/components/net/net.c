/* Wi-Fi station with NVS-persisted credentials.
 * Emits connectivity changes through the callback registered by the app. */

#include "net.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "net";
static const char *NVS_NS = "watcher";
static const char *NVS_SSID = "wifi_ssid";
static const char *NVS_PASS = "wifi_pass";

static net_event_cb_t event_cb_;
static EventGroupHandle_t wifi_events_;
#define GOT_IP_BIT BIT0
/* Only auto-connect when we have credentials (saved or just provided). */
static bool auto_connect_ = false;

static void emit(bool connected, const char *ip)
{
    if (event_cb_) {
        event_cb_(connected, ip);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (auto_connect_) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "disconnected, retrying");
        emit(false, NULL);
        xEventGroupClearBits(wifi_events_, GOT_IP_BIT);
        if (auto_connect_) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        char ip[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
        ESP_LOGI(TAG, "got IP: %s", ip);
        xEventGroupSetBits(wifi_events_, GOT_IP_BIT);
        emit(true, ip);
    }
}

/* v0.4.7: NVS access via the nvs task (declared extern — net is a
 * dependency OF seal, so no CMake cycle; see hid_ble.c). */
extern esp_err_t seal_dev_nvs_write_str_sync(const char *ns, const char *key,
                                             const char *val,
                                             uint32_t timeout_ms);
extern esp_err_t seal_dev_nvs_read_str_sync(const char *ns, const char *key,
                                            char *out, size_t *inout_len,
                                            uint32_t timeout_ms);

static bool load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    /* v0.4.7: via the nvs task. The task is the ONLY code that calls
     * nvs_open — never open NVS handles directly. */
    bool ok = seal_dev_nvs_read_str_sync(NVS_NS, NVS_SSID, ssid, &ssid_len,
                                         5000) == ESP_OK &&
              seal_dev_nvs_read_str_sync(NVS_NS, NVS_PASS, pass, &pass_len,
                                         5000) == ESP_OK &&
              ssid[0] != '\0';
    return ok;
}

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

esp_err_t net_init(void)
{
    wifi_events_ = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL, NULL));

    char ssid[33] = { 0 }, pass[65] = { 0 };
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "connecting to saved SSID '%s'", ssid);
        auto_connect_ = true;
        apply_sta_config(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no saved Wi-Fi credentials; send {\"t\":\"wifi\",...}");
        wifi_config_t cfg = { 0 };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void net_set_event_cb(net_event_cb_t cb)
{
    event_cb_ = cb;
}

/* (extern NVS task APIs declared above, before load_creds) */

esp_err_t net_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid || !pass) {
        return ESP_ERR_INVALID_ARG;
    }
    /* v0.4.3: persist via the nvs task. This runs on the
     * link worker (PSRAM stack) via the {"t":"wifi"} command — direct
     * nvs_set_* / nvs_commit here panics ("Cache disabled but cached
     * memory region accessed"), the same crash as v0.4.1 set_name. */
    esp_err_t err = seal_dev_nvs_write_str_sync(NVS_NS, NVS_SSID, ssid,
                                                10000 /* ms */);
    if (err == ESP_OK) {
        err = seal_dev_nvs_write_str_sync(NVS_NS, NVS_PASS, pass,
                                          10000 /* ms */);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi creds persist failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "saved SSID '%s', reconnecting", ssid);
    auto_connect_ = true;
    ESP_ERROR_CHECK(esp_wifi_stop());
    apply_sta_config(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

bool net_is_up(void)
{
    return wifi_events_ &&
           (xEventGroupGetBits(wifi_events_) & GOT_IP_BIT) != 0;
}
