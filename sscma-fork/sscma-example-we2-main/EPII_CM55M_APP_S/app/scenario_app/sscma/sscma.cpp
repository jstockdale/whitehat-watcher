#include <sscma.h>

#include <cstdio>
#include <forward_list>
#include <string>
#include <vector>

#include "__sscma.h"
#include "ma_codec.h"
#include "ma_server.h"
#include "resource.hpp"

static void __task(void*) {
    MA_LOGD(MA_TAG, "Initializing Encoder");
    ma::EncoderJSON encoder;

    MA_LOGD(MA_TAG, "Initializing ATServer");
    ma::ATServer    server(encoder);

    int ret = 0;

    MA_LOGD(MA_TAG, "Initializing ATServer services");
    ret = server.init();
    if (ret != MA_OK) {
        MA_LOGE(MA_TAG, "ATServer init failed: %d", ret);
    }

    MA_LOGD(MA_TAG, "Starting ATServer");
    ret = server.start();
    if (ret != MA_OK) {
        MA_LOGE(MA_TAG, "ATServer start failed: %d", ret);
    }

    MA_LOGD(MA_TAG, "ATServer started");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" {

int app_main(void) {
    puts("Build date: " __DATE__ " " __TIME__);
    if (xTaskCreate(__task, "__task", 20480, NULL, 3, NULL) != pdPASS) {
        puts("__task creation failed!");
        while (1) {
        }
    }

    vTaskStartScheduler();

    return 0;
}

}
