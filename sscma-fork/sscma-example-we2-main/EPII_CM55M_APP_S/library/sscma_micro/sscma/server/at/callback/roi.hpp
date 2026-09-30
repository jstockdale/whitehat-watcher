#pragma once

#include <string>
#include <vector>

#include "core/ma_core.h"
#include "porting/ma_porting.h"
#include "resource.hpp"

namespace ma::server::callback {

using namespace ma;

/* Phase 1 (SSCMA fork): hardware ROI (crop-zoom) via the OV5647 INP crop.
 * Coordinates are in sensor pixels for the active sensor mode.
 * AT+ROI=0,0,0,0 disables the ROI (full sensor). */

static Sensor* findCameraSensor() {
    auto& sensors = static_resource->device->getSensors();
    auto  it      = std::find_if(sensors.begin(), sensors.end(), [](const Sensor* s) {
        return s->getType() == Sensor::Type::kCamera;
    });
    return it != sensors.end() ? *it : nullptr;
}

void setROI(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 5);
    const auto& cmd = args[0];

    uint16_t x = static_cast<uint16_t>(std::atoi(args[1].c_str()));
    uint16_t y = static_cast<uint16_t>(std::atoi(args[2].c_str()));
    uint16_t w = static_cast<uint16_t>(std::atoi(args[3].c_str()));
    uint16_t h = static_cast<uint16_t>(std::atoi(args[4].c_str()));

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensor();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

    ret = static_cast<Camera*>(sensor)->setROI(x, y, w, h);
    if (ret != MA_OK) {
        goto exit;
    }

    if (w == 0 || h == 0) {
        /* ROI disabled: persist zeros so the disabled state survives reboot.
         * (Named lvalues: MA_STORAGE_SET_POD requires an lvalue.) */
        uint16_t zx = 0, zy = 0, zw = 0, zh = 0;
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_X, zx);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_Y, zy);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_W, zw);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_H, zh);
    } else {
        /* Re-read the applied ROI: the driver rounds odd coordinates down
         * to even, so persist what is actually in effect. */
        uint16_t ax = 0, ay = 0, aw = 0, ah = 0;
        ret = static_cast<Camera*>(sensor)->getROI(ax, ay, aw, ah);
        if (ret != MA_OK) {
            goto exit;
        }
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_X, ax);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_Y, ay);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_W, aw);
        MA_STORAGE_SET_POD(ret, static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_H, ah);
    }

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

void getROI(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 1);
    const auto& cmd = args[0];

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensor();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

    {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        ret = static_cast<Camera*>(sensor)->getROI(x, y, w, h);
        if (ret != MA_OK) {
            goto exit;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%u,%u,%u,%u", x, y, w, h);
        encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
        encoder.write("roi", std::string(buf));
        encoder.end();
        transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
        return;
    }

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

void initDefaultROI(Encoder& encoder) {
    (void)encoder;

    auto* sensor = findCameraSensor();
    if (!sensor) {
        MA_LOGD(MA_TAG, "No camera sensor found, skipping ROI restore");
        return;
    }

    /* A missing key reads back as 0, which means "ROI disabled" — the
     * same as the driver default, so this is a safe no-op on first boot. */
    uint16_t x = 0, y = 0, w = 0, h = 0;
    MA_STORAGE_GET_POD(static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_X, x, 0);
    MA_STORAGE_GET_POD(static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_Y, y, 0);
    MA_STORAGE_GET_POD(static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_W, w, 0);
    MA_STORAGE_GET_POD(static_resource->device->getStorage(), MA_STORAGE_KEY_ROI_H, h, 0);

    if (w == 0 || h == 0) {
        return;
    }

    ma_err_t ret = static_cast<Camera*>(sensor)->setROI(x, y, w, h);
    if (ret != MA_OK) {
        MA_LOGW(MA_TAG, "Failed to restore persisted ROI (%u,%u,%u,%u): %d", x, y, w, h, ret);
    } else {
        MA_LOGD(MA_TAG, "Restored persisted ROI (%u,%u,%u,%u)", x, y, w, h);
    }
}

}  // namespace ma::server::callback
