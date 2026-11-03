#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#if CONFIG_SSCMA_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "sscma_client_io_interface.h"
#include "sscma_client_io.h"
#include "driver/spi_master.h"
#include "esp_io_expander.h"
/* v0.4.18.31 (Phase 2): For Himax TX-ready GPIO signaling via PCA9535 P07.
 * The eyes component owns the expander handle. */
extern esp_io_expander_handle_t eyes_io_expander(void);
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

/* v0.4.18.36: Forward declaration for MISO-flag helper (defined below).
 * Used by client_io_spi_write for AT commands over SPI. */
static bool spi_miso_wait_level_(int target_level, uint32_t timeout_ms, const char *what);

/* v0.4.18.32 (Phase 2): GPIO2 interrupt for Himax ready signaling.
 * Himax PB8 -> PCA9535 P07 -> PCA9535 INT (open-drain, active-low) -> ESP_GPIO2.
 * When P07 changes, the expander pulls INT low, firing the GPIO2 ISR.
 * The ISR gives a semaphore; the SPI task waits on it, then reads P07 via I2C
 * to confirm (filters out INTs from other expander pins). */
#define HIMAX_READY_GPIO 2
/* v0.4.18.35: MISO-flag handshake replaces the PB8/expander path.
 * MISO (GPIO6) is the signal: LOW = RX armed, HIGH = TX ready.
 * The pull-up is enabled in eyes.c after SPI bus init. */
static SemaphoreHandle_t himax_int_sem_ = NULL;
static bool himax_gpio_init_ = false;

static void IRAM_ATTR himax_ready_isr_(void *arg)
{
    (void)arg;
    BaseType_t higher_woken = pdFALSE;
    if (himax_int_sem_ != NULL) {
        xSemaphoreGiveFromISR(himax_int_sem_, &higher_woken);
    }
    if (higher_woken) {
        portYIELD_FROM_ISR();
    }
}

static void himax_ready_gpio_init_(void)
{
    if (himax_gpio_init_) {
        return;
    }
    himax_int_sem_ = xSemaphoreCreateBinary();
    if (himax_int_sem_ == NULL) {
        return;
    }
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE, /* Expander INT is active-low */
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << HIMAX_READY_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* Open-drain needs pull-up */
    };
    if (gpio_config(&io_conf) != ESP_OK) {
        return;
    }
    if (gpio_install_isr_service(0) != ESP_OK) {
        /* May already be installed; not fatal. */
    }
    if (gpio_isr_handler_add(HIMAX_READY_GPIO, himax_ready_isr_, NULL) != ESP_OK) {
        return;
    }
    himax_gpio_init_ = true;
}

static const char *TAG = "sscma_client.io.spi";

/* Log the real errno plus DMA-heap state before bailing on a transmit
 * failure. The stock ESP_GOTO_ON_ERROR message hides the errno, which made
 * the 0.4.0/0.4.1 camera wedge undiagnosable (suspected ESP_ERR_NO_MEM
 * from the per-transaction DMA bounce buffer the IDF SPI driver allocates
 * for PSRAM rx buffers). */
#define SPI_XMIT_GOTO_ON_ERROR(_ret, _label) do { \
    if ((_ret) != ESP_OK) { \
        ESP_LOGE(TAG, "spi transmit (queue) failed: %s; " \
                 "dma_heap_free=%u largest=%u", \
                 esp_err_to_name(_ret), \
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), \
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA)); \
        goto _label; \
    } \
} while (0)

#define HEADER_LEN   (uint8_t)4
#define MAX_PL_LEN   (uint8_t)250
#define CHECKSUM_LEN (uint8_t)2

#define PACKET_SIZE (uint16_t)(HEADER_LEN + MAX_PL_LEN + CHECKSUM_LEN)

#define MAX_RECIEVE_SIZE (uint16_t)4095

#define FEATURE_TRANSPORT               0x10
#define FEATURE_TRANSPORT_CMD_READ      0x01
#define FEATURE_TRANSPORT_CMD_WRITE     0x02
#define FEATURE_TRANSPORT_CMD_AVAILABLE 0x03
#define FEATURE_TRANSPORT_CMD_START     0x04
#define FEATURE_TRANSPORT_CMD_STOP      0x05
#define FEATURE_TRANSPORT_CMD_RESET     0x06
/* v0.4.14 (P1): CRC-protected bulk read. Like CMD_READ but the Himax
 * stages [data][CRC16] and the S3 clocks len+2 bytes. */
#define FEATURE_TRANSPORT_CMD_READ_CRC  0x07
/* v0.4.18.11: SPI channel self-test. See Himax ma_transport_spi.cpp
 * FEATURE_TRANSPORT_CMD_TEST_PATTERN (0x08). */
#define FEATURE_TRANSPORT_CMD_TEST_PATTERN 0x08
/* v0.4.18.59 (Claude PARK spec): Park the MISO flag for SD card use.
 * No-response command. Himax re-arms RX but leaves PB2 released
 * (tristate → pull-up HIGH). S3 can safely run SD transactions on SPI2.
 * Resume: send AVAILABLE (on faith, no LOW gate while parked) with
 * 50ms timeout + one retry, fallback to RESET resync. */
#define FEATURE_TRANSPORT_CMD_PARK      0x09

/* v0.4.14 (P1): WRITE payload cap for CRC mode. The Himax RX path has a
 * stable 2-byte spill (bytes 254-255 of a command packet are lost when the
 * command follows a response read), so a 250-byte payload would push the
 * CRC trailer into the lost region. At 248 bytes the CRC at [252],[253]
 * always survives and the Himax can verify every WRITE. */
#define MAX_PL_LEN_CRC (uint8_t)248

/* v0.4.14 (P1): bulk-transfer tunables. */
#define BULK_MAX_CHUNK      (uint16_t)4090 /* v0.4.18.26: revert to 4090. Dummy bytes removed; cs_ena_pretrans handles TX startup. xfer=len+2(CRC) fits spi_trans_max_bytes=4092 */
#define BULK_CRC_LEN        2
/* v0.4.23 (Track A1): escalating empty-poll backoff in
 * spi_bulk_poll_available_ — 1 ms for the first BULK_POLL_FAST_TRIES
 * empty polls, then 10 ms, under a ~3 s total budget. Replaces the
 * flat 100 ms sleep (was BULK_POLL_TIMEOUT_MS x BULK_POLL_RETRIES),
 * which taxed every feeder hiccup at 100 ms a tick. */
#define BULK_POLL_BUDGET_MS   3000  /* total empty-poll budget per AVAILABLE call */
#define BULK_POLL_FAST_TRIES  20    /* empty polls at 1 ms before escalating */
#define BULK_POLL_FAST_MS     1
#define BULK_POLL_SLOW_MS     10
/* v0.4.23 (Track A2): bound on consecutive below-threshold AVAILABLE
 * results in the drain loop before declaring the feeder stuck. */
#define BULK_BELOW_MAX        3000  /* 3000 x 1 ms = ~3 s */
#define BULK_MAX_ATTEMPTS   3      /* whole-transfer attempts (RESET+restart) */

/* v0.4.23 (Step 0.3): per-transfer bulk statistics. Zeroed at the start
 * of spi_txn_bulk_exec and updated by the poll/read/wait helpers while
 * bulk_stats_active_ is set (bulk executes as one serialized worker
 * txn, so every MISO wait inside the window belongs to the transfer).
 * Logged as a single bulk_stats line when the transfer ends. */
typedef struct {
    uint32_t avail_polls;   /* AVAILABLE calls issued by the drain loop */
    uint32_t avail_empty;   /* empty polls inside spi_bulk_poll_available_ */
    uint32_t avail_small;   /* drain-loop polls with 0 < avail < threshold */
    uint32_t reads_full;    /* READ_CRC of a full BULK_MAX_CHUNK */
    uint32_t reads_runt;    /* READ_CRC issued below threshold (non-tail) */
    uint32_t reads_tail;    /* READ_CRC of the true final partial chunk */
    uint32_t crc_fail;      /* chunk reads rejected on CRC */
    uint64_t wait_low_us;   /* accumulated spi_miso_wait_level_(LOW) time */
    uint64_t wait_high_us;  /* accumulated spi_miso_wait_level_(HIGH) time */
} bulk_stats_t;
static bulk_stats_t bulk_stats_;
static bool bulk_stats_active_ = false;

static void bulk_stats_note_wait_(int target_level, int64_t t0_us)
{
    if (!bulk_stats_active_) {
        return;
    }
    uint64_t dt = (uint64_t)(esp_timer_get_time() - t0_us);
    if (target_level == 0) {
        bulk_stats_.wait_low_us += dt;
    } else {
        bulk_stats_.wait_high_us += dt;
    }
}

/* CRC16-CCITT (poly 0x1021, init 0xFFFF). Mirrored in the Himax firmware
 * (ma_transport_spi.cpp); both directions of the v0.4.14 protocol use it:
 * S3->Himax command packets (header+payload) and Himax->S3 bulk data. */
