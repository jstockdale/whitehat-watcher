#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/ma_core.h"
#include "porting/ma_porting.h"
#include "resource.hpp"

#include <ma_transport_spi.h>
#include "ma_camera_himax.h"
#include "watcher_sha256.h"

namespace ma::server::callback {

using namespace ma;

/* v0.4.14 (P1): SPI bulk frame dump.
 * AT+SPIDUMP captures one frame via Sensor->INP->WDMA2 at the current INP
 * output size (same path as AT+RAWDUMP), hashes it with SHA-256, stages the
 * capture buffer for the SPI bulk feeder (ma::spi_bulk_prepare), and
 * reports {len, sha256}. The S3 then sends CMD_START and drains the frame
 * with CMD_READ_CRC (per-chunk CRC16), verifying the SHA-256 end-to-end.
 * v0.4.15 (P4): also reports {width, height} so the S3 can size the PNG
 * encode / strip reassembly for crop windows.
 * Reply: {"type":0,"name":"SPIDUMP","code":0,
 *         "data":{"len":1259712,"sha256":"...","width":2592,"height":486}} */

static Sensor* findCameraSensorForSpiDump() {
    auto& sensors = static_resource->device->getSensors();
    auto  it      = std::find_if(sensors.begin(), sensors.end(), [](const Sensor* s) {
        return s->getType() == Sensor::Type::kCamera;
    });
    return it != sensors.end() ? *it : nullptr;
}

void spiDump(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 1);
    const auto& cmd = args[0];

    ma_err_t ret    = MA_OK;
    auto*    sensor = findCameraSensorForSpiDump();
    if (!sensor) {
        ret = MA_ENOENT;
        goto exit;
    }

    {
        auto* cam = static_cast<CameraHimax*>(sensor);
        /* v0.4.18: ensure a raw capture buffer exists. The camera boots in
         * preset 3 (480x480 JPEG) which allocates no raw buffer; SPIDUMP
         * needs the 1296x972 RAW8 preset (preset 8). If getRawCaptureBuffer
         * fails and no crop window is active, switch to preset 8. We stay
         * in preset 8 afterwards (capture-only mode: the S3 is the only
         * client and always wants full-res raw). */
        const uint8_t* probe_buf = nullptr;
        size_t         probe_len = 0;
        if (cam->getRawCaptureBuffer(&probe_buf, &probe_len, nullptr) != MA_OK) {
            cam->deInit();
            ret = cam->init(8); /* 1296x972 RAW8 */
            if (ret != MA_OK) {
                goto exit;
            }
        }

        /* Fresh capture (same path as RAWDUMP). */
        Camera::raw_dump_result_t result = {};
        ret = static_cast<Camera*>(sensor)->rawDump(result);
        if (ret != MA_OK) {
            goto exit;
        }

        const uint8_t* buf  = nullptr;
        size_t         blen = 0;
        ret = cam->getRawCaptureBuffer(&buf, &blen, nullptr);
        if (ret != MA_OK || buf == nullptr || blen == 0) {
            goto exit;
        }

        /* Hash the frame for the S3's end-to-end verification. */
        uint8_t digest[32];
        watcher_sha256(buf, blen, digest);
        char hex[65];
        for (int i = 0; i < 32; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }

        /* Stage the buffer for the bulk feeder; the S3 arms it with
         * CMD_START after parsing this reply. */
        ma::spi_bulk_prepare(buf, blen);

        encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
        encoder.write("len", static_cast<uint32_t>(blen));
        encoder.write("sha256", std::string(hex));
        encoder.write("width", result.width);
        encoder.write("height", result.height);
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
