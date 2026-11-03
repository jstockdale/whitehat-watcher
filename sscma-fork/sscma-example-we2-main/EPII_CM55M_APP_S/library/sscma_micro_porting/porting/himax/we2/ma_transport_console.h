#ifndef _MA_TRANSPORT_CONSOLE_H_
#define _MA_TRANSPORT_CONSOLE_H_

#include <porting/ma_transport.h>

namespace ma {

class Console final : public Transport {
   public:
    Console() noexcept;
    ~Console() noexcept;

    ma_err_t init(const void* config) noexcept override;
    void     deInit() noexcept override;

    size_t available() const noexcept override;
    size_t send(const char* data, size_t length) noexcept override;
    size_t flush() noexcept override;
    size_t receive(char* data, size_t length) noexcept override;
    size_t receiveIf(char* data, size_t length, char delimiter) noexcept override;
};

}  // namespace ma

/* v0.4.20.7: Synchronized direct-UART print. Waits for in-flight Console TX
 * DMA to drain, then writes directly to the UART. Use for boot-time DIAG
 * lines that must not interleave with AT JSON responses (which use DMA).
 * Declared extern "C" for use from C and C++ TUs. */
#ifdef __cplusplus
extern "C" {
#endif
void ma_console_print_sync(const char* str);
#ifdef __cplusplus
}
#endif

#endif