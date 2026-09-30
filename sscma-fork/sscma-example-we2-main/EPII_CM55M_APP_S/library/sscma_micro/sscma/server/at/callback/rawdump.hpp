#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/ma_core.h"
#include "porting/ma_porting.h"
#include "resource.hpp"

namespace ma::server::callback {

using namespace ma;

/* Phase 3 (SSCMA fork): WDMA2 raw format probe.
 * AT+RAWDUMP captures one frame via Sensor->INP->WDMA2 at the current INP
 * output size and reports the measured byte count plus content statistics.
 * The byte count classifies the pixel packing:
 *   bytes == w*h      -> 1 byte/px (e.g. RAW8)
 *   bytes == w*h*1.25 -> packed RAW10
 *   bytes == w*h*2    -> 16-bit unpacked
 * See phase3-rawdump-design.md. */

static Sensor* findCameraSensorForRawDump() {
    auto& sensors = static_resource->device->getSensors();
    auto  it      = std::find_if(sensors.begin(), sensors.end(), [](const Sensor* s) {
        return s->getType() == Sensor::Type::kCamera;
    });
    return it != sensors.end() ? *it : nullptr;
}

void rawDump(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 1);
    const auto& cmd = args[0];

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensorForRawDump();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

    {
        Camera::raw_dump_result_t result = {};
        ret = static_cast<Camera*>(sensor)->rawDump(result);
        if (ret != MA_OK) {
            goto exit;
        }

        char hex[129];
        for (int i = 0; i < 64; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", result.head[i]);
        }

        char hist[192];
        int  off = 0;
        for (int i = 0; i < 16; ++i) {
            off += std::snprintf(hist + off, sizeof(hist) - off, "%s%lu",
                                 i ? "," : "", (unsigned long)result.hist[i]);
        }

        encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
        encoder.write("width", static_cast<uint32_t>(result.width));
        encoder.write("height", static_cast<uint32_t>(result.height));
        encoder.write("bytes", result.bytes_written);
        encoder.write("head_hex", std::string(hex));
        encoder.write("hist", std::string(hist));
        encoder.write("min", static_cast<uint32_t>(result.min_val));
        encoder.write("max", static_cast<uint32_t>(result.max_val));
        encoder.write("capture_ms", result.capture_ms);
        encoder.end();
        transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
        return;
    }

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

}  // namespace ma::server::callback