static uint16_t crc16_ccitt_(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t client_io_spi_del(sscma_client_io_t *io);
static esp_err_t client_io_spi_write(sscma_client_io_t *io, const void *data, size_t len);
static esp_err_t client_io_spi_read(sscma_client_io_t *io, void *data, size_t len);
static esp_err_t client_io_spi_available(sscma_client_io_t *io, size_t *len);
static esp_err_t client_io_spi_flush(sscma_client_io_t *io);
static esp_err_t client_io_spi_read_bulk(sscma_client_io_t *io, void *data, size_t size);
/* Forward declarations for gated primitives (defined later in file) */

/* v0.4.18.53 (Phase B): Dedicated SPI worker task.
 * All SPI transactions go through a queue to a high-priority worker.
 * This makes each transaction atomic from the Himax's perspective —
 * no preemption between wait-HIGH and clock. */
typedef enum {
    SPI_TXN_AVAILABLE,   // Get 2-byte available count
    SPI_TXN_READ,        // Read N bytes (AT response)
    SPI_TXN_WRITE,       // Write AT command
    SPI_TXN_BULK,        // Full bulk transfer: START→READ_CRC×N→STOP (atomic)
} spi_txn_type_t;

#define SPI_TXN_POOL_SIZE 8

typedef struct {
    spi_txn_type_t type;
    union {
        struct { size_t *out_len; } available;
        struct { void *buf; size_t len; } read;
        struct { const void *buf; size_t len; } write;
        struct { void *buf; size_t len; } bulk;
    } params;
    SemaphoreHandle_t done;
    esp_err_t result;
    volatile bool in_use;
} spi_txn_t;

/* Static pool — no heap allocation, no double-free. */
static spi_txn_t s_txn_pool[SPI_TXN_POOL_SIZE];
static SemaphoreHandle_t s_txn_pool_lock = NULL;

#define SPI_WORKER_QUEUE_LEN 8
#define SPI_WORKER_PRIORITY 15
#define SPI_WORKER_STACK_SIZE 8192

typedef struct
{
    sscma_client_io_t base;
    spi_device_handle_t spi_dev;          // SPI bus id, indicating which SPI port
    int sync_gpio_num;                    // D/C line GPIO number
    size_t spi_trans_max_bytes;           // SPI transaction max bytes
    int wait_delay;                       // SPI wait delay
    void *user_ctx;                       // User context
    esp_io_expander_handle_t io_expander; // IO expander
    SemaphoreHandle_t lock;               // Lock
    uint8_t buffer[PACKET_SIZE];
    /* Persistent internal DMA staging buffer for SPI reads. The IDF SPI
     * driver allocates a transient 4 KB MALLOC_CAP_DMA bounce buffer for
     * every transaction whose rx_buffer lives in PSRAM; under DMA-heap
     * pressure that allocation fails (ESP_ERR_NO_MEM) and the transfer
     * errors out. Reading into this internal staging buffer and memcpy'ing
     * to PSRAM eliminates the per-transaction allocation. Sized to
     * spi_trans_max_bytes (the loop below never exceeds it per chunk). */
    uint8_t *dma_stage;
    /* v0.4.18.49: Flag to prevent background AT polling during spitest.
     * The spitest drives the SPI bus directly; if the background process
     * task calls available() and sees HIGH (test pattern ready), it will
     * try to read and interfere. Set true during test_pattern, false after.
     * Checked in client_io_spi_available(). */
    /* v0.4.18.51: test_active as counter. test_begin() increments,
     * test_end() decrements. test_pattern() does NOT touch it (was clearing
     * the run-held flag after test #1, defeating the purpose). */
    volatile int test_active;
    /* v0.4.18.53: SPI worker task queue and handle. */
    QueueHandle_t txn_queue;
    TaskHandle_t worker_task;
} sscma_client_io_spi_t;
/* Forward declarations for gated primitives (defined later) */
static esp_err_t spi_send_cmd_(sscma_client_io_spi_t *spi_client_io, uint8_t cmd, uint16_t len);
static bool spi_miso_wait_level_(int target_level, uint32_t timeout_ms, const char *what);
/* v0.4.18.53: SPI worker task and internal executors. */
static void spi_worker_task(void *arg);
static esp_err_t spi_txn_available_exec(sscma_client_io_spi_t *spi_client_io, size_t *out_len);
static esp_err_t spi_txn_read_exec(sscma_client_io_spi_t *spi_client_io, void *buf, size_t len);
static esp_err_t spi_txn_write_exec(sscma_client_io_spi_t *spi_client_io, const void *buf, size_t len);
static esp_err_t spi_txn_bulk_exec(sscma_client_io_spi_t *spi_client_io, void *buf, size_t len);

esp_err_t sscma_client_new_io_spi_bus(sscma_client_spi_bus_handle_t bus, const sscma_client_io_spi_config_t *io_config, sscma_client_io_handle_t *ret_io)
{
#if CONFIG_SSCMA_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    sscma_client_io_spi_t *spi_client_io = NULL;
    ESP_GOTO_ON_FALSE(io_config && ret_io, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    spi_client_io = (sscma_client_io_spi_t *)calloc(1, sizeof(sscma_client_io_spi_t));
    ESP_GOTO_ON_FALSE(spi_client_io, ESP_ERR_NO_MEM, err, TAG, "no mem for spi client io");

    spi_device_interface_config_t dev_config = {
        .flags = (io_config->flags.lsb_first ? SPI_DEVICE_TXBIT_LSBFIRST : 0) | (io_config->flags.sio_mode ? SPI_DEVICE_3WIRE : 0) | (io_config->flags.cs_high_active ? SPI_DEVICE_POSITIVE_CS : 0),
        .clock_speed_hz = io_config->pclk_hz,
        .mode = io_config->spi_mode,
        .spics_io_num = io_config->cs_gpio_num,
        .queue_size = 1,
        /* v0.4.18.56 (doc verification §5): cs_ena_pretrans correction.
         * Was 192, comment claimed "192 cycles @ 12MHz = 16us". Wrong on
         * both counts: (1) S3 truncates (val-1) into 5-bit SPI_CS_SETUP_TIME,
         * so 191 mod 32 = 31 → 32 SCLK cycles, not 192; (2) real SCLK is
         * 80/7 ≈ 11.43 MHz, so 32 cycles ≈ 2.8 µs, not 16 µs. The old value
         * only worked by mod-32 luck (193 would wrap to 1 cycle). Set 32
         * explicitly = hardware max, same effect, no luck involved.
         * Hardware delay between CS assert and first clock gives the Himax
         * slave TX DMA time to drive MISO. Applies in full duplex on S3. */
        .cs_ena_pretrans = 32,
    };

    ret = spi_bus_add_device((spi_host_device_t)bus, &dev_config, &spi_client_io->spi_dev);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "adding spi device to bus failed");

    if (io_config->sync_gpio_num >= 0)
    {
        if (io_config->flags.sync_use_expander)
        {
            if (io_config->io_expander == NULL)
            {
                ESP_GOTO_ON_FALSE(false, ESP_ERR_INVALID_ARG, err, TAG, "io expander not set");
            }
            spi_client_io->io_expander = io_config->io_expander;
            ESP_GOTO_ON_ERROR(esp_io_expander_set_dir(io_config->io_expander, io_config->sync_gpio_num, IO_EXPANDER_INPUT), err, TAG, "setting sync GPIO for sync failed");
        }
        else
        {
            // zero-initialize the config structure.
            gpio_config_t io_conf = {};
            io_conf.intr_type = GPIO_INTR_DISABLE;
            io_conf.pin_bit_mask = (1 << io_config->sync_gpio_num);
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_down_en = 1;
            ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configuring sync GPIO for sync failed");
        }
    }

    spi_client_io->sync_gpio_num = io_config->sync_gpio_num;
    spi_client_io->wait_delay = io_config->wait_delay;
    spi_client_io->user_ctx = io_config->user_ctx;
    spi_client_io->base.del = client_io_spi_del;
    spi_client_io->base.write = client_io_spi_write;
    spi_client_io->base.read = client_io_spi_read;
    spi_client_io->base.available = client_io_spi_available;
    spi_client_io->base.flush = client_io_spi_flush;
    spi_client_io->base.read_bulk = client_io_spi_read_bulk;
    spi_client_io->base.handle = spi_client_io->spi_dev;

    spi_client_io->lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(spi_client_io->lock, ESP_ERR_NO_MEM, err, TAG, "no mem for mutex");

    size_t max_trans_bytes = 0;
    ESP_GOTO_ON_ERROR(spi_bus_get_max_transaction_len((spi_host_device_t)bus, &max_trans_bytes), err, TAG, "get spi max transaction len failed");
    spi_client_io->spi_trans_max_bytes = max_trans_bytes;
    ESP_LOGI(TAG, "spi max trans bytes: %d", spi_client_io->spi_trans_max_bytes);

    /* Persistent internal DMA staging buffer (see struct comment). */
    spi_client_io->dma_stage =
        (uint8_t *)heap_caps_malloc(max_trans_bytes,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(spi_client_io->dma_stage, ESP_ERR_NO_MEM, err, TAG,
                      "no DMA mem for rx staging buffer");

    /* v0.4.18.53: Create SPI worker task queue and task.
     * v0.4.18.56 (doc verification §7): Pin worker to core 0, same as the
     * SPI ISR (isr_cpu_id in eyes.c). Avoids cross-core ISR wakeup latency. */
    spi_client_io->txn_queue = xQueueCreate(SPI_WORKER_QUEUE_LEN, sizeof(spi_txn_t *));
    ESP_GOTO_ON_FALSE(spi_client_io->txn_queue, ESP_ERR_NO_MEM, err, TAG, "no mem for spi txn queue");
    BaseType_t task_ret = xTaskCreatePinnedToCore(spi_worker_task, "spi_worker",
                                                  SPI_WORKER_STACK_SIZE, spi_client_io,
                                                  SPI_WORKER_PRIORITY,
                                                  &spi_client_io->worker_task,
                                                  0);
    ESP_GOTO_ON_FALSE(task_ret == pdPASS, ESP_FAIL, err, TAG, "spi worker task create failed");
    ESP_LOGI(TAG, "SPI worker task started (prio %d, core 0)", SPI_WORKER_PRIORITY);

    *ret_io = &spi_client_io->base;
    ESP_LOGD(TAG, "new spi sscma client io @%p", spi_client_io);

    return ESP_OK;

err:
    if (spi_client_io)
    {
        if (spi_client_io->spi_dev)
        {
            spi_bus_remove_device(spi_client_io->spi_dev);
            spi_bus_free((spi_host_device_t)bus);
        }
        if (spi_client_io->lock)
        {
            vSemaphoreDelete(spi_client_io->lock);
        }
        free(spi_client_io);
    }
    return ret;
}

static esp_err_t client_io_spi_del(sscma_client_io_t *io)
{
    esp_err_t ret = ESP_OK;
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    if (spi_client_io->lock)
    {
        vSemaphoreDelete(spi_client_io->lock);
    }
    spi_bus_remove_device(spi_client_io->spi_dev);
    spi_bus_free((spi_host_device_t)spi_client_io->spi_dev);
    if (spi_client_io->sync_gpio_num >= 0)
    {
        gpio_reset_pin(spi_client_io->sync_gpio_num);
    }
    ESP_LOGD(TAG, "del spi sscma client io @%p", spi_client_io);

    heap_caps_free(spi_client_io->dma_stage);
    free(spi_client_io);
    return ret;
}

static esp_err_t spi_txn_write_exec(sscma_client_io_spi_t *spi_client_io, const void *data, size_t len)
{
    esp_err_t ret = ESP_OK;
    spi_transaction_t spi_trans = {};
    /* v0.4.18.53: Worker executor — spi_client_io passed directly. */
    /* v0.4.18.36: MISO-flag: Wait for Himax RX-armed (MISO LOW) before sending
     * AT command via SPI. The Himax drives PB2 LOW when RX DMA is armed.
     * v0.4.18.38: Fatal now. If timeout, Himax is down; fail fast. */
    if (!spi_miso_wait_level_(0, 50, "RX-armed LOW (AT cmd)")) {
        ESP_LOGE(TAG, "client_io_spi_write: Himax not RX-armed (MISO not LOW), failing");
        return ESP_ERR_TIMEOUT;
    }
    /* v0.4.14 (P1): 248-byte chunks with CRC16-CCITT trailer. The CRC covers
     * [FEATURE, CMD, LEN_HI, LEN_LO, payload...]; the Himax verifies it
     * (or accepts the legacy 0xFF 0xFF trailer from old S3 firmware). */
    uint16_t packets = len / MAX_PL_LEN_CRC;
    uint16_t remain = len % MAX_PL_LEN_CRC;
    size_t trans_len = 0;

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);

    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    if (data)
    {
        for (uint16_t i = 0; i < packets; i++)
        {
            trans_len = PACKET_SIZE;
            memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
            spi_client_io->buffer[0] = FEATURE_TRANSPORT;
            spi_client_io->buffer[1] = FEATURE_TRANSPORT_CMD_WRITE;
            spi_client_io->buffer[2] = MAX_PL_LEN_CRC >> 8;
            spi_client_io->buffer[3] = MAX_PL_LEN_CRC & 0xFF;
            memcpy(spi_client_io->buffer + 4, data + i * MAX_PL_LEN_CRC, MAX_PL_LEN_CRC);
            uint16_t crc = crc16_ccitt_(spi_client_io->buffer, 4 + MAX_PL_LEN_CRC);
            spi_client_io->buffer[4 + MAX_PL_LEN_CRC] = (uint8_t)(crc >> 8);
            spi_client_io->buffer[5 + MAX_PL_LEN_CRC] = (uint8_t)(crc & 0xFF);
            spi_trans.tx_buffer = spi_client_io->buffer;
            if (spi_client_io->wait_delay > 0)
            {
                vTaskDelay(pdMS_TO_TICKS(spi_client_io->wait_delay));
            }
            do
            {
                uint16_t chunk_size = trans_len;
                if (chunk_size > spi_client_io->spi_trans_max_bytes)
                {
                    chunk_size = spi_client_io->spi_trans_max_bytes;
                    spi_trans.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
                }
                else
                {
                    chunk_size = trans_len;
                    spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
                }
                spi_trans.length = chunk_size * 8;
                spi_trans.rxlength = 0;
                spi_trans.rx_buffer = NULL;
                spi_trans.user = spi_client_io;
                ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
                SPI_XMIT_GOTO_ON_ERROR(ret, err);
                spi_trans.tx_buffer = spi_trans.tx_buffer + chunk_size;
                trans_len -= chunk_size;
            }
            while (trans_len > 0);
        }

        if (remain)
        {
            trans_len = PACKET_SIZE;
            memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
            spi_client_io->buffer[0] = FEATURE_TRANSPORT;
            spi_client_io->buffer[1] = FEATURE_TRANSPORT_CMD_WRITE;
            spi_client_io->buffer[2] = remain >> 8;
            spi_client_io->buffer[3] = remain & 0xFF;
            memcpy(spi_client_io->buffer + 4, data + packets * MAX_PL_LEN_CRC, remain);
            uint16_t crc = crc16_ccitt_(spi_client_io->buffer, 4 + remain);
            spi_client_io->buffer[4 + remain] = (uint8_t)(crc >> 8);
            spi_client_io->buffer[5 + remain] = (uint8_t)(crc & 0xFF);
            spi_trans.tx_buffer = spi_client_io->buffer;
            if (spi_client_io->wait_delay > 0)
            {
                vTaskDelay(pdMS_TO_TICKS(spi_client_io->wait_delay));
            }
            do
            {
                uint16_t chunk_size = trans_len;
                if (chunk_size > spi_client_io->spi_trans_max_bytes)
                {
                    chunk_size = spi_client_io->spi_trans_max_bytes;
                    spi_trans.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
                }
                else
                {
                    chunk_size = trans_len;
                    spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
                }
                spi_trans.length = chunk_size * 8;
                spi_trans.tx_buffer = spi_client_io->buffer;
                spi_trans.rxlength = 0;
                spi_trans.rx_buffer = NULL;
                spi_trans.user = spi_client_io;
                ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
                SPI_XMIT_GOTO_ON_ERROR(ret, err);
                spi_trans.tx_buffer = spi_trans.tx_buffer + chunk_size;
                trans_len -= chunk_size;
            }
            while (trans_len > 0);
        }
    }

err:
    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}

static esp_err_t spi_txn_read_exec(sscma_client_io_spi_t *spi_client_io, void *data, size_t len)
{
    /* v0.4.18.51 (Phase A2.3): Fully gated READ transaction.
     * Old code sent CMD_READ blind (no wait-LOW) and clocked data after a
     * vTaskDelay (no wait-HIGH). The blind 4095-byte clock against a re-armed
     * Himax injected ~16 junk RX frames.
     *
     * New: wait-LOW -> cmd -> wait-HIGH -> clock-exact(len).
     * - spi_send_cmd_() waits for LOW before sending CMD_READ.
     * - spi_miso_wait_level_(1) waits for HIGH (Himax staged the data).
     * - Clock exactly len bytes into dma_stage, memcpy to data.
     * - No vTaskDelay anywhere (wait_delay=0).
     * v0.4.18.53: Worker executor — spi_client_io passed directly. */
    esp_err_t ret = ESP_OK;
    spi_transaction_t spi_trans = {};

    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);

    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    /* Gate 1: wait-LOW (Himax RX-armed), then send CMD_READ with length. */
    ret = spi_send_cmd_(spi_client_io, FEATURE_TRANSPORT_CMD_READ, (uint16_t)len);
    if (ret != ESP_OK) {
        goto err;
    }

    /* Gate 2: wait-HIGH (Himax has staged the data). */
    if (!spi_miso_wait_level_(1, 50, "READ data HIGH")) {
        ESP_LOGW(TAG, "read: Himax never staged data (HIGH timeout, len=%u)", (unsigned)len);
        ret = ESP_ERR_TIMEOUT;
        goto err;
    }

    /* Clock exactly len bytes. */
    {
        size_t remaining = len;
        uint8_t *dst = (uint8_t *)data;
        while (remaining > 0) {
            uint16_t chunk = (remaining > spi_client_io->spi_trans_max_bytes)
                ? spi_client_io->spi_trans_max_bytes : (uint16_t)remaining;
            spi_trans.length = chunk * 8;
            spi_trans.tx_buffer = NULL;
            spi_trans.rxlength = chunk * 8;
            spi_trans.rx_buffer = spi_client_io->dma_stage;
            spi_trans.user = spi_client_io;
            spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
            ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
            if (ret != ESP_OK) {
                goto err;
            }
            memcpy(dst, spi_client_io->dma_stage, chunk);
            dst += chunk;
            remaining -= chunk;
        }
    }

err:
    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}
