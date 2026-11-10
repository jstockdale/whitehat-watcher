/* SPDX-License-Identifier: MIT
 * Phase 2 native MQTT: the device as its own MQTT endpoint (esp-mqtt +
 * TLS; broker URI and topic prefix provisionable, defaulting to
 * mqtts://mqtt.offx1.com:8883 and "watcher").
 *
 * Dual-mode arbitration: native mode = the device subscribes
 * <prefix>/command, verifies kind-2 envelopes itself (with the DELIVERED
 * topic), and publishes kind-1-MAC'd telemetry. The page auto-disables
 * forwarding when it sees mqtt_mode="native" in status/heartbeat.
 *
 * Boot safety: mqtt_native_init() only creates queues and a poll task.
 * The client starts only when WiFi is up AND broker creds are
 * provisioned; otherwise the device stays dormant and reports
 * mqtt_mode="bridged". Reconnect is esp-mqtt's own (do not fight it).
 *
 * Inbound payloads: raw WMS2 bytes, or {"sealed":"<b64>"} JSON, or
 * (grace only, strict=0) a plain JSON command. Verified bodies are
 * {"id","cmd"}: the inner command is dispatched through
 * link_sealed_dispatch with _mid=id (tagged _via_mqtt, so serial-only
 * provisioning commands are refused for it).
 *
 * Outbound: every serial TX line is mirrored to MQTT while native is
 * up (heartbeat -> <prefix>/heartbeat, rest -> <prefix>/telemetry).
 * When a telemetry seed is provisioned, heartbeat/status JSON carries
 * wms2_mac = hex16(BLAKE2b-keyed(k_tel_mac,
 * u16le(len(topic))||topic||body)); without a seed the JSON goes out
 * un-MAC'd (skip silently).
 *
 * D6: no large stack buffers. The envelope decode buffer is static;
 * JSON copies are heap.
 */

#include "mqtt_native.h"
#include "task_psram.h"
#include "esp_heap_caps.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

#include "net.h"
#include "seal_dev.h"
#include "wia1.h"
#include "monocypher.h"

/* link.h is deliberately NOT #included (one-directional build DAG:
 * link.c #includes this component's headers, never the reverse). */
extern void link_sealed_dispatch(const char *json, size_t len, bool verified);
extern void link_set_tx_mirror(void (*cb)(const char *line, size_t len));

static const char *TAG = "mqtt_native";

/* v0.4.6: native MQTT task runs on PSRAM stack (was 24KB internal). */
static task_psram_handle_t *native_psram_ = NULL;

#define MQTT_BROKER_URI_DEFAULT   "mqtts://mqtt.offx1.com:8883"
#define MQTT_TOPIC_PREFIX_DEFAULT "watcher"
/* Broker URI and topic prefix are runtime-configurable (serial
 * {"t":"mqtt_broker"} / {"t":"mqtt_topic"}) and persisted in the seal
 * NVS namespace. Defaults keep 0.4.0 behavior on a fresh device. */
#define MQTT_BROKER_URI_MAX   95
#define MQTT_TOPIC_PREFIX_MAX 32

/* Max inbound envelope the device will decode: 40 + 2048 + 64.
 * v0.4.3 (R2): moved to PSRAM — CPU-only base64 decode scratch (no DMA,
 * no ISR), saves 2152 B of internal DRAM. */
static EXT_RAM_BSS_ATTR uint8_t
    in_env_[WMS2_HDR_LEN + SEAL_MAX_CMD_BODY + WMS2_SIG_LEN];

typedef struct {
    uint8_t *data;
    size_t   len;
    char     topic[96];
} in_msg_t;

typedef struct {
    char  *line;
    size_t len;
} tx_msg_t;

static bool                  inited_ = false;
static esp_mqtt_client_handle_t client_ = NULL;
static bool                  native_up_ = false;
static QueueHandle_t         in_q_ = NULL;
static QueueHandle_t         tx_q_ = NULL;
static volatile bool         creds_dirty_ = false;
/* Static so esp-mqtt can keep referencing them for the session. */
static char mqtt_user_[64];
static char mqtt_pass_[128];
/* Broker URI + topic prefix: NVS-backed, rebuilt into the four concrete
 * topics by topics_rebuild(). */
