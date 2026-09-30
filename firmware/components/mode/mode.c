/* Device UI mode ring: POINTER / KEYS / MEDIA / TIMER / EYES.
 * The input task reinterprets wheel, touch and wheel-click per mode. */

#include "mode.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "mode";

static const char *names_[MODE_COUNT] = {
    "pointer", "keys", "media", "timer", "eyes",
};

static mode_t current_ = MODE_POINTER;
static mode_change_cb_t change_cb_ = NULL;

void mode_init(void)
{
    current_ = MODE_POINTER;
    ESP_LOGI(TAG, "mode ring ready, starting at pointer");
}

mode_t mode_get(void)
{
    return current_;
}

const char *mode_name(mode_t m)
{
    if (m >= 0 && m < MODE_COUNT) {
        return names_[m];
    }
    return "?";
}

static void fire(mode_t m)
{
    ESP_LOGI(TAG, "mode -> %s", mode_name(m));
    if (change_cb_) {
        change_cb_(m);
    }
}

bool mode_set_by_name(const char *name)
{
    if (!name) {
        return false;
    }
    for (int i = 0; i < MODE_COUNT; i++) {
        if (strcmp(name, names_[i]) == 0) {
            if ((mode_t)i != current_) {
                current_ = (mode_t)i;
                fire(current_);
            }
            return true;
        }
    }
    return false;
}

void mode_next(void)
{
    current_ = (mode_t)((current_ + 1) % MODE_COUNT);
    fire(current_);
}

void mode_set_change_cb(mode_change_cb_t cb)
{
    change_cb_ = cb;
}