static esp_err_t spi_txn_available_exec(sscma_client_io_spi_t *spi_client_io, size_t *len)
{
    /* v0.4.18.51 (Phase A2.2): Fully gated AVAILABLE transaction.
     * Old code sent the 256-byte CMD_AVAILABLE blind (no wait-LOW) and clocked
     * the 2-byte count with no wait-HIGH. Under the MISO-flag protocol this
     * raced the Himax's TX->RX re-arm.
     *
     * New: wait-LOW -> cmd -> wait-HIGH -> clock-exact(2).
     * - spi_send_cmd_() waits for LOW (Himax RX-armed) before sending.
     * - spi_miso_wait_level_(1) waits for HIGH (Himax has staged the count).
     * - Clock exactly 2 bytes. No vTaskDelay anywhere (wait_delay=0).
     *
     * With Himax v0.4.20.41+ (status-based re-arm), HIGH means "TX staged and
     * NOT yet clocked out" (re-arm happens on TXFLR==0 && !BUSY, not timer).
     * The wait-HIGH cannot miss. */
    esp_err_t ret = ESP_OK;
    spi_transaction_t spi_trans = {};
    /* v0.4.18.53: Worker executor — spi_client_io passed directly. */

    *len = 0;

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);

    /* v0.4.18.49: Skip AT polling during spitest. */
    if (spi_client_io->test_active > 0) {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_OK;
    }

    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    /* Gate 1: wait-LOW (Himax RX-armed), then send CMD_AVAILABLE. */
    ret = spi_send_cmd_(spi_client_io, FEATURE_TRANSPORT_CMD_AVAILABLE, 0);
    if (ret != ESP_OK) {
        goto err;
    }

    /* Gate 2: wait-HIGH (Himax has staged the 2-byte count). */
    if (!spi_miso_wait_level_(1, 50, "AVAILABLE count HIGH")) {
        ESP_LOGW(TAG, "available: Himax never staged count (HIGH timeout)");
        ret = ESP_ERR_TIMEOUT;
        goto err;
    }

    /* Clock exactly 2 bytes. */
    spi_trans.length = 2 * 8;
    spi_trans.tx_buffer = NULL;
    spi_trans.rxlength = 2 * 8;
    spi_trans.rx_buffer = spi_client_io->buffer;
    spi_trans.user = spi_client_io;
    spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
    memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
    ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
    if (ret != ESP_OK) {
        goto err;
    }
    *len = (spi_client_io->buffer[0] << 8) | spi_client_io->buffer[1];
    if (*len == 0xFFFF)
    {
        *len = 0;
    }