static char broker_uri_[MQTT_BROKER_URI_MAX + 1];
static char topic_prefix_[MQTT_TOPIC_PREFIX_MAX + 1];
static char sub_topic_[MQTT_TOPIC_PREFIX_MAX + 16];
static char tel_topic_[MQTT_TOPIC_PREFIX_MAX + 16];
static char hb_topic_[MQTT_TOPIC_PREFIX_MAX + 16];
static char status_topic_[MQTT_TOPIC_PREFIX_MAX + 16];

static void topics_rebuild(void)
{
    snprintf(sub_topic_, sizeof(sub_topic_), "%s/command", topic_prefix_);
    snprintf(tel_topic_, sizeof(tel_topic_), "%s/telemetry", topic_prefix_);
    snprintf(hb_topic_, sizeof(hb_topic_), "%s/heartbeat", topic_prefix_);
    snprintf(status_topic_, sizeof(status_topic_), "%s/status",
             topic_prefix_);
}

/* ------------------------------------------------------------------ */
/* kind-1 MAC for native telemetry                                     */
/* ------------------------------------------------------------------ */

/* Serialize `o`, compute tag = first-16-bytes of
 * BLAKE2b-keyed(k_tel_mac, u16le(len(topic))||topic||body), attach it
 * as "wms2_mac" (32 hex chars), and return the re-serialized string.
 * Returns NULL (caller publishes un-MAC'd) when no tel seed. */
static char *json_with_mac(cJSON *o, const char *topic)
{
    uint8_t k_mac[32], k_aead[32];
    char *ret = NULL;
    if (!seal_dev_tel_keys(k_mac, k_aead)) {
        return NULL;
    }
    crypto_wipe(k_aead, sizeof(k_aead));
    char *body = cJSON_PrintUnformatted(o);
    if (body) {
        size_t tlen = strlen(topic);
        size_t blen = strlen(body);
        size_t mlen = 2 + tlen + blen;
        uint8_t *msg = heap_caps_malloc(mlen, MALLOC_CAP_SPIRAM); /* v0.4.6: PSRAM */
        if (msg) {
            uint8_t tag[16]; /* digest_size=16: matches mqtt_seal TAG_LEN */
            char machex[33];
            msg[0] = (uint8_t)(tlen & 0xff);
            msg[1] = (uint8_t)((tlen >> 8) & 0xff);
            memcpy(msg + 2, topic, tlen);
            memcpy(msg + 2 + tlen, body, blen);
            crypto_blake2b_keyed(tag, sizeof(tag), k_mac, sizeof(k_mac),
                                 msg, mlen);
            crypto_wipe(msg, mlen);
            free(msg);
            for (int i = 0; i < 16; i++) {
                snprintf(machex + 2 * i, 3, "%02x", tag[i]);
            }
            machex[32] = '\0';
            crypto_wipe(tag, sizeof(tag));
            cJSON_AddStringToObject(o, "wms2_mac", machex);
            ret = cJSON_PrintUnformatted(o);
        }
        free(body);
    }
    crypto_wipe(k_mac, sizeof(k_mac));
    return ret;
}

/* Our own serializer emits {"t":"heartbeat",...} with no spaces; a
 * substring check is enough to route the line. */
static bool is_heartbeat_line(const char *line)
{
    return strstr(line, "\"t\":\"heartbeat\"") != NULL;
}

/* ------------------------------------------------------------------ */
/* TX mirror: every serial line -> MQTT while native is up.            */
/* Called from link_send with tx_lock_ held: never block, never call   */
/* back into link.                                                     */
/* ------------------------------------------------------------------ */

void mqtt_native_mirror_tx(const char *line, size_t len)
{
    if (!line || len == 0 || !tx_q_ || !native_up_) {
        return;
    }
    tx_msg_t m;
    m.len = len;
    m.line = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM); /* v0.4.6: PSRAM */
    if (!m.line) {
        return;
    }
    memcpy(m.line, line, len);
    m.line[len] = '\0';
    if (xQueueSend(tx_q_, &m, 0) != pdPASS) {
        free(m.line); /* drop on full: serial must never wait on MQTT */
    }
}

