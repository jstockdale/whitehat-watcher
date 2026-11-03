#pragma once

#include <string>
#include <vector>
#include <cstdio>

#include "core/ma_core.h"
#include "porting/ma_porting.h"
#include "resource.hpp"
#include "rawdump.hpp"

#include <core/utils/ma_base64.h>
#include "ma_camera_himax.h"

namespace ma::server::callback {

using namespace ma;

#ifdef CAPTURE_ONLY_MODE

/* v0.4.5: AT+RAWREAD=<offset>,<len> — chunked bulk transfer of the full-frame
 * raw capture buffer.
 *
 * A 1296x972 RAW8 frame is 1,259,712 bytes: far too large for a single AT
 * response (the SPI TX ring is 4096 bytes), so the host pulls it in chunks:
 *
 *   AT+SENSOR=0,1,8        select the 1296x972 RAW8 preset (preset 8)
 *   AT+RAWDUMP             capture one frame into the registered raw buffer
 *   AT+RAWREAD=0,2048      first chunk
 *   AT+RAWREAD=2048,2048   ...
 *   ... until offset+len >= total
 *
 * Each response is JSON: {"offset":O,"len":L,"total":T,"bits":8,"b64":"..."}.
 * <len> is capped at RAWREAD_MAX_CHUNK (2048) so the base64 response stays
 * under ~3KB and always fits the 4096-byte SPI TX ring. `total` is the full
 * registered buffer size; the host reassembles exactly `total` bytes and
 * validates the frame (e.g. by checksum) on its side.
 */

#define RAWREAD_MAX_CHUNK 2048

#ifdef ARMCM55
/* Cortex-M55 DWT cycle counter — cycle-accurate, no OS dependency.
 * FreeRTOS Tick::current() has 1 ms resolution here (configTICK_RATE_HZ=1000)
 * and Tick::toMicroseconds() is not implemented on the FreeRTOS OSAL, so the
 * DWT is the only way to time a sub-millisecond base64 encode. */
#define MEMDBG_DWT_CTRL (*reinterpret_cast<volatile uint32_t*>(0xE0001000UL))
#define MEMDBG_DWT_CYCCNT (*reinterpret_cast<volatile uint32_t*>(0xE0001004UL))
#define MEMDBG_DEMCR (*reinterpret_cast<volatile uint32_t*>(0xE000EDFCUL))
static inline void memdbg_dwt_init() {
    MEMDBG_DEMCR |= (1UL << 24); /* TRCENA: enable trace/DWT */
    MEMDBG_DWT_CTRL |= 1UL;      /* CYCCNTENA: start cycle counter */
}
static inline uint32_t memdbg_cycles() { return MEMDBG_DWT_CYCCNT; }

/* HX6538 SRAM regions (WE2_device_addr.h). Classifies an address so the host
 * can see exactly where the capture buffer and each read window land. */
static const char* memdbg_region_name(uintptr_t a) {
    if (a >= 0x34000000UL && a < 0x34100000UL) return "SRAM0A";
    if (a >= 0x34100000UL && a < 0x34200000UL) return "SRAM1A";
    if (a >= 0x36000000UL && a < 0x36060000UL) return "SRAM2A";
    if (a >= 0x30000000UL && a < 0x30040000UL) return "DTCMA";
    if (a >= 0x24000000UL && a < 0x24100000UL) return "SRAM0";
    if (a >= 0x24100000UL && a < 0x24200000UL) return "SRAM1";
    if (a >= 0x26000000UL && a < 0x26060000UL) return "SRAM2";
    if (a >= 0x20000000UL && a < 0x20040000UL) return "DTCM";
    return "OTHER";
}
#endif /* ARMCM55 */

void rawRead(const std::vector<std::string>& args, Transport& transport, Encoder& encoder) {
    MA_ASSERT(args.size() >= 3);
    const auto& cmd = args[0];

    ma_err_t ret = MA_OK;

    uint32_t offset  = static_cast<uint32_t>(std::atoi(args[1].c_str()));
    uint32_t req_len = static_cast<uint32_t>(std::atoi(args[2].c_str()));

    const uint8_t* buf      = nullptr;
    size_t         buf_size = 0;
    uint8_t        bits     = 0;

    auto* sensor = findCameraSensorForRawDump();
    if (sensor == nullptr) {
        ret = MA_ENOENT;
        goto exit;
    }

    {
        /* The only camera sensor on this board is a CameraHimax. */
        auto* cam = static_cast<CameraHimax*>(sensor);
        ret       = cam->getRawCaptureBuffer(&buf, &buf_size, &bits);
        if (ret != MA_OK) {
            goto exit;
        }
    }

    if (req_len == 0 || req_len > RAWREAD_MAX_CHUNK) {
        req_len = RAWREAD_MAX_CHUNK;
    }
    if (offset >= buf_size) {
        ret = MA_EINVAL;
        goto exit;
    }

    {
        uint32_t avail = static_cast<uint32_t>(buf_size - offset);
        uint32_t chunk = req_len < avail ? req_len : avail;

        /* v0.4.9 instrumentation: time the base64 encode with the DWT cycle
         * counter to detect slow memory regions (cycle count / 400 = us at
         * 400 MHz core clock). */
#ifdef ARMCM55
        memdbg_dwt_init();
        uint32_t cyc0 = memdbg_cycles();
#endif

        /* base64: 4 output chars per 3 input bytes, no NUL written. */
        char b64[4 * ((RAWREAD_MAX_CHUNK + 2) / 3) + 1];
        int  out_len = static_cast<int>(sizeof(b64) - 1);
        ret          = ma::utils::base64_encode(buf + offset, static_cast<int>(chunk), b64, &out_len);

#ifdef ARMCM55
        uint32_t cyc1         = memdbg_cycles();
        uint32_t encode_cycles = cyc1 - cyc0;
#endif

        if (ret != MA_OK) {
            goto exit;
        }
        b64[out_len] = '\0';

        /* v0.4.9: report buffer base/end and the read window so the host can
         * check buf+offset against the HX6538 memory map for SRAM-bank
         * boundary crossings (e.g. SRAM0A -> SRAM1A at 0x34100000). */
        uintptr_t buf_base = (uintptr_t)buf;
        uintptr_t win0     = buf_base + offset;
        uintptr_t win1     = win0 + chunk; /* one past the last byte read */
        char      addr_str[4][19];
        snprintf(addr_str[0], sizeof(addr_str[0]), "0x%08lx", (unsigned long)buf_base);
        snprintf(addr_str[1], sizeof(addr_str[1]), "0x%08lx", (unsigned long)(buf_base + buf_size));
        snprintf(addr_str[2], sizeof(addr_str[2]), "0x%08lx", (unsigned long)win0);
        snprintf(addr_str[3], sizeof(addr_str[3]), "0x%08lx", (unsigned long)win1);
        /* xbound: read window straddles the SRAM0A/SRAM1A 1 MB line. */
        uint32_t xbound = (win0 < 0x34100000UL && win1 > 0x34100000UL) ? 1u : 0u;

        encoder.begin(MA_MSG_TYPE_RESP, MA_OK, cmd);
        encoder.write("offset", offset);
        encoder.write("len", chunk);
        encoder.write("total", static_cast<uint32_t>(buf_size));
        encoder.write("bits", static_cast<uint32_t>(bits));
        encoder.write("b64", std::string(b64, static_cast<size_t>(out_len)));
        encoder.write("buf_addr", std::string(addr_str[0]));
        encoder.write("buf_end", std::string(addr_str[1]));
        encoder.write("win_addr", std::string(addr_str[2]));
        encoder.write("win_end", std::string(addr_str[3]));
        encoder.write("xbound", xbound);
#ifdef ARMCM55
        encoder.write("buf_region", std::string(memdbg_region_name(buf_base)));
        encoder.write("win_region", std::string(memdbg_region_name(win0)));
        encoder.write("encode_cycles", encode_cycles);
#endif
        encoder.end();

        size_t sent = transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
        if (sent != encoder.size()) {
            /* The TX ring (4096 bytes on SPI) was fuller than expected; the
             * response was truncated. The host must retry the chunk. */
            MA_LOGE(MA_TAG, "RAWREAD: short send %u/%u (offset %lu)",
                    (unsigned)sent, (unsigned)encoder.size(), (unsigned long)offset);
        }
        return;
    }

exit:
    encoder.begin(MA_MSG_TYPE_RESP, ret, cmd);
    encoder.end();
    transport.send(reinterpret_cast<const char*>(encoder.data()), encoder.size());
}

#endif  // CAPTURE_ONLY_MODE

}  // namespace ma::server::callback