err:
    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}

static esp_err_t client_io_spi_flush(sscma_client_io_t *io)
{
    esp_err_t ret = ESP_OK;
    spi_transaction_t spi_trans = {};
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    size_t trans_len = 0;
    uint32_t sync_level = 0;

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);

    if (spi_client_io->sync_gpio_num >= 0)
    {
        if (spi_client_io->io_expander)
        {
            if (esp_io_expander_get_level(spi_client_io->io_expander, spi_client_io->sync_gpio_num, &sync_level) != ESP_OK)
            {
                xSemaphoreGive(spi_client_io->lock);
                return ESP_FAIL;
            }
        }
        else
        {
            sync_level = gpio_get_level(spi_client_io->sync_gpio_num);
        }
        if (sync_level == 0)
        {
            xSemaphoreGive(spi_client_io->lock);
            return ESP_OK;
        }
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(spi_client_io->wait_delay));
    }

    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    trans_len = PACKET_SIZE;
    memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
    spi_client_io->buffer[0] = FEATURE_TRANSPORT;
    spi_client_io->buffer[1] = FEATURE_TRANSPORT_CMD_RESET;
    spi_client_io->buffer[2] = 0x00;
    spi_client_io->buffer[3] = 0x00;
    spi_client_io->buffer[4] = 0xFF;
    spi_client_io->buffer[5] = 0xFF;
    spi_trans.tx_buffer = spi_client_io->buffer;
    if (spi_client_io->wait_delay > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(spi_client_io->wait_delay));
    }
    do
    {
        uint16_t chunk_size = trans_len;
        if (chunk_size > spi_client_io->spi_trans_max_bytes)
        {
            chunk_size = spi_client_io->spi_trans_max_bytes;
            spi_trans.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        }
        else
        {
            chunk_size = trans_len;
            spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
        }
        spi_trans.length = chunk_size * 8;
        spi_trans.rxlength = 0;
        spi_trans.rx_buffer = NULL;
        spi_trans.user = spi_client_io;
        ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
        SPI_XMIT_GOTO_ON_ERROR(ret, err);
        spi_trans.tx_buffer = spi_trans.tx_buffer + chunk_size;
        trans_len -= chunk_size;
    }
    while (trans_len > 0);

err:
    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}
/* v0.4.14 (P1): send one transport command packet (256-byte frame, header +
 * CRC16 trailer, zero-padded). The Himax RX DMA expects a full 256-byte
 * frame per command. Caller must hold the io lock and the SPI bus. */
/* Forward declaration for MISO-flag helper (defined below). */

/* v0.4.18.45: Synchronously drain Himax TX FIFO when MISO is HIGH.
 * The Himax drives HIGH when it has data (e.g., an AT response) in its TX
 * FIFO. If the S3 needs to send a command (bulk transfer, spitest) but the
 * Himax is HIGH, we must drain the FIFO first to get the Himax back to LOW
 * (RX-armed). Reads 64-byte chunks (no CMD, just clock — Himax in TX mode
 * ignores MOSI) until MISO goes LOW or timeout. Caller must hold the io lock
 * and the SPI bus. Returns true if drained to LOW, false on timeout.
 * Public: called explicitly before bulk transfers, NOT automatically in
 * spi_send_cmd_() (which would drain BULK data mid-transfer). */
bool spi_drain_tx_(sscma_client_io_spi_t *spi_client_io, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    spi_transaction_t spi_trans = {};
    uint8_t dummy[64];

    ESP_LOGI(TAG, "spi_drain_tx: Himax HIGH, draining TX FIFO...");
    while (gpio_get_level(GPIO_NUM_6) == 1) {
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ESP_LOGW(TAG, "spi_drain_tx: timeout, Himax still HIGH");
            return false;
        }
        /* Clock 64 bytes: tx_buffer=NULL (no MOSI), Himax shifts out TX FIFO */
        memset(&spi_trans, 0, sizeof(spi_trans));
        spi_trans.length = sizeof(dummy) * 8;
        spi_trans.tx_buffer = NULL;
        spi_trans.rxlength = sizeof(dummy) * 8;
        spi_trans.rx_buffer = dummy;  /* discard */
        spi_trans.user = spi_client_io;
        esp_err_t ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "spi_drain_tx: transmit failed: %s", esp_err_to_name(ret));
            return false;
        }
        /* Brief pause to let Himax update MISO after FIFO drains */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGI(TAG, "spi_drain_tx: Himax LOW (RX-armed), drain complete");
    return true;
}

static esp_err_t spi_send_cmd_(sscma_client_io_spi_t *spi_client_io,
                               uint8_t cmd, uint16_t len)
{
    /* v0.4.18.35 (Phase 2): MISO-flag handshake. Wait for LOW before sending.
     * LOW = Himax RX armed (PB2 muxed to GPIO13, driven LOW).
     * v0.4.18.38: Fatal now. Himax 0.4.20.38+ drives PB2. If timeout,
     * the Himax is down (rebooted/crashed); fail fast. */
    if (!spi_miso_wait_level_(0, 50, "RX-armed LOW")) {
        ESP_LOGE(TAG, "spi_send_cmd: Himax not RX-armed (cmd=0x%02x), failing (Himax down?)", cmd);
        return ESP_ERR_TIMEOUT;
    }
    spi_transaction_t spi_trans = {};
    size_t trans_len = PACKET_SIZE;

    memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
    spi_client_io->buffer[0] = FEATURE_TRANSPORT;
    spi_client_io->buffer[1] = cmd;
    spi_client_io->buffer[2] = (uint8_t)(len >> 8);
    spi_client_io->buffer[3] = (uint8_t)(len & 0xFF);
    uint16_t crc = crc16_ccitt_(spi_client_io->buffer, HEADER_LEN);
    spi_client_io->buffer[4] = (uint8_t)(crc >> 8);
    spi_client_io->buffer[5] = (uint8_t)(crc & 0xFF);
    spi_trans.tx_buffer = spi_client_io->buffer;
    if (spi_client_io->wait_delay > 0)
    {
        vTaskDelay(pdMS_TO_TICKS(spi_client_io->wait_delay));
    }
    do
    {
        uint16_t chunk_size = trans_len;
        if (chunk_size > spi_client_io->spi_trans_max_bytes)
        {
            chunk_size = spi_client_io->spi_trans_max_bytes;
            spi_trans.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        }
        else
        {
            chunk_size = trans_len;
            spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
        }
        spi_trans.length = chunk_size * 8;
        spi_trans.rxlength = 0;
        spi_trans.rx_buffer = NULL;
        spi_trans.user = spi_client_io;
        esp_err_t ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "cmd 0x%02x transmit failed: %s", cmd,
                     esp_err_to_name(ret));
            return ret;
        }
        spi_trans.tx_buffer = (const uint8_t *)spi_trans.tx_buffer + chunk_size;
        trans_len -= chunk_size;
    }
    while (trans_len > 0);
    return ESP_OK;
}