static void drain_tx_queue(void)
{
    tx_msg_t m;
    if (!client_) {
        return;
    }
    if (!native_up_) {
        /* Drop backlog while offline: stale lines would confuse the host. */
        while (xQueueReceive(tx_q_, &m, 0) == pdTRUE) {
            free(m.line);
        }
        return;
    }
    while (xQueueReceive(tx_q_, &m, 0) == pdTRUE) {
        const char *topic = tel_topic_;
        char *pub = m.line;
        char *macd = NULL;
        if (is_heartbeat_line(m.line)) {
            /* Heartbeat goes to the page's heartbeat topic, MAC'd when
             * the tel seed is provisioned. */
            topic = hb_topic_;
            cJSON *o = cJSON_Parse(m.line);
            if (o) {
                macd = json_with_mac(o, topic);
                cJSON_Delete(o);
                if (macd) {
                    pub = macd;
                }
            }
        }
        esp_mqtt_client_publish(client_, topic, pub, (int)strlen(pub), 0, 0);
        free(m.line);
        free(macd);
    }
}

/* ------------------------------------------------------------------ */
/* Inbound: verify, then dispatch into the link worker path.           */
/* ------------------------------------------------------------------ */

static void native_tel_err(const char *cmd, const char *msg)
{
    if (!client_ || !native_up_) {
        return;
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return;
    }
    cJSON_AddStringToObject(o, "t", "err");
    cJSON_AddStringToObject(o, "cmd", cmd ? cmd : "");
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        esp_mqtt_client_publish(client_, tel_topic_, s, (int)strlen(s),
                                0, 0);
        free(s);
    }
}

static void dispatch_verified_body(const uint8_t *body, size_t blen)
{
    char *js;
    char *s;
    cJSON *outer;
    cJSON *id;
    cJSON *inner;

    if (!body || blen == 0 || blen > SEAL_MAX_CMD_BODY) {
        return;
    }
    js = heap_caps_malloc(blen + 1, MALLOC_CAP_SPIRAM); /* v0.4.6: PSRAM */
    if (!js) {
        return;
    }
    memcpy(js, body, blen);
    js[blen] = '\0';
    outer = cJSON_Parse(js);
    free(js);
    if (!outer) {
        ESP_LOGW(TAG, "sealed body is not JSON");
        return;
    }
    id = cJSON_DetachItemFromObject(outer, "id");
    inner = cJSON_DetachItemFromObject(outer, "cmd");
    cJSON_Delete(outer);
    if (!cJSON_IsObject(inner)) {
        ESP_LOGW(TAG, "sealed body has no cmd object");
        cJSON_Delete(inner);
        cJSON_Delete(id);
        return;
    }
    if (cJSON_IsString(id) && id->valuestring) {
        cJSON_AddStringToObject(inner, "_mid", id->valuestring);
    } else if (cJSON_IsNumber(id)) {
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%d", id->valueint);
        cJSON_AddStringToObject(inner, "_mid", tmp);
    }
    cJSON_Delete(id);
    s = cJSON_PrintUnformatted(inner);
    cJSON_Delete(inner);
    if (s) {
        /* v0.4.50: this body came out of a seal_dev-verified envelope
         * (both callers verify first) — dispatch as _sealed. */
        link_sealed_dispatch(s, strlen(s), true);
        free(s);
    }
}

