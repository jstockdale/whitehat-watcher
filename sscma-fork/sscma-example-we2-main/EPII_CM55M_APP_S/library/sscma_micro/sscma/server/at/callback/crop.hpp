#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/ma_core.h"
#include "porting/ma_porting.h"
#include "resource.hpp"

#include "ma_camera_himax.h"

namespace ma::server::callback {

using namespace ma;

/* v0.4.15 (P4/P5): one-shot native-resolution crop window.
 * AT+CROP=x,y,w,h enters full-res mode: the sensor is programmed for
 * 2592x1944 and the INP captures the window 1:1 (see
 * CameraHimax::setCropWindow). Coordinates must be even (Bayer phase),
 * inside 2592x1944, and w*h <= CROP_WINDOW_MAX_BYTES (1,259,712).
 * AT+CROP=0,0,0,0 clears the window and restores the 1296x972 raw preset.
 * AT+CROP? reports the active window (all zero = none).
 * Unlike AT+ROI, the window is NOT persisted and does NOT survive reboot.
 * Typical P5 strip flow: AT+CROP=0,<y0>,2592,486 -> AT+SPIDUMP (x N).
 * Same-size window moves take the cheap INP-recrop path on-device.
 * Reply: {"type":0,"name":"CROP","code":0,
 *         "data":{"x":0,"y":486,"w":2592,"h":486}} */

static Sensor* findCameraSensorForCrop() {
    auto& sensors = static_resource->device->getSensors();
    auto  it      = std::find_if(sensors.begin(), sensors.end(), [](const Sensor* s) {
        return s->getType() == Sensor::Type::kCamera;
    });
    return it != sensors.end() ? *it : nullptr;
}

void setCrop(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 1);
    const auto& cmd = args[0];

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensorForCrop();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

#ifdef CAPTURE_ONLY_MODE
    {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        if (args.size() >= 5) {
            x = static_cast<uint16_t>(std::atoi(args[1].c_str()));
            y = static_cast<uint16_t>(std::atoi(args[2].c_str()));
            w = static_cast<uint16_t>(std::atoi(args[3].c_str()));
            h = static_cast<uint16_t>(std::atoi(args[4].c_str()));
        }

        auto* cam = static_cast<CameraHimax*>(sensor);
        if (w == 0 || h == 0) {
            ret = cam->clearCropWindow();
        } else {
            ret = cam->setCropWindow(x, y, w, h);
        }
        if (ret != MA_OK) {
            goto exit;
        }

        ret = cam->getCropWindow(x, y, w, h);
        if (ret != MA_OK) {
            goto exit;
        }

        encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
        encoder.write("x", x);
        encoder.write("y", y);
        encoder.write("w", w);
        encoder.write("h", h);
        encoder.end();
        transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
        return;
    }
#else
    ret = MA_ENOTSUP;
    goto exit;
#endif

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

void getCrop(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 1);
    const auto& cmd = args[0];

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensorForCrop();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

#ifdef CAPTURE_ONLY_MODE
    {
        uint16_t x = 0, y = 0, w = 0, h = 0;
        ret = static_cast<CameraHimax*>(sensor)->getCropWindow(x, y, w, h);
        if (ret != MA_OK) {
            goto exit;
        }

        encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
        encoder.write("x", x);
        encoder.write("y", y);
        encoder.write("w", w);
        encoder.write("h", h);
        encoder.end();
        transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
        return;
    }
#else
    ret = MA_ENOTSUP;
    goto exit;
#endif

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

}  // namespace ma::server::callback