/* v0.4.18.60 (Claude F1): No-response command send with stale-LOW settle.
 * No-response commands (START/STOP/RESET/WRITE/PARK) leave MISO LOW
 * asserted through the Himax handler's entire execution (~30-80us for
 * START with its 4KB prefill). The S3's next wait-LOW passes instantly
 * on this stale LOW and can land the following command mid-handler,
 * before the Himax re-arms RX — causing self-sustaining frame
 * misalignment. The 150us settle (busy-wait, not vTaskDelay) gives the
 * Himax time to complete the handler and re-arm, making the next
 * wait-LOW honest. Costs nothing against 171us command wire-time. */
static esp_err_t spi_send_cmd_noresp_(sscma_client_io_spi_t *spi_client_io,
                                      uint8_t cmd, uint16_t len)
{
    esp_err_t ret = spi_send_cmd_(spi_client_io, cmd, len);
    if (ret == ESP_OK) {
        esp_rom_delay_us(150);
    }
    return ret;
}

/* v0.4.18.59 (Claude PARK): No-gate command send. Skips the MISO LOW wait.
 * Used for PARK resume: while parked, the line is HIGH (pull-up), so the
 * normal LOW gate would timeout. The command is sent "on faith" — the
 * Himax is RX-armed and listening, just silent. */
static esp_err_t spi_send_cmd_no_gate_(sscma_client_io_spi_t *spi_client_io,
                                       uint8_t cmd, uint16_t len)
{
    spi_transaction_t spi_trans = {};
    size_t trans_len = PACKET_SIZE;

    memset(spi_client_io->buffer, 0, sizeof(spi_client_io->buffer));
    spi_client_io->buffer[0] = FEATURE_TRANSPORT;
    spi_client_io->buffer[1] = cmd;
    spi_client_io->buffer[2] = (uint8_t)(len >> 8);
    spi_client_io->buffer[3] = (uint8_t)(len & 0xFF);
    uint16_t crc = crc16_ccitt_(spi_client_io->buffer, HEADER_LEN);
    spi_client_io->buffer[4] = (uint8_t)(crc >> 8);
    spi_client_io->buffer[5] = (uint8_t)(crc & 0xFF);
    spi_trans.tx_buffer = spi_client_io->buffer;
    do
    {
        uint16_t chunk_size = trans_len;
        if (chunk_size > spi_client_io->spi_trans_max_bytes)
        {
            chunk_size = spi_client_io->spi_trans_max_bytes;
            spi_trans.flags |= SPI_TRANS_CS_KEEP_ACTIVE;
        }
        else
        {
            chunk_size = trans_len;
            spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
        }
        spi_trans.length = chunk_size * 8;
        spi_trans.rxlength = 0;
        spi_trans.rx_buffer = NULL;
        spi_trans.user = spi_client_io;
        esp_err_t ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
        if (ret != ESP_OK)
        {
            return ret;
        }
        spi_trans.tx_buffer = (const uint8_t *)spi_trans.tx_buffer + chunk_size;
        trans_len -= chunk_size;
    }
    while (trans_len > 0);
    return ESP_OK;
}

/* v0.4.18.59 (Claude PARK): Enter parked state. Sends CMD_PARK via the
 * normal gated path (Himax must be RX-armed/LOW). After PARK, the Himax
 * releases PB2 (HIGH via pull-up); S3 can safely run SD transactions.
 * Must be called from a verified-armed state. */
static esp_err_t sscma_client_spi_park(sscma_client_io_spi_t *spi_client_io)
{
    /* PARK is a no-response command; use the normal gated send. */
    return spi_send_cmd_(spi_client_io, FEATURE_TRANSPORT_CMD_PARK, 0);
}

/* v0.4.18.59 (Claude PARK): Exit parked state. Sends AVAILABLE "on faith"
 * (no LOW gate — line is HIGH while parked). 50ms timeout + one retry,
 * fallback to RESET resync. On success, the Himax's normal handler has
 * restored GPIO13-LOW flag behavior. */