static void process_inbound(const uint8_t *data, size_t len,
                            const char *topic)
{
    const uint8_t *body = NULL;
    size_t blen = 0;
    wms2_hdr hdr;
    wms2_rc rc;

    if (len >= 4 && memcmp(data, "WMS2", 4) == 0) {
        /* Raw WMS2 envelope: verify against the DELIVERED topic. */
        rc = seal_dev_verify_envelope(data, len, topic, &body, &blen,
                                      &hdr);
        if (rc < 0) {
            native_tel_err("sealed", wms2_rc_str(rc));
            return;
        }
        if (rc == WMS2_OK_DUP) {
            return; /* QoS echo: already executed, stay silent */
        }
        dispatch_verified_body(body, blen);
        return;
    }
    if (len == 0 || len > 8192 || data[0] != '{') {
        ESP_LOGW(TAG, "dropping %u non-JSON bytes", (unsigned)len);
        return;
    }
    char *js = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM); /* v0.4.6: PSRAM */
    cJSON *o;
    cJSON *sealed;
    if (!js) {
        return;
    }
    memcpy(js, data, len);
    js[len] = '\0';
    o = cJSON_Parse(js);
    if (!o) {
        ESP_LOGW(TAG, "unparseable JSON on command topic");
        free(js);
        return;
    }
    sealed = cJSON_GetObjectItem(o, "sealed");
    if (cJSON_IsString(sealed) && sealed->valuestring) {
        const char *b64 = sealed->valuestring;
        size_t b64len = strlen(b64);
        size_t olen = 0;
        int brc = -1;
        /* Decode BEFORE deleting o: b64 points into the cJSON tree. */
        if (b64len <= 4096) {
            brc = mbedtls_base64_decode(in_env_, sizeof(in_env_), &olen,
                                        (const unsigned char *)b64, b64len);
        }
        cJSON_Delete(o);
        free(js);
        if (brc != 0) {
            native_tel_err("sealed", "bad b64");
            return;
        }
        rc = seal_dev_verify_envelope(in_env_, olen, topic, &body, &blen,
                                      &hdr);
        if (rc < 0) {
            native_tel_err("sealed", wms2_rc_str(rc));
            return;
        }
        if (rc == WMS2_OK_DUP) {
            return;
        }
        dispatch_verified_body(body, blen);
        return;
    }
    cJSON_Delete(o);
    if (seal_dev_strict()) {
        free(js);
        native_tel_err("plain", "strict: unsealed command rejected");
        return;
    }
    /* Grace: plain JSON command from the topic. v0.4.50: dispatched
     * UNVERIFIED — tagged _via_mqtt but never _sealed. */
    link_sealed_dispatch(js, len, false);
    free(js);
}

static void drain_in_queue(void)
{
    in_msg_t m;
    while (xQueueReceive(in_q_, &m, 0) == pdTRUE) {
        process_inbound(m.data, m.len, m.topic);
        if (m.len) {
            crypto_wipe(m.data, m.len); /* payload may be sensitive */
        }
        free(m.data);
    }
}

/* ------------------------------------------------------------------ */
/* Client lifecycle: never block boot, never fight esp-mqtt's own     */
/* reconnect.                                                          */
/* ------------------------------------------------------------------ */

static bool load_mqtt_creds(char *user, size_t ulen, char *pass, size_t plen)
{
    /* v0.4.7: via the nvs task. The mqtt_native task runs on a PSRAM
     * stack and must never call nvs_* directly. */
    size_t l1 = ulen, l2 = plen;
    bool ok = seal_dev_nvs_read_blob_sync("", "mqtt_user", user, &l1,
                                          5000) == ESP_OK &&
              seal_dev_nvs_read_blob_sync("", "mqtt_pass", pass, &l2,
                                          5000) == ESP_OK;
    if (!ok) {
        return false;
    }
    user[ulen - 1] = '\0';
    pass[plen - 1] = '\0';
    return user[0] != '\0';
}

/* Load the NVS-backed broker URI and topic prefix (written as NUL-
 * included blobs, same convention as the creds). Missing keys keep the
 * compiled-in defaults, so a fresh device behaves like 0.4.0.
 * v0.4.7: via the nvs task. */
