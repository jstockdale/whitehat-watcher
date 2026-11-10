#pragma once

#include <stdbool.h>
#include "soc/soc_caps.h"
#include "esp_err.h"
#include "esp_io_expander.h"

#include "sscma_client_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *sscma_client_spi_bus_handle_t;  /*!< Type of SSCMA SPI bus handle */
typedef void *sscma_client_i2c_bus_handle_t;  /*!< Type of SSCMA I2C bus handle */
typedef void *sscma_client_uart_bus_handle_t; /*!< Type of SSCMA UART bus handle */

/**
 * @brief Client IO configuration structure, for SPI interface
 */
typedef struct
{
    int cs_gpio_num;   /*!< GPIO used for CS line */
    int sync_gpio_num; /*!< GPIO used for SYNC line */
    int spi_mode;
    int wait_delay;                       /*!< Traditional SPI mode (0~3) */
    unsigned int pclk_hz;                 /*!< Frequency of pixel clock */
    size_t trans_queue_depth;             /*!< Size of internal transaction queue */
    void *user_ctx;                       /*!< User private data, passed directly to on_color_trans_done's user_ctx */
    esp_io_expander_handle_t io_expander; /*!< IO expander handle */
    struct
    {
        unsigned int octal_mode : 1;        /*!< transmit with octal mode (8 data lines), this mode is used to simulate Intel 8080 timing */
        unsigned int sio_mode : 1;          /*!< Read and write through a single data line (MOSI) */
        unsigned int lsb_first : 1;         /*!< transmit LSB bit first */
        unsigned int cs_high_active : 1;    /*!< CS line is high active */
        unsigned int sync_high_active : 1;  /*!< SYNC line is high active */
        unsigned int sync_use_expander : 1; /*!< SYNC line use IO expander */
    } flags;
} sscma_client_io_spi_config_t;

/**
 * @brief Create SSCMA client IO handle, for SPI interface
 *
 * @param[in] bus SPI bus handle
 * @param[in] io_config IO configuration, for SPI interface
 * @param[out] ret_io Returned IO handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 */

esp_err_t sscma_client_new_io_spi_bus(sscma_client_spi_bus_handle_t bus, const sscma_client_io_spi_config_t *io_config, sscma_client_io_handle_t *ret_io);

/**
 * @brief Client IO configuration structure, for I2C interface
 *
 */
typedef struct
{
    uint32_t dev_addr; /*!< I2C device address */
    int wait_delay;
    void *user_ctx; /*!< User private data, passed directly to user_ctx */
} sscma_client_io_i2c_config_t;

/**
 * @brief Create SSCMA client IO handle, for I2C interface
 *
 * @param[in] bus I2C bus handle
 * @param[in] io_config IO configuration, for I2C interface
 * @param[out] ret_io Returned IO handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 */
esp_err_t sscma_client_new_io_i2c_bus(sscma_client_i2c_bus_handle_t bus, const sscma_client_io_i2c_config_t *io_config, sscma_client_io_handle_t *ret_io);

/**
 * @brief Client IO configuration structure, for uart interface
 *
 */
typedef struct
{
    void *user_ctx; /*!< User private data, passed directly to user_ctx */
} sscma_client_io_uart_config_t;

/**
 * @brief Create SSCMA client IO handle, for uart interface
 *
 * @param[in] bus UART bus handle
 * @param[in] io_config IO configuration, for uart interface
 * @param[out] ret_io Returned IO handle
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_ERR_NO_MEM        if out of memory
 *          - ESP_OK                on success
 */
esp_err_t sscma_client_new_io_uart_bus(sscma_client_uart_bus_handle_t bus, const sscma_client_io_uart_config_t *io_config, sscma_client_io_handle_t *ret_io);

/**
 * @brief Destory SSCMA client IO handle
 *
 * @param[in] io IO handle
 * @return
 *          - ESP_OK on success
 */
esp_err_t sscma_client_del_io(sscma_client_io_handle_t io);

/**
 * @brief Write data to SSCMA client IO
 *
 * @param[in] io IO handle
 * @param[in] data Data to be written
 * @param[in] size Size of data
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_OK                on success
 */
esp_err_t sscma_client_io_write(sscma_client_io_handle_t io, const void *data, size_t size);

/**
 * @brief Read data from SSCMA client IO
 *
 * @param[in] io IO handle
 * @param[in] data Data to be read
 * @param[in] size Size of data
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_OK                on success
 */
esp_err_t sscma_client_io_read(sscma_client_io_handle_t io, void *data, size_t size);