static esp_err_t sscma_client_spi_unpark(sscma_client_io_spi_t *spi_client_io)
{
    esp_err_t ret;
    /* Try AVAILABLE (no gate) — the resume command. */
    for (int attempt = 0; attempt < 2; attempt++) {
        ret = spi_send_cmd_no_gate_(spi_client_io,
                                    FEATURE_TRANSPORT_CMD_AVAILABLE, 0);
        if (ret == ESP_OK) {
            /* Wait for HIGH (Himax stages the 2-byte count). */
            if (spi_miso_wait_level_(1, 50, "UNPARK AVAILABLE HIGH")) {
                /* Success — Himax is back, flag restored. */
                /* Clock out the 2-byte count to complete the transaction. */
                spi_transaction_t spi_trans = {};
                spi_trans.length = 2 * 8;
                spi_trans.rxlength = 2 * 8;
                spi_trans.rx_buffer = spi_client_io->buffer;
                spi_trans.user = spi_client_io;
                spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
                if (spi_device_transmit(spi_client_io->spi_dev,
                                        &spi_trans) == ESP_OK) {
                    return ESP_OK;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* Fallback: RESET resync (also no-gate). */
    ESP_LOGW(TAG, "unpark: AVAILABLE failed, trying RESET resync");
    ret = spi_send_cmd_no_gate_(spi_client_io,
                                FEATURE_TRANSPORT_CMD_RESET, 0);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
        /* After RESET, the Himax re-arms normally (LOW). */
        if (spi_miso_wait_level_(0, 100, "UNPARK RESET LOW")) {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* v0.4.18.15: Common TX-DMA synchronization helper. After the S3 sends a
 * command that causes the Himax to stage data via TX DMA (e.g.,
 * CMD_READ_CRC, CMD_TEST_PATTERN), the DMA needs time to move the data
 * from RAM to the SPI TX FIFO before the S3 starts clocking. Without this
 * sync, the S3 reads stale FIFO contents (observed as a 16-byte offset in
 * spitest byte-level diagnostics).
 *
 * The delay is calculated from the DMA transfer time, not arbitrary:
 *   DMA time ~= xfer_len bytes / 50MB/s; use 1ms per 4KB + 1ms margin.
 *
 * ALL future SPI data pathways MUST call this after spi_send_cmd_() and
 * before the data-clocking transaction. */
static bool spi_miso_wait_level_(int target_level, uint32_t timeout_ms, const char *what)
{
    /* v0.4.18.35 (Phase 2): MISO-flag handshake.
     * Wait for MISO (GPIO6) to reach target_level.
     * LOW = Himax RX armed (safe to send command)
     * HIGH = Himax TX ready (safe to clock response)
     * Returns true if level reached, false on timeout.
     * v0.4.18.54: Yield in poll loop. At worker priority 15, a tight
     * busy-poll starves all lower-priority tasks during the 50ms timeout.
     * A 1-tick yield per iteration costs ~1ms worst-case latency but
     * keeps the system responsive.
     * v0.4.23 (Step 0.3): time every wait; while a bulk transfer is
     * active the elapsed time accrues to bulk_stats_ (split LOW/HIGH),
     * which is how the Himax re-arm toll shows up in the bulk_stats
     * line as wait_low. */
    int64_t t0_us = esp_timer_get_time();
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (gpio_get_level(GPIO_NUM_6) == target_level) {
            bulk_stats_note_wait_(target_level, t0_us);
            return true;
        }
        /* Yield to avoid starving lower-priority tasks at prio 15. */
        vTaskDelay(1);
    }
    bulk_stats_note_wait_(target_level, t0_us);
    ESP_LOGW("sscma_client.io.spi", "MISO-flag timeout waiting for %s (want %d)",
             what, target_level);
    return false;
}

static bool spi_sync_tx_ready_(uint16_t xfer_len, uint32_t delay_ms)
{
    /* v0.4.18.35 (Phase 2): MISO-flag handshake (replaces PB8 polling).
     * Wait for MISO HIGH (Himax released PB2 to SPI_S_DO = response ready).
     * On timeout, do NOT blind-clock; the caller should fail and retry.
     * For now, fall back to delay to avoid breaking existing flows;
     * the wait-LOW gating is added at the command-send sites.
     *
     * v0.4.18.34: Optimized polling for Himax TX-ready via P07 (wrong pin!).
     * v0.4.18.32/33: GPIO2 interrupt (too much latency, worse than polling).
     * v0.4.18.31: P07 polling (worked partially, but wrong pin + floating node).
     */
    (void)xfer_len;
    (void)delay_ms;
    /* Wait up to 50ms for HIGH. If timeout, the Himax is not ready;
     * blind-clocking would cause underflow and protocol desync.
     * v0.4.18.39: Return error instead of blind-clocking. Caller must fail. */
    if (!spi_miso_wait_level_(1, 50, "TX-ready HIGH")) {
        return false;
    }
    /* Note: No fallback delay. If the Himax isn't ready, we should not clock. */
    return true;
}

/* v0.4.14 (P1): poll AVAILABLE with a bounded wait. Returns ESP_OK with
 * the byte count in *out_len (always > 0 on success) or ESP_ERR_TIMEOUT
 * after ~BULK_POLL_BUDGET_MS of empty polls.
 * Caller must hold the io lock and the SPI bus. */
static esp_err_t spi_bulk_poll_available_(sscma_client_io_spi_t *spi_client_io,
                                          size_t *out_len)
{
    esp_err_t ret;
    int64_t start_us = esp_timer_get_time();
    int empty = 0;
    for (;;)
    {
        ret = spi_send_cmd_(spi_client_io, FEATURE_TRANSPORT_CMD_AVAILABLE, 0);
        if (ret != ESP_OK)
        {
            return ret;
        }
        /* v0.4.18.51: Wait-HIGH before clocking count (Phase A2.2). */
        if (!spi_miso_wait_level_(1, 50, "BULK AVAILABLE HIGH")) {
            /* Himax never staged the count; the wait itself burned
             * 50 ms, so just re-check the budget and retry. */
            if ((esp_timer_get_time() - start_us) / 1000 >= BULK_POLL_BUDGET_MS) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        /* Clock out the 2-byte count (legacy format, unchanged). */
        spi_transaction_t spi_trans = {};
        spi_trans.length = 2 * 8;
        spi_trans.tx_buffer = NULL;
        spi_trans.rxlength = 2 * 8;
        spi_trans.rx_buffer = spi_client_io->buffer;
        spi_trans.user = spi_client_io;
        spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
        ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
        if (ret != ESP_OK)
        {
            return ret;
        }
        size_t avail = ((size_t)spi_client_io->buffer[0] << 8) |
                        spi_client_io->buffer[1];
        if (avail > 0)
        {
            /* v0.4.18.51: No 2ms gap. Himax v0.4.20.41+ re-arms on hardware
             * status (TXFLR==0 && !BUSY), not timer. The gap was the timer's
             * shadow. */
            *out_len = avail;
            return ESP_OK;
        }
        /* Empty ring. v0.4.23 (Track A1 + Step 0.3): count the empty
         * poll HERE, inside the poll function — this loop is the only
         * place empty polls exist (the drain loop never sees avail==0;
         * a counter placed there would read zero forever). Back off
         * on the escalating 1 ms -> 10 ms ladder under the ~3 s budget
         * instead of the old flat 100 ms sleep. */
        if (bulk_stats_active_) {
            bulk_stats_.avail_empty++;
        }
        empty++;
        if ((esp_timer_get_time() - start_us) / 1000 >= BULK_POLL_BUDGET_MS) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(empty <= BULK_POLL_FAST_TRIES
                                 ? BULK_POLL_FAST_MS : BULK_POLL_SLOW_MS));
    }
}

/* v0.4.14 (P1): read one CRC-protected chunk via CMD_READ_CRC. The Himax
 * stages [data][CRC16]; we clock len+2 bytes into the DMA staging buffer,
 * verify the CRC, and copy the payload out. Caller holds lock + bus. */
static esp_err_t spi_bulk_read_chunk_(sscma_client_io_spi_t *spi_client_io,
                                      uint8_t *out, uint16_t len)
{
    esp_err_t ret = spi_send_cmd_(spi_client_io,
                                  FEATURE_TRANSPORT_CMD_READ_CRC, len);
    if (ret != ESP_OK)
    {
        return ret;
    }
    /* v0.4.18.15: use common TX-DMA sync helper. */
    /* v0.4.18.28: 1ms -> 2ms. READ_CRC was getting FFs (Himax TX not ready).
     * TEST_PATTERN uses 2ms and works. The Himax needs 2ms for SPI TX
     * setup regardless of data source (LCG vs ring peek). Uniform 2ms.
     * v0.4.18.39: Fail if Himax not TX-ready; don't blind-clock. */
    if (!spi_sync_tx_ready_(len + BULK_CRC_LEN, 2)) {
        ESP_LOGE(TAG, "bulk chunk: Himax not TX-ready (MISO not HIGH), failing");
        return ESP_ERR_TIMEOUT;
    }
    /* v0.4.18.26: cs_ena_pretrans (16us HW delay) handles TX startup.
     * No dummy bytes needed. */
    uint16_t xfer = len + BULK_CRC_LEN;
    /* The DMA staging buffer is spi_trans_max_bytes; chunks never exceed
     * BULK_MAX_CHUNK (4090) + 2 CRC + 4 dummy = 4096 which fits trans max. */
    if (xfer > spi_client_io->spi_trans_max_bytes)
    {
        ESP_LOGE(TAG, "bulk chunk %u exceeds trans max %u", xfer,
                 (unsigned)spi_client_io->spi_trans_max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }
    spi_transaction_t spi_trans = {};
    spi_trans.length = xfer * 8;
    spi_trans.tx_buffer = NULL;
    spi_trans.rxlength = xfer * 8;
    spi_trans.rx_buffer = spi_client_io->dma_stage;
    spi_trans.user = spi_client_io;
    spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
    ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
    if (ret != ESP_OK)
    {
        return ret;
    }
    /* v0.4.18.26: cs_ena_pretrans ensures MISO is driven from clock 0.
     * No dummy byte skip needed. */
    uint8_t *data = spi_client_io->dma_stage;
    uint16_t rx_crc = ((uint16_t)data[len] << 8) | data[len + 1];
    uint16_t calc = crc16_ccitt_(data, len);
    if (rx_crc != calc)
    {
        ESP_LOGW(TAG, "bulk chunk CRC mismatch: got %04x want %04x (len %u)",
                 rx_crc, calc, len);
        /* v0.4.18.29: Enhanced diagnostic - dump first 64 bytes and count
         * leading 0xFF run. This instruments the TX startup latency:
         * - Fixed FF count = timing issue (Himax not ready)
         * - Variable FF count = something else (e.g., DMA not started)
         * - Zero FFs but wrong data = protocol desync */
        if (len >= 4)
        {
            /* Count leading 0xFF bytes */
            uint16_t ff_count = 0;
            while (ff_count < len && data[ff_count] == 0xFF) {
                ff_count++;
            }
            /* Log first 64 bytes in 4 lines of 16 */
            ESP_LOGW(TAG, "bulk chunk len=%u ff_lead=%u", len, ff_count);
            for (int row = 0; row < 4 && row*16 < len; row++) {
                int off = row*16;
                ESP_LOGW(TAG, "  [%04x]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                         off,
                         data[off], data[off+1], data[off+2], data[off+3],
                         data[off+4], data[off+5], data[off+6], data[off+7],
                         data[off+8], data[off+9], data[off+10], data[off+11],
                         data[off+12], data[off+13], data[off+14], data[off+15]);
            }
            ESP_LOGW(TAG, "  crc bytes: %02x %02x", data[len], data[len+1]);
        }
        return ESP_ERR_INVALID_CRC;
    }
    memcpy(out, data, len);
    return ESP_OK;
}

/* v0.4.18.11: SPI channel self-test. Sends CMD_TEST_PATTERN with encoded
 * len+pattern_id, clocks len+2 bytes, verifies CRC and data integrity.
 * Returns ESP_OK if all checks pass, or the first failure encountered.
 * Caller holds lock + bus. */
static esp_err_t spi_test_pattern_(sscma_client_io_spi_t *spi_client_io,
                                   uint16_t len, uint8_t pattern_id,
                                   uint8_t *out)
{
    if (pattern_id > 3 || len == 0 || len > 4094)
    {
        return ESP_ERR_INVALID_ARG;
    }
    /* Encode: bits 15-14 = pattern_id, bits 13-0 = len */
    uint16_t encoded = ((uint16_t)pattern_id << 14) | (len & 0x3FFF);
    esp_err_t ret = spi_send_cmd_(spi_client_io,
                                  FEATURE_TRANSPORT_CMD_TEST_PATTERN,
                                  encoded);
    if (ret != ESP_OK)
    {
        return ret;
    }
    /* v0.4.18.15: use common TX-DMA sync helper.
     * v0.4.18.39: Fail if Himax not TX-ready; don't blind-clock. */
    if (!spi_sync_tx_ready_(len + BULK_CRC_LEN, 2)) {
        ESP_LOGE(TAG, "test pattern: Himax not TX-ready, failing");
        return ESP_ERR_TIMEOUT;
    }
    uint16_t xfer = len + BULK_CRC_LEN;
    if (xfer > spi_client_io->spi_trans_max_bytes)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    spi_transaction_t spi_trans = {};
    spi_trans.length = xfer * 8;
    spi_trans.tx_buffer = NULL;
    spi_trans.rxlength = xfer * 8;
    spi_trans.rx_buffer = spi_client_io->dma_stage;
    spi_trans.user = spi_client_io;
    spi_trans.flags &= ~SPI_TRANS_CS_KEEP_ACTIVE;
    ret = spi_device_transmit(spi_client_io->spi_dev, &spi_trans);
    if (ret != ESP_OK)
    {
        return ret;
    }
    /* Verify CRC */
    uint16_t rx_crc = ((uint16_t)spi_client_io->dma_stage[len] << 8) |
                       spi_client_io->dma_stage[len + 1];
    uint16_t calc = crc16_ccitt_(spi_client_io->dma_stage, len);
    if (rx_crc != calc)
    {
        /* v0.4.18.12: byte-level diagnostics. Dump received vs expected
         * to identify framing, staging, or corruption issues. */
        ESP_LOGW(TAG, "spitest CRC fail: pat=%u len=%u got=%04x want=%04x",
                 pattern_id, len, rx_crc, calc);
        if (len >= 16)
        {
            ESP_LOGW(TAG, "spitest rx[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     spi_client_io->dma_stage[0], spi_client_io->dma_stage[1],
                     spi_client_io->dma_stage[2], spi_client_io->dma_stage[3],
                     spi_client_io->dma_stage[4], spi_client_io->dma_stage[5],
                     spi_client_io->dma_stage[6], spi_client_io->dma_stage[7],
                     spi_client_io->dma_stage[8], spi_client_io->dma_stage[9],
                     spi_client_io->dma_stage[10], spi_client_io->dma_stage[11],
                     spi_client_io->dma_stage[12], spi_client_io->dma_stage[13],
                     spi_client_io->dma_stage[14], spi_client_io->dma_stage[15]);
            ESP_LOGW(TAG, "spitest rx[%u..%u]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | crc bytes: %02x %02x",
                     len-16, len-1,
                     spi_client_io->dma_stage[len-16], spi_client_io->dma_stage[len-15],
                     spi_client_io->dma_stage[len-14], spi_client_io->dma_stage[len-13],
                     spi_client_io->dma_stage[len-12], spi_client_io->dma_stage[len-11],
                     spi_client_io->dma_stage[len-10], spi_client_io->dma_stage[len-9],
                     spi_client_io->dma_stage[len-8], spi_client_io->dma_stage[len-7],
                     spi_client_io->dma_stage[len-6], spi_client_io->dma_stage[len-5],
                     spi_client_io->dma_stage[len-4], spi_client_io->dma_stage[len-3],
                     spi_client_io->dma_stage[len-2], spi_client_io->dma_stage[len-1],
                     spi_client_io->dma_stage[len], spi_client_io->dma_stage[len+1]);
        }
        return ESP_ERR_INVALID_CRC;
    }
    /* Verify data integrity against expected pattern */
    uint32_t lcg_state = 12345;  /* v0.4.18.11: local, reset per test */
    for (uint16_t i = 0; i < len; i++)
    {
        uint8_t expected;
        switch (pattern_id)
        {
            case 0: expected = 0x00; break;
            case 1: expected = 0xFF; break;
            case 2: expected = (uint8_t)(i & 0xFF); break;
            case 3:
                lcg_state = (1103515245u * lcg_state + 12345u) & 0x7FFFFFFFu;
                expected = (uint8_t)((lcg_state >> 16) & 0xFF);
                break;
            default: expected = 0; break;
        }
        if (spi_client_io->dma_stage[i] != expected)
        {
            ESP_LOGW(TAG, "spitest data fail: pat=%u len=%u @%u got=%02x want=%02x",
                     pattern_id, len, i,
                     spi_client_io->dma_stage[i], expected);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (out)
    {
        memcpy(out, spi_client_io->dma_stage, len);
    }
    return ESP_OK;
}

/* v0.4.18.11: public wrapper for SPI channel self-test. */
esp_err_t sscma_client_io_spi_test_pattern(sscma_client_io_handle_t io,
                                           uint16_t len, uint8_t pattern_id,
                                           uint8_t *out)
{
    if (!io)
    {
        return ESP_ERR_INVALID_ARG;
    }
    sscma_client_io_spi_t *spi_client_io =
        __containerof(io, sscma_client_io_spi_t, base);

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);
    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    /* v0.4.18.45: Drain any pending TX data before the test. The Himax may
     * be HIGH from a previous AT response. Drain synchronously here. */
    if (gpio_get_level(GPIO_NUM_6) == 1) {
        ESP_LOGI(TAG, "spitest: Himax HIGH before start, draining TX FIFO...");
        if (!spi_drain_tx_(spi_client_io, 2000)) {
            ESP_LOGE(TAG, "spitest: drain failed, Himax still HIGH");
            spi_device_release_bus(spi_client_io->spi_dev);
            xSemaphoreGive(spi_client_io->lock);
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t ret = spi_test_pattern_(spi_client_io, len, pattern_id, out);

    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}

void sscma_client_io_spi_test_begin(sscma_client_io_handle_t io)
{
    if (!io) return;
    sscma_client_io_spi_t *spi_client_io =
        __containerof(io, sscma_client_io_spi_t, base);
    spi_client_io->test_active++;
}

void sscma_client_io_spi_test_end(sscma_client_io_handle_t io)
{
    if (!io) return;
    sscma_client_io_spi_t *spi_client_io =
        __containerof(io, sscma_client_io_spi_t, base);
    if (spi_client_io->test_active > 0) {
        spi_client_io->test_active--;
    }
}

/* v0.4.14 (P1): bulk-read `size` bytes via CMD_START / CMD_READ_CRC /
 * CMD_STOP. Holds the io lock and the SPI bus for the whole transfer so
 * the AT poll task cannot interleave. On a chunk CRC mismatch the transfer
 * is NOT retried at chunk granularity (the Himax pops the previous chunk
 * when the next command arrives, so a retried chunk would return the wrong
 * bytes); instead the whole transfer restarts via CMD_RESET + CMD_START,
 * up to BULK_MAX_ATTEMPTS times, then aborts with a diagnostic. */
static esp_err_t spi_txn_bulk_exec(sscma_client_io_spi_t *spi_client_io, void *data,
                                   size_t size)
{
    /* v0.4.18.56 Phase 2: Bulk as atomic worker transaction.
     * Previously drove SPI directly, holding the lock for ~40s and starving
     * AT commands (telemetry 2026-10-07: AT "write command failed" during
     * bulk). Now serialized through the worker queue — AT txns queue behind
     * bulk and timeout cleanly instead of interleaving. */
    if (!spi_client_io || !data || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(spi_client_io->lock, portMAX_DELAY);
    if (spi_device_acquire_bus(spi_client_io->spi_dev, portMAX_DELAY) != ESP_OK)
    {
        xSemaphoreGive(spi_client_io->lock);
        return ESP_FAIL;
    }

    /* v0.4.23 (Step 0.3): start the per-transfer stats window here so
     * the pre-start drain, START, drain loop and STOP all accrue to it. */
    memset(&bulk_stats_, 0, sizeof(bulk_stats_));
    bulk_stats_active_ = true;
    int64_t bulk_t0_us = esp_timer_get_time();
    int attempts_used = 0;

    /* v0.4.18.45: Drain any pending TX data (e.g., AT response) before
     * starting the bulk transfer. The Himax may be HIGH from a previous
     * AT command response. Drain synchronously here (foreground, lock held)
     * to avoid background-task interference. Do NOT drain between bulk
     * commands (where HIGH is expected for bulk data). */
    if (gpio_get_level(GPIO_NUM_6) == 1) {
        ESP_LOGI(TAG, "bulk: Himax HIGH before start, draining TX FIFO...");
        if (!spi_drain_tx_(spi_client_io, 2000)) {
            ESP_LOGE(TAG, "bulk: drain failed, Himax still HIGH");
            bulk_stats_active_ = false;  /* early return: close the stats window */
            spi_device_release_bus(spi_client_io->spi_dev);
            xSemaphoreGive(spi_client_io->lock);
            return ESP_ERR_TIMEOUT;
        }
    }

    for (int attempt = 0; attempt < BULK_MAX_ATTEMPTS; attempt++)
    {
        attempts_used = attempt + 1;
        if (attempt > 0)
        {
            /* Restart: reset the Himax transport (clears rings, disarms the
             * feeder) and re-arm from offset 0. */
            ESP_LOGW(TAG, "bulk attempt %d/%d: resetting", attempt + 1,
                     BULK_MAX_ATTEMPTS);
            ret = spi_send_cmd_noresp_(spi_client_io, FEATURE_TRANSPORT_CMD_RESET, 0);
            if (ret != ESP_OK)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* Arm the Himax feeder: it starts pushing frame bytes into the TX
         * ring (AT+SPIDUMP must have run first to capture the frame). */
        ret = spi_send_cmd_noresp_(spi_client_io, FEATURE_TRANSPORT_CMD_START, 0);
        if (ret != ESP_OK)
        {
            break;
        }

        size_t done = 0;
        uint8_t *dst = (uint8_t *)data;
        int below_thresh = 0;
        while (done < size)
        {
            size_t avail = 0;
            ret = spi_bulk_poll_available_(spi_client_io, &avail);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "bulk poll failed: %s", esp_err_to_name(ret));
                break;
            }
            bulk_stats_.avail_polls++;
            size_t remaining = size - done;
            uint16_t threshold = remaining < BULK_MAX_CHUNK
                                 ? (uint16_t)remaining : BULK_MAX_CHUNK;
            /* v0.4.23 (Track A2): never issue a runt READ_CRC. The
             * AVAILABLE snapshot is taken in the same Himax command
             * parse that pops the previous chunk, so it catches the TX
             * ring at its emptiest instant — 4095 reads as 5 right
             * after a full pop (Himax DIAG: avail is back to 4095 by
             * READ_CRC parse in 99% of records; the feeder refills in
             * ~1 ms). Acting on the 5 produced the 4090/5 alternation:
             * 54% of all READ_CRCs were runts carrying 12% of bytes.
             * Wait 1 ms and re-poll instead. Bounded: a genuinely
             * stuck feeder aborts loudly after ~3 s below threshold.
             * (This subsumes the old v0.4.18.9 zero-avail guard: the
             * poll function only ever returns avail > 0, so that path
             * was unreachable.) */
            if (avail < threshold)
            {
                if (avail > 0) {
                    bulk_stats_.avail_small++;
                }
                if (++below_thresh > BULK_BELOW_MAX)
                {
                    ESP_LOGW(TAG, "bulk: feeder stuck below threshold @%u "
                             "(avail %u < want %u)", (unsigned)done,
                             (unsigned)avail, (unsigned)threshold);
                    ret = ESP_ERR_TIMEOUT;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
            below_thresh = 0;
            uint16_t want = threshold;  /* avail >= threshold: full-size read */
            ret = spi_bulk_read_chunk_(spi_client_io, dst + done, want);
            if (ret != ESP_OK)
            {
                if (ret == ESP_ERR_INVALID_CRC) {
                    bulk_stats_.crc_fail++;
                }
                ESP_LOGW(TAG, "bulk chunk @%u failed: %s", (unsigned)done,
                         esp_err_to_name(ret));
                break;
            }
            if (want == BULK_MAX_CHUNK) {
                bulk_stats_.reads_full++;
            } else {
                bulk_stats_.reads_tail++;
            }
            done += want;
            /* v0.4.18.51: No inter-chunk gap. Himax re-arms on status,
             * not timer. The spi_send_cmd_() wait-LOW gate ensures the Himax
             * is ready before each READ_CRC. */
        }

        /* Always disarm the feeder, then decide whether to retry. */
        esp_err_t stop_ret =
            spi_send_cmd_noresp_(spi_client_io, FEATURE_TRANSPORT_CMD_STOP, 0);
        if (stop_ret != ESP_OK)
        {
            ESP_LOGW(TAG, "bulk STOP failed: %s", esp_err_to_name(stop_ret));
        }
        if (ret == ESP_OK && done == size)
        {
            ESP_LOGI(TAG, "bulk read %u bytes ok (%d attempt(s))",
                     (unsigned)size, attempt + 1);
            ret = ESP_OK;
            break;
        }
        if (ret == ESP_OK)
        {
            ret = ESP_ERR_INVALID_SIZE; /* short transfer */
        }
        /* else: ret already holds the failure; loop retries. */
    }

    /* v0.4.23 (Step 0.3): one-line transfer accounting, success or
     * failure. wait_low is where the Himax TX->RX re-arm toll lands;
     * avail_small counts the polls A2 converted from runt reads into
     * re-polls; reads_runt should stay 0 by construction. */
    if (bulk_stats_active_) {
        int64_t wall_ms = (esp_timer_get_time() - bulk_t0_us) / 1000;
        ESP_LOGI(TAG, "bulk_stats: wall=%lld ms polls=%u empty=%u small=%u "
                      "reads full=%u runt=%u tail=%u crc_fail=%u "
                      "wait_low=%lld ms wait_high=%lld ms attempts=%d",
                 (long long)wall_ms,
                 (unsigned)bulk_stats_.avail_polls,
                 (unsigned)bulk_stats_.avail_empty,
                 (unsigned)bulk_stats_.avail_small,
                 (unsigned)bulk_stats_.reads_full,
                 (unsigned)bulk_stats_.reads_runt,
                 (unsigned)bulk_stats_.reads_tail,
                 (unsigned)bulk_stats_.crc_fail,
                 (long long)(bulk_stats_.wait_low_us / 1000),
                 (long long)(bulk_stats_.wait_high_us / 1000),
                 attempts_used);
        bulk_stats_active_ = false;
    }

    spi_device_release_bus(spi_client_io->spi_dev);
    xSemaphoreGive(spi_client_io->lock);
    return ret;
}

/* v0.4.18.53: SPI worker task. Processes queued transactions atomically.
 * Runs at SPI_WORKER_PRIORITY (15), above app tasks. Each transaction
 * executes start-to-finish without preemption by lower-priority tasks. */
static void spi_worker_task(void *arg)
{
    sscma_client_io_spi_t *spi_client_io = (sscma_client_io_spi_t *)arg;
    spi_txn_t *txn;

    ESP_LOGI(TAG, "SPI worker task running");
    for (;;) {
        if (xQueueReceive(spi_client_io->txn_queue, &txn, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!txn) {
            continue;
        }
        /* Execute the transaction. */
        switch (txn->type) {
        case SPI_TXN_AVAILABLE:
            txn->result = spi_txn_available_exec(spi_client_io,
                                                 txn->params.available.out_len);
            break;
        case SPI_TXN_READ:
            txn->result = spi_txn_read_exec(spi_client_io,
                                            txn->params.read.buf,
                                            txn->params.read.len);
            break;
        case SPI_TXN_WRITE:
            txn->result = spi_txn_write_exec(spi_client_io,
                                             txn->params.write.buf,
                                             txn->params.write.len);
            break;
        case SPI_TXN_BULK:
            txn->result = spi_txn_bulk_exec(spi_client_io,
                                            txn->params.bulk.buf,
                                            txn->params.bulk.len);
            break;
        default:
            ESP_LOGE(TAG, "SPI worker: unknown txn type %d", (int)txn->type);
            txn->result = ESP_ERR_INVALID_ARG;
            break;
        }
        /* Signal completion. Pool entries are never freed; caller marks
         * not-in-use after taking the semaphore. */
        xSemaphoreGive(txn->done);
        /* Do NOT touch txn after give — caller owns it now. */
    }
}

/* v0.4.18.53: Queue-based wrappers. Package the transaction, send to worker,
 * wait for completion. The worker executes atomically at high priority. */

static esp_err_t spi_submit_txn(sscma_client_io_spi_t *spi_client_io,
                                spi_txn_type_t type, void *p1, size_t p2,
                                uint32_t timeout_ms)
{
    /* v0.4.18.54: Static pool, no heap. Eliminates double-free. */
    if (!s_txn_pool_lock) {
        s_txn_pool_lock = xSemaphoreCreateMutex();
    }
    spi_txn_t *txn = NULL;
    xSemaphoreTake(s_txn_pool_lock, portMAX_DELAY);
    for (int i = 0; i < SPI_TXN_POOL_SIZE; i++) {
        if (!s_txn_pool[i].in_use) {
            txn = &s_txn_pool[i];
            txn->in_use = true;
            break;
        }
    }
    xSemaphoreGive(s_txn_pool_lock);
    if (!txn) {
        return ESP_ERR_NO_MEM;
    }
    txn->type = type;
    txn->result = ESP_ERR_TIMEOUT;
    switch (type) {
    case SPI_TXN_AVAILABLE:
        txn->params.available.out_len = (size_t *)p1;
        break;
    case SPI_TXN_READ:
        txn->params.read.buf = p1;
        txn->params.read.len = p2;
        break;
    case SPI_TXN_WRITE:
        txn->params.write.buf = p1;
        txn->params.write.len = p2;
        break;
    case SPI_TXN_BULK:
        txn->params.bulk.buf = p1;
        txn->params.bulk.len = p2;
        break;
    default:
        txn->in_use = false;
        return ESP_ERR_INVALID_ARG;
    }
    txn->done = xSemaphoreCreateBinary();
    if (!txn->done) {
        txn->in_use = false;
        return ESP_ERR_NO_MEM;
    }
    if (xQueueSend(spi_client_io->txn_queue, &txn, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(txn->done);
        txn->in_use = false;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(txn->done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ret = txn->result;
        /* Success: worker gave semaphore. We own cleanup. */
        vSemaphoreDelete(txn->done);
        xSemaphoreTake(s_txn_pool_lock, portMAX_DELAY);
        txn->in_use = false;
        xSemaphoreGive(s_txn_pool_lock);
    } else {
        /* Timeout: worker may still be processing. Do NOT touch txn —
         * worker will give the (now orphaned) semaphore and we leak this
         * pool slot. Better than a use-after-free or double-free. */
    }
    return ret;
}

static esp_err_t client_io_spi_available(sscma_client_io_t *io, size_t *len)
{
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    /* v0.4.18.49: Skip AT polling during spitest (bypass queue). */
    if (spi_client_io->test_active > 0) {
        *len = 0;
        return ESP_OK;
    }
    return spi_submit_txn(spi_client_io, SPI_TXN_AVAILABLE, len, 0, 5000);
}

static esp_err_t client_io_spi_read(sscma_client_io_t *io, void *data, size_t len)
{
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    return spi_submit_txn(spi_client_io, SPI_TXN_READ, data, len, 5000);
}

static esp_err_t client_io_spi_write(sscma_client_io_t *io, const void *data, size_t len)
{
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    return spi_submit_txn(spi_client_io, SPI_TXN_WRITE, (void *)data, len, 5000);
}

static esp_err_t client_io_spi_read_bulk(sscma_client_io_t *io, void *data, size_t size)
{
    sscma_client_io_spi_t *spi_client_io = __containerof(io, sscma_client_io_spi_t, base);
    /* v0.4.18.56 Phase 2: Bulk through the worker queue. 120s timeout —
     * 1.26MB at ~30KB/s takes ~40s. AT txns queued behind bulk will hit
     * their 5s timeout and fail cleanly rather than interleaving. */
    return spi_submit_txn(spi_client_io, SPI_TXN_BULK, data, size, 120000);
}