static void load_mqtt_net(void)
{
    size_t l1 = sizeof(broker_uri_), l2 = sizeof(topic_prefix_);

    strncpy(broker_uri_, MQTT_BROKER_URI_DEFAULT, sizeof(broker_uri_));
    strncpy(topic_prefix_, MQTT_TOPIC_PREFIX_DEFAULT,
            sizeof(topic_prefix_));
    if (seal_dev_nvs_read_blob_sync("", "mqtt_uri", broker_uri_, &l1,
                                    5000) == ESP_OK &&
        broker_uri_[0] != '\0') {
        broker_uri_[sizeof(broker_uri_) - 1] = '\0';
    }
    if (seal_dev_nvs_read_blob_sync("", "mqtt_prefix", topic_prefix_, &l2,
                                    5000) == ESP_OK &&
        topic_prefix_[0] != '\0') {
        topic_prefix_[sizeof(topic_prefix_) - 1] = '\0';
    }
    topics_rebuild();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t eid, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch (eid) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        native_up_ = true;
        esp_mqtt_client_subscribe(client_, sub_topic_, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        native_up_ = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "subscribed to %s", sub_topic_);
        break;
    case MQTT_EVENT_DATA:
        if (!e->topic ||
            e->topic_len != (int)strlen(sub_topic_) ||
            strncmp(e->topic, sub_topic_, (size_t)e->topic_len) != 0) {
            break;
        }
        if (e->current_data_offset != 0 ||
            e->data_len != e->total_data_len) {
            ESP_LOGW(TAG, "fragmented payload dropped");
            break;
        } {
            in_msg_t m;
            size_t tl;
            m.len = (size_t)e->data_len;
            m.data = heap_caps_malloc(m.len ? m.len : 1, MALLOC_CAP_SPIRAM); /* v0.4.6: PSRAM */
            if (!m.data) {
                break;
            }
            if (m.len) {
                memcpy(m.data, e->data, m.len);
            }
            tl = (size_t)e->topic_len;
            if (tl >= sizeof(m.topic)) {
                tl = sizeof(m.topic) - 1;
            }
            memcpy(m.topic, e->topic, tl);
            m.topic[tl] = '\0';
            if (xQueueSend(in_q_, &m, 0) != pdPASS) {
                free(m.data);
                ESP_LOGW(TAG, "inbound queue full; command dropped");
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error event");
        break;
    default:
        break;
    }
}

static void start_client(void)
{
    esp_mqtt_client_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.broker.address.uri = broker_uri_;
    cfg.credentials.username = mqtt_user_;
    cfg.credentials.authentication.password = mqtt_pass_;
    cfg.session.keepalive = 60;
    cfg.network.reconnect_timeout_ms = 5000;
    cfg.buffer.size = 8192;
    cfg.buffer.out_size = 8192;
    /* NOTE: no CA is configured, so esp-tls proceeds WITHOUT server
     * certificate verification: the channel is encrypted but the broker
     * is not authenticated (TOFU-grade). Pinning the broker CA is a
     * coordinator TODO before this leaves the bench. */
    client_ = esp_mqtt_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(TAG, "mqtt client init failed");
        return;
    }
    esp_mqtt_client_register_event(client_, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(client_) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt client start failed");
        esp_mqtt_client_destroy(client_);
        client_ = NULL;
        return;
    }
    ESP_LOGW(TAG,
             "native mqtt starting (TLS, no server-cert verification yet)");
}

static void stop_client(void)
{
    if (!client_) {
        return;
    }
    esp_mqtt_client_stop(client_);
    esp_mqtt_client_destroy(client_);
    client_ = NULL;
    native_up_ = false;
}

/* One-shot retained status on (re)connect, so late subscribers (and
 * the page's arbitration) see the mode without waiting for the next
 * heartbeat mirror. */
static cJSON *native_status_obj(void)
{
    cJSON *o = cJSON_CreateObject();
    char sess[17];

    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "t", "status");
    cJSON_AddStringToObject(o, "mqtt_mode", "native");
    cJSON_AddNumberToObject(o, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "seal_floor",
                            (double)seal_dev_floor());
    cJSON_AddBoolToObject(o, "seal_armed", wia1_is_armed());
    cJSON_AddBoolToObject(o, "seal_dev_verify",
                          seal_dev_has_commander_key());
    wia1_session_id_hex(sess);
    if (sess[0]) {
        cJSON_AddStringToObject(o, "seal_session", sess);
    } else {
        cJSON_AddNullToObject(o, "seal_session");
    }
    return o;
}

