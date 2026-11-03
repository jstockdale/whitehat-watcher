#ifndef _MA_TRANSPORT_SPI_H_
#define _MA_TRANSPORT_SPI_H_

#include <porting/ma_transport.h>

namespace ma {

class SPI final : public Transport {
   public:
    SPI();
    ~SPI();

    ma_err_t init(const void* config) noexcept override;
    void     deInit() noexcept override;

    size_t available() const noexcept override;
    size_t send(const char* data, size_t length) noexcept override;
    size_t flush() noexcept override;
    size_t receive(char* data, size_t length) noexcept override;
    size_t receiveIf(char* data, size_t length, char delimiter) noexcept override;
};

/* v0.4.6.1: thread-context SPI housekeeping — deferred DMA diagnostics
 * and RX re-arm retry. Call periodically from the AT-server main loop. */
void spi_poll();

/* v0.4.14 (P1): stage a capture buffer for the bulk feeder (AT+SPIDUMP).
 * The buffer must stay valid until the S3 drains it. */
void spi_bulk_prepare(const uint8_t* buf, size_t len);

}  // namespace ma

#endif
