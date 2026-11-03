#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sscma_client_io_t sscma_client_io_t; /*!< Type of SSCMA client IO */

/**
 * @brief SSCMA IO interface
 */
struct sscma_client_io_t
{
    /**
     * @brief Bus handle
     */
    void *handle;

    /**
     * @brief Destory SCCMA client
     *
     * @param[in] io SCCMA client handle]
     * @return
     *          - ESP_OK on success
     */
    esp_err_t (*del)(sscma_client_io_t *io);

    /**
     * @brief Write data to SCCMA client
     *
     * @param[in] io SCCMA client handle
     * @param[in] data Data to be written
     * @param[in] size Size of data
     * @return
     *          - ESP_ERR_INVALID_ARG   if parameter is invalid
     *          - ESP_ERR_NOT_SUPPORTED if read is not supported by transport
     *          - ESP_OK                on success
     */
    esp_err_t (*write)(sscma_client_io_t *io, const void *data, size_t size);

    /**
     * @brief Read data from SCCMA client
     *
     * @param[in] io SCCMA client IO handle
     * @param[in] data Data to be read
     * @param[in] size Size of data
     * @return
     *          - ESP_ERR_INVALID_ARG   if parameter is invalid
     *          - ESP_ERR_NOT_SUPPORTED if read is not supported by transport
     *          - ESP_OK                on success
     *
     */
    esp_err_t (*read)(sscma_client_io_t *io, void *data, size_t size);

    /**
     * @brief Get available size of data
     *
     * @param[in] io SCCMA client IO handle
     * @param[out] ret_avail Available size
     * @return
     *          - ESP_ERR_INVALID_ARG   if parameter is invalid
     *          - ESP_ERR_NOT_SUPPORTED if read is not supported by transport
     *          - ESP_OK
     */
    esp_err_t (*available)(sscma_client_io_t *io, size_t *ret_avail);

    /**
     * @brief Flush data
     *
     * @param[in] io IO handle
     * @return
     *          - ESP_OK
     */
    esp_err_t (*flush)(sscma_client_io_t *io);

    /**
     * @brief Bulk-read `size` bytes in one bus transaction sequence
     *
     * Optional fast path for large transfers (e.g. Himax SPI frame dumps).
     * The transport holds its bus lock for the whole transfer and must not
     * interleave AT traffic. NULL = not supported; the ops layer falls back
     * to the chunked available()/read() loop.
     *
     * @param[in] io SCCMA client IO handle
     * @param[in] data Destination buffer
     * @param[in] size Bytes to transfer
     * @return
     *          - ESP_ERR_INVALID_ARG   if parameter is invalid
     *          - ESP_ERR_NOT_SUPPORTED if bulk read is not supported
     *          - ESP_ERR_INVALID_CRC   on checksum mismatch
     *          - ESP_ERR_TIMEOUT       on transfer timeout
     *          - ESP_OK                on success
     */
    esp_err_t (*read_bulk)(sscma_client_io_t *io, void *data, size_t size);
};

#ifdef __cplusplus
}
#endif