/**
 * @brief Get available size of data
 *
 * @param[in] io IO handle
 * @param[out] len Available size
 * @return
 *          - ESP_ERR_INVALID_ARG   if parameter is invalid
 *          - ESP_OK
 */
esp_err_t sscma_client_io_available(sscma_client_io_handle_t io, size_t *len);

/**
 * @brief Flush data
 *
 * @param[in] io IO handle
 * @return
 *          - ESP_OK
 */
esp_err_t sscma_client_io_flush(sscma_client_io_handle_t io);

/**
 * @brief SPI channel self-test (v0.4.18.11)
 *
 * Sends CMD_TEST_PATTERN to the Himax, which generates a deterministic
 * pattern, stages [pattern][CRC16], and the S3 verifies both data
 * integrity and CRC. Isolates SPI transport issues from camera capture.
 *
 * @param[in] io IO handle (must be SPI)
 * @param[in] len Test length in bytes (1-4094)
 * @param[in] pattern_id Pattern: 0=zeros, 1=0xFF, 2=incrementing, 3=LCG
 * @param[out] out Optional buffer to receive the pattern (len bytes), or NULL
 * @return
 *          - ESP_OK if CRC and data integrity both pass
 *          - ESP_ERR_INVALID_ARG if parameters invalid
 *          - ESP_ERR_INVALID_CRC if CRC mismatch
 *          - ESP_ERR_INVALID_RESPONSE if data integrity fails
 */
esp_err_t sscma_client_io_spi_test_pattern(sscma_client_io_handle_t io,
                                           uint16_t len, uint8_t pattern_id,
                                           uint8_t *out);

/* v0.4.18.50: Begin/end markers for spitest. Sets the test_active flag to
 * prevent background AT polling from interfering during the entire test run
 * (not just per-test-case). Call begin before the test loop, end after. */
void sscma_client_io_spi_test_begin(sscma_client_io_handle_t io);
void sscma_client_io_spi_test_end(sscma_client_io_handle_t io);

/* v0.4.50 (link hardening WI-3): read the spitest hold flag. The eyes
 * link-health supervisor checks it before running a recovery rung —
 * recovery defers while a self-test owns the bus. */
bool sscma_client_io_spi_test_active(sscma_client_io_handle_t io);

/* v0.4.50 (link hardening WI-3): queue a no-gate CMD_RESET resync on the
 * SPI worker (ladder rung 2). Serialized against AT and bulk traffic by
 * the worker queue. ESP_OK if the Himax re-armed (MISO LOW) after the
 * reset, ESP_ERR_TIMEOUT otherwise. */
esp_err_t sscma_client_io_spi_resync(sscma_client_io_handle_t io);

/* v0.4.50 (link hardening WI-2): the consecutive transport gate-failure
 * streak, owned by the SPI io layer. Counts consecutive worker
 * transactions that failed at a gate (never transmitted, or transmitted
 * but never answered at the transport layer); an AT-layer error reply
 * is a completed transaction. Implemented by the SPI transport; the
 * accessors exist so ops.c (poll summary) and eyes.c (link-health
 * supervisor, heartbeat) can read the same counter the io layer
 * maintains.
 * v0.4.51 (D1): the streak resets ONLY at exec-level completions —
 * the write-exec ESP_OK epilogue, the read-exec ESP_OK exits, the
 * available exec's wire path after the count clock-out, and the
 * bulk-exec success point. There is deliberately no exported reset:
 * the reset sites must stay enumerable (exactly four
 * gate_note_success_ call sites in the io source). */
uint32_t sscma_client_gate_streak(void);
/* Milliseconds since the current streak began (0 when the streak is 0). */
int64_t sscma_client_gate_streak_age_ms(void);
/* v0.4.51 (G1): gate failures in the trailing 60 s (slotted window,
 * 6 slots x 10 s, maintained in the io layer's failure funnel) and the
 * monotonic since-boot failure total. The window is the ladder's rate
 * trigger — it sees duty-cycle faults (the 2026-11-09 flap) that a
 * consecutive-only streak structurally cannot. */
uint32_t sscma_client_gate_failures_window(void);
uint32_t sscma_client_gate_failures_total(void);
/* Declared rung-1 drain recoveries since boot. v0.4.51 (D2): a
 * recovery is declared only on a gate-LOW drain exit whose captured
 * segment survives validation (non-padding after the trailing idle
 * run is stripped; decoded count fits the drain stash). */
uint32_t sscma_client_gate_drain_recoveries(void);
/* Drain captures discarded by the D2 validation since boot. Counted,
 * never logged per-discard (drains fire at flap cadence); surfaced
 * via the ops poll summary. */
uint32_t sscma_client_gate_drain_discards(void);

#ifdef __cplusplus
}
#endif
