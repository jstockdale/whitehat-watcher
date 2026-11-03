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
    /* v0.4.12 DIAG: heap baseline BEFORE any task/allocation. v0.4.11 proved
     * the FreeRTOS heap is healthy here (1,461,784 B) while boot still threw
     * std::bad_alloc — the thrower was libstdc++'s default ALIGNED operator
     * new routing to newlib's 16 KiB _sbrk heap. Fixed in v0.4.12 by routing
     * aligned new/delete to pvPortMalloc (ma_device.cpp). If this line
     * prints and boot proceeds past it, the fix is confirmed. */
    printf("DIAG v0.4.15: heap free at app_main: %lu bytes\n",
           (unsigned long)xPortGetFreeHeapSize());
    #ifdef CAPTURE_ONLY_MODE
    /* v0.4.6.2: 16KB stack sufficient for AT server only (no TFLM inference).
     * Frees 64KB for 1296x972 RAW8 capture buffer. */
    if (xTaskCreate(__task, "__task", 4096, NULL, 3, NULL) != pdPASS) {
#else
    if (xTaskCreate(__task, "__task", 20480, NULL, 3, NULL) != pdPASS) {
#endif
        puts("__task creation failed!");
        while (1) {
        }
    }

    vTaskStartScheduler();

    return 0;
}

}