static void publish_native_status(void)
{
    cJSON *o;
    char *s;

    if (!client_) {
        return;
    }
    o = native_status_obj();
    if (!o) {
        return;
    }
    s = json_with_mac(o, status_topic_); /* NULL when no tel seed */
    cJSON_Delete(o);
    if (!s) {
        /* No tel seed: publish un-MAC'd (skip silently). */
        o = native_status_obj();
        if (!o) {
            return;
        }
        s = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
    }
    if (s) {
        esp_mqtt_client_publish(client_, status_topic_, s,
                                (int)strlen(s), 1, 1);
        free(s);
    }
}

static void native_task(void *arg)
{
    (void)arg;
    bool was_up = false;

    ESP_LOGI(TAG, "dormant: waiting for WiFi + broker creds (bridged)");
    load_mqtt_net(); /* broker URI + topic prefix before first connect */
    for (;;) {
        if (!client_) {
            if (net_is_up() &&
                load_mqtt_creds(mqtt_user_, sizeof(mqtt_user_),
                                mqtt_pass_, sizeof(mqtt_pass_))) {
                ESP_LOGI(TAG, "wifi up + creds present: starting client");
                start_client();
            } else {
                crypto_wipe(mqtt_user_, sizeof(mqtt_user_));
                crypto_wipe(mqtt_pass_, sizeof(mqtt_pass_));
            }
        } else if (creds_dirty_) {
            /* Re-provisioned while running: restart with the new id. */
            creds_dirty_ = false;
            ESP_LOGI(TAG, "creds updated: restarting mqtt client");
            stop_client();
        }
        drain_tx_queue();
        drain_in_queue();
        if (native_up_ && !was_up) {
            publish_native_status();
        }
        was_up = native_up_;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t mqtt_native_init(void)
{
    if (inited_) {
        return ESP_OK;
    }
    /* Mirror every serial TX line to MQTT while native is up. Safe
     * before the queues exist: the mirror drops when tx_q_ is NULL. */
    link_set_tx_mirror(mqtt_native_mirror_tx);
    /* v0.4.3 (R6): defer the 6144 B task + queues until native MQTT can
     * actually be used. If broker creds were provisioned on a previous
     * boot, start now so native comes up after reboot; otherwise stay
     * dormant until the first provisioning command (mqtt_broker,
     * mqtt_topic, mqtt_cred, wifi_cred) calls
     * mqtt_native_ensure_running(). */
    char user[64], pass[64];
    if (load_mqtt_creds(user, sizeof(user), pass, sizeof(pass))) {
        crypto_wipe(user, sizeof(user));
        crypto_wipe(pass, sizeof(pass));
        return mqtt_native_ensure_running();
    }
    crypto_wipe(user, sizeof(user));
    crypto_wipe(pass, sizeof(pass));
    ESP_LOGI(TAG, "native MQTT dormant: no broker creds provisioned");
    return ESP_OK;
}

/* v0.4.3 (R6): idempotent lazy creation of the queues + native task.
 * Called at boot when creds are already provisioned, and from every
 * provisioning command. */
esp_err_t mqtt_native_ensure_running(void)
{
    if (inited_) {
        return ESP_OK;
    }
    in_q_ = xQueueCreate(8, sizeof(in_msg_t));
    tx_q_ = xQueueCreate(16, sizeof(tx_msg_t));
    if (!in_q_ || !tx_q_) {
        return ESP_ERR_NO_MEM;
    }
    /* v0.4.6: PSRAM stack, 6144 words = 24576 bytes (was 24KB internal).
     * TLS needs deep stack; size preserved. */
    native_psram_ = task_psram_create(native_task, "mqtt_native", 6144 * 4,
                                     NULL, 5, tskNO_AFFINITY);
    if (!native_psram_) {
        return ESP_FAIL;
    }
    inited_ = true;
    return ESP_OK;
}

bool mqtt_native_is_up(void)
{
    return native_up_;
}

const char *seal_mqtt_mode(void)
{
    return native_up_ ? "native" : "bridged";
}

/* ------------------------------------------------------------------ */
/* Serial provisioning.                                                */
/* ------------------------------------------------------------------ */

int mqtt_native_cmd_cred(const char *user, const char *pass)
{
    size_t ulen, plen;

    if (!user || !user[0] || !pass) {
        return -1;
    }
    /* v0.4.3 (R6): first provisioning wakes the dormant native task. */
    if (mqtt_native_ensure_running() != ESP_OK) {
        return -1;
    }
    ulen = strlen(user);
    plen = strlen(pass);
    if (ulen > sizeof(mqtt_user_) - 1 || plen > sizeof(mqtt_pass_) - 1) {
        return -1;
    }
    /* Stored through the nvs task (NUL-included). v0.4.7: synchronous;
     * the mqtt_native task runs on a PSRAM stack and must never touch
     * NVS directly. */
    if (seal_dev_nvs_write_blob_sync("", "mqtt_user", user, ulen + 1,
                                     5000) != ESP_OK ||
        seal_dev_nvs_write_blob_sync("", "mqtt_pass", pass, plen + 1,
                                     5000) != ESP_OK) {
        return -1;
    }
    creds_dirty_ = true; /* wake the native task: (re)connect now */
    return 0;
}

int mqtt_native_cmd_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || !pass) {
        return -1;
    }
    /* v0.4.3 (R6): first provisioning wakes the dormant native task. */
    if (mqtt_native_ensure_running() != ESP_OK) {
        return -1;
    }
    return net_set_wifi(ssid, pass) == ESP_OK ? 0 : -1;
}

static bool url_valid(const char *url)
{
    size_t n;
    bool tls, plain;

    if (!url || !url[0]) {
        return false;
    }
    n = strlen(url);
    if (n > MQTT_BROKER_URI_MAX) {
        return false;
    }
    tls = strncmp(url, "mqtts://", 8) == 0;
    plain = strncmp(url, "mqtt://", 7) == 0;
    if (!tls && !plain) {
        return false;
    }
    /* Must have a host after the scheme. */
    return n > (size_t)(tls ? 8 : 7) + 1;
}

static bool prefix_valid(const char *prefix)
{
    size_t n, i;

    if (!prefix || !prefix[0]) {
        return false;
    }
    n = strlen(prefix);
    if (n > MQTT_TOPIC_PREFIX_MAX) {
        return false;
    }
    /* One topic level: no slashes, wildcards, or whitespace. */
    for (i = 0; i < n; i++) {
        char c = prefix[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

int mqtt_native_cmd_broker(const char *url)
{
    size_t n;

    if (!url_valid(url)) {
        return -1;
    }
    /* v0.4.3 (R6): first provisioning wakes the dormant native task. */
    if (mqtt_native_ensure_running() != ESP_OK) {
        return -1;
    }
    n = strlen(url);
    if (strncmp(url, "mqtt://", 7) == 0) {
        ESP_LOGW(TAG, "plaintext mqtt:// broker configured; "
                      "credentials cross the network unencrypted");
    }
    /* Stored through the nvs task (NUL-included). v0.4.7: synchronous. */
    if (seal_dev_nvs_write_blob_sync("", "mqtt_uri", url, n + 1,
                                     5000) != ESP_OK) {
        return -1;
    }
    strncpy(broker_uri_, url, sizeof(broker_uri_));
    creds_dirty_ = true; /* wake the native task: reconnect to the new broker */
    return 0;
}

int mqtt_native_cmd_topic(const char *prefix)
{
    size_t n;

    if (!prefix_valid(prefix)) {
        return -1;
    }
    /* v0.4.3 (R6): first provisioning wakes the dormant native task. */
    if (mqtt_native_ensure_running() != ESP_OK) {
        return -1;
    }
    n = strlen(prefix);
    /* Stored through the nvs task (NUL-included). v0.4.7: synchronous. */
    if (seal_dev_nvs_write_blob_sync("", "mqtt_prefix", prefix, n + 1,
                                     5000) != ESP_OK) {
        return -1;
    }
    strncpy(topic_prefix_, prefix, sizeof(topic_prefix_));
    topics_rebuild();
    creds_dirty_ = true; /* wake the native task: resubscribe under the prefix */
    return 0;
}
