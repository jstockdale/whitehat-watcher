/* Display: SPD2010 1.45" 412x412 round LCD on QSPI + LVGL status screen.
 *
 * Wiring (SenseCAP Watcher v1.0, from the official BSP):
 *   SPI3 QSPI: PCLK=GPIO7, D0=GPIO9, D1=GPIO1, D2=GPIO14, D3=GPIO13
 *   LCD_CS=GPIO45, LCD_BL=GPIO8 (active high), no reset pin
 *   LCD power rail: IO expander pin 9 (shared handle owned by eyes)
 *
 * Shows the device name plus live BLE / Wi-Fi status lines. Content stays
 * clear of the screen corners (the panel is round). */

#include "display.h"
#include "hid_ble.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_spd2010.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "eyes.h"
#include "touch.h"
#include "hid_ble.h"

static const char *TAG = "display";

/* LCD pins */
#define LCD_PCLK      7
#define LCD_D0        9
#define LCD_D1        1
#define LCD_D2        14
#define LCD_D3        13
#define LCD_CS        45
#define LCD_BL        8
#define LCD_HRES      412
#define LCD_VRES      412
#define LCD_PCLK_HZ   (40 * 1000 * 1000)

/* IO expander pin gating the LCD power rail */
#define EXP_PIN_PWR_LCD  (1U << 9)

/* Backlight: LEDC PWM like the official BSP (5 kHz, 10-bit, channel 1). */
#define BL_LEDC_TIMER      LEDC_TIMER_1
#define BL_LEDC_CHANNEL    LEDC_CHANNEL_1
#define BL_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define BL_LEDC_MAX_DUTY   (1 << 10)

/* ESP32-S3 SPI DMA can move at most 2^18 bits (32 KiB) per transaction;
 * the LCD panel-io layer chunks larger flushes at max_transfer_sz, so this
 * must stay within the hardware limit or every flush fails. */
#define LCD_MAX_TRANSFER_SZ (32 * 1024)

/* LVGL draw buffers: small DMA-capable INTERNAL buffers (vendor BSP layout).
 * Full-frame PSRAM buffers force the SPI driver to allocate internal bounce
 * buffers (PSRAM sits outside the S3 DMA window) and chunked transfers also
 * need CS_KEEP_ACTIVE; both fail when internal RAM is tight. 10 rows =
 * one SPI transaction per flush, no bounce buffer, no chunking. */
#define LVGL_BUF_ROWS 10

static bool ready_ = false;
static esp_lcd_panel_handle_t panel_ = NULL;
static lv_obj_t *ble_label_ = NULL;
static lv_obj_t *wifi_label_ = NULL;
static lv_obj_t *mode_label_ = NULL;
static lv_obj_t *timer_label_ = NULL;
/* Container holding the status-screen labels, so KEYS mode can hide the
 * whole status UI and show the keyboard instead. */
static lv_obj_t *status_cont_ = NULL;
/* On-screen keyboard for KEYS mode. Hidden unless keyboard visible. */
static lv_obj_t *kb_ = NULL;
static bool kb_visible_ = false;
/* Media remote UI for MEDIA mode. Hidden unless media visible. */
static lv_obj_t *media_cont_ = NULL;
static lv_obj_t *media_icon_ = NULL;
static lv_obj_t *media_volbar_ = NULL;
static lv_obj_t *media_vol_label_ = NULL;
/* Message view: full-screen scrollable text for host responses
 * (e.g. Muse's reply after a wake-word PTT). Hidden unless a message
 * is showing. */
static lv_obj_t *msg_cont_ = NULL;
static lv_obj_t *msg_label_ = NULL;
static lv_obj_t *msg_hint_ = NULL;
static bool msg_visible_ = false;
/* Which UI was showing before the message overlay (0=status, 1=kb,
 * 2=media), so dismiss restores it. */
static int msg_return_to_ = 0;
static char *msg_text_ = NULL; /* PSRAM buffer for the message text */

/* v0.4.45: paced reveal — a host reply appears at reading speed
 * (~18 chars/s, snapped forward to word boundaries so words never
 * split) instead of dumping the whole text at once, then
 * auto-dismisses MSG_HOLD_MS after the reveal completes. A wheel
 * scroll cancels the auto-dismiss (the user is reading); a click
 * dismisses immediately, as before. */
static char *msg_shown_ = NULL; /* PSRAM prefix buffer for the reveal */
static esp_timer_handle_t msg_timer_ = NULL;
static int64_t msg_reveal_start_us_ = 0;
static size_t msg_reveal_len_ = 0;
static bool msg_reveal_done_ = false;
static int64_t msg_hide_at_us_ = 0; /* 0 = no auto-dismiss pending */
#define MSG_REVEAL_CPS 18
#define MSG_HOLD_MS 15000   /* John 2026-11-08: 6 s was too easy to miss;
                             * scroll cancels the timer, click dismisses. */
static void msg_tick_(void *arg);

/* QSPI partial flushes must start/end on 4-px x boundaries. */
static void rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    (void)disp_drv;
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = (((area->x2 >> 2) << 2) + 3);
}

static esp_err_t power_up_lcd(void)
{
    esp_io_expander_handle_t exp = eyes_io_expander();
    if (!exp) {
        ESP_LOGE(TAG, "no IO expander (eyes_init must run first)");
        return ESP_FAIL;
    }
    esp_err_t ret = esp_io_expander_set_dir(exp, EXP_PIN_PWR_LCD,
                                            IO_EXPANDER_OUTPUT);
    if (ret == ESP_OK) {
        ret = esp_io_expander_set_level(exp, EXP_PIN_PWR_LCD, 1);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD power rail on failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Rail + panel need a moment to settle before first command. */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "LCD power rail on");
    return ESP_OK;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer);
    if (ret == ESP_OK) {
        ledc_channel_config_t ch = {
            .gpio_num = LCD_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = BL_LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = BL_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ch);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "backlight LEDC init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t display_set_backlight(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (BL_LEDC_MAX_DUTY * (uint32_t)percent) / 100;
    esp_err_t ret = ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty);
    if (ret == ESP_OK) {
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
    }
    return ret;
}

static esp_err_t panel_init(esp_lcd_panel_handle_t *ret_panel,
                            esp_lcd_panel_io_handle_t *ret_io)
{
    /* Backlight starts at 0; turned up only once the panel is up. */
    if (backlight_init() != ESP_OK) {
        return ESP_FAIL;
    }

    esp_err_t ret;
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PCLK,
        .data0_io_num = LCD_D0,
        .data1_io_num = LCD_D1,
        .data2_io_num = LCD_D2,
        .data3_io_num = LCD_D3,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
    };
    ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = -1, /* QSPI: D/C encoded in the 32-bit command */
        .spi_mode = 3,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = { .quad_mode = true },
    };
    spd2010_vendor_config_t vendor_cfg = {
        .flags = { .use_qspi_interface = 1 },
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                   &io_cfg, ret_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel IO create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ret = esp_lcd_new_panel_spd2010(*ret_io, &panel_cfg, ret_panel);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_reset(*ret_panel);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_init(*ret_panel);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_mirror(*ret_panel, false, false);
    }
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_disp_on_off(*ret_panel, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_del(*ret_panel);
        esp_lcd_panel_io_del(*ret_io);
        *ret_panel = NULL;
        *ret_io = NULL;
        return ret;
    }

    display_set_backlight(100);
    ESP_LOGI(TAG, "SPD2010 panel up, backlight on");
    return ESP_OK;
}

/** Keyboard button press -> BLE HID key. Attached to LV_EVENT_VALUE_CHANGED.
 *  Only text buttons reach us; the keyboard widget consumes its own control
 *  buttons (shift, 1#/ABC) internally for mode switching. */
static void kb_event_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    uint16_t btn_id = lv_keyboard_get_selected_btn(kb);
    const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
    if (!txt || !*txt) {
        return;
    }
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        hid_ble_key("backspace", NULL);
    } else if (strcmp(txt, "Enter") == 0) {
        hid_ble_key("enter", NULL);
    } else if (strlen(txt) == 1) {
        /* Single ASCII char: letter, digit, symbol, or space. */
        hid_ble_key(txt, NULL);
    }
    /* Anything else (shouldn't happen) is ignored. */
}

/** LVGL pointer indev: feeds the SPD2010 touch controller to LVGL, but
 *  only while the keyboard is visible. In other modes the input task owns
 *  the touch panel (pointer touchpad) and LVGL must not see the touches. */
static void touch_indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static lv_point_t last = { .x = 0, .y = 0 };
    /* v0.4.18.76: Removed the kb_visible_ gate. Touch now flows to LVGL
     * always; screens without touch handlers simply ignore it. The gate
     * broke every touch UI outside KEYS mode (including pairing buttons). */
    uint16_t x[TOUCH_MAX_POINTS], y[TOUCH_MAX_POINTS], s[TOUCH_MAX_POINTS];
    int n = touch_ready() ? touch_read(x, y, s) : 0;
    if (n > 0) {
        last.x = (lv_coord_t)x[0];
        last.y = (lv_coord_t)y[0];
        data->state = LV_INDEV_STATE_PR;
        data->point = last;
    } else {
        data->state = LV_INDEV_STATE_REL;
        data->point = last;
    }
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Status screen lives in a full-screen container so KEYS mode can hide
     * it wholesale and show the keyboard instead. Children keep their
     * screen coordinates because the container is at (0,0). */
    status_cont_ = lv_obj_create(scr);
    lv_obj_set_size(status_cont_, 412, 412);
    lv_obj_set_pos(status_cont_, 0, 0);
    lv_obj_set_style_bg_opa(status_cont_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_cont_, 0, 0);
    lv_obj_set_style_pad_all(status_cont_, 0, 0);
    lv_obj_clear_flag(status_cont_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(status_cont_);
    lv_label_set_text(title, hid_ble_device_name());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *sub = lv_label_create(status_cont_);
    lv_label_set_text(sub, "AI input assistant");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x999999), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 136);

    ble_label_ = lv_label_create(status_cont_);
    lv_obj_set_style_text_font(ble_label_, &lv_font_montserrat_22, 0);
    lv_obj_align(ble_label_, LV_ALIGN_TOP_MID, 0, 196);
    /* Initial states: display_set_*() no-ops until ready_ is true,
     * so paint the defaults here while the LVGL lock is held. */
    lv_label_set_text(ble_label_, "BLE  waiting...");
    lv_obj_set_style_text_color(ble_label_, lv_color_hex(0xddaa33), 0);

    wifi_label_ = lv_label_create(status_cont_);
    lv_obj_set_style_text_font(wifi_label_, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_label_, LV_ALIGN_TOP_MID, 0, 238);
    lv_label_set_text(wifi_label_, "WiFi  ---");
    lv_obj_set_style_text_color(wifi_label_, lv_color_hex(0x888888), 0);

    mode_label_ = lv_label_create(status_cont_);
    lv_obj_set_style_text_font(mode_label_, &lv_font_montserrat_22, 0);
    lv_obj_align(mode_label_, LV_ALIGN_TOP_MID, 0, 280);
    lv_label_set_text(mode_label_, "pointer");
    lv_obj_set_style_text_color(mode_label_, lv_color_hex(0x33ccff), 0);

    /* Stopwatch readout, centered and hidden until timer mode is entered. */
    timer_label_ = lv_label_create(status_cont_);
    lv_obj_set_style_text_font(timer_label_, &lv_font_montserrat_32, 0);
    lv_obj_align(timer_label_, LV_ALIGN_CENTER, 0, -24);
    lv_label_set_text(timer_label_, "00:00.00");
    lv_obj_set_style_text_color(timer_label_, lv_color_hex(0xffcc33), 0);
    lv_obj_add_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(status_cont_);
    lv_label_set_text(hint, "pair in Bluetooth settings");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666666), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 318);

    /* On-screen keyboard for KEYS mode. No textarea attached: we send each
     * key straight over BLE HID in kb_event_cb. Starts hidden. */
    kb_ = lv_keyboard_create(scr);
    lv_obj_set_size(kb_, 400, 330);
    lv_obj_align(kb_, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_event_cb(kb_, kb_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);

    /* Media remote UI for MEDIA mode. Starts hidden. */
    media_cont_ = lv_obj_create(scr);
    lv_obj_set_size(media_cont_, 412, 412);
    lv_obj_set_pos(media_cont_, 0, 0);
    lv_obj_set_style_bg_opa(media_cont_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(media_cont_, 0, 0);
    lv_obj_set_style_pad_all(media_cont_, 0, 0);
    lv_obj_clear_flag(media_cont_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *media_title = lv_label_create(media_cont_);
    lv_label_set_text(media_title, "Media Remote");
    lv_obj_set_style_text_font(media_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(media_title, lv_color_white(), 0);
    lv_obj_align(media_title, LV_ALIGN_TOP_MID, 0, 92);

    /* Play/pause icon: toggles on wheel-click. */
    media_icon_ = lv_label_create(media_cont_);
    lv_label_set_text(media_icon_, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(media_icon_, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(media_icon_, lv_color_hex(0x33ccff), 0);
    lv_obj_align(media_icon_, LV_ALIGN_CENTER, 0, -40);

    /* Volume bar + numeric readout. */
    media_volbar_ = lv_bar_create(media_cont_);
    lv_obj_set_size(media_volbar_, 280, 24);
    lv_obj_align(media_volbar_, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_range(media_volbar_, 0, 100);
    lv_bar_set_value(media_volbar_, 50, LV_ANIM_OFF);
    /* Style the bar: blue fill on dark track. */
    lv_obj_set_style_bg_color(media_volbar_, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(media_volbar_, lv_color_hex(0x33ccff), LV_PART_INDICATOR);

    media_vol_label_ = lv_label_create(media_cont_);
    lv_label_set_text(media_vol_label_, "50%");
    lv_obj_set_style_text_font(media_vol_label_, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(media_vol_label_, lv_color_hex(0x999999), 0);
    lv_obj_align(media_vol_label_, LV_ALIGN_CENTER, 0, 80);

    lv_obj_t *media_hint = lv_label_create(media_cont_);
    lv_label_set_text(media_hint, "wheel=volume  click=play/pause");
    lv_obj_set_style_text_font(media_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(media_hint, lv_color_hex(0x666666), 0);
    lv_obj_align(media_hint, LV_ALIGN_TOP_MID, 0, 318);

    /* Message view for host responses. Full-screen scrollable container
     * with a word-wrapped label. Starts hidden. */
    msg_cont_ = lv_obj_create(scr);
    lv_obj_set_size(msg_cont_, 412, 412);
    lv_obj_set_pos(msg_cont_, 0, 0);
    lv_obj_set_style_bg_color(msg_cont_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(msg_cont_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(msg_cont_, 0, 0);
    lv_obj_set_style_pad_all(msg_cont_, 0, 0);
    /* Scrollable vertically; no scrollbar visuals (round screen). */
    lv_obj_set_scrollbar_mode(msg_cont_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(msg_cont_, LV_OBJ_FLAG_HIDDEN);

    msg_label_ = lv_label_create(msg_cont_);
    /* Inset to the MEASURED safe circle (v0.4.47): the ring test
     * (0.4.46) showed the full r=200 circle visible with a sharp
     * black band beyond it, so UI must inscribe in r<=196. The old
     * geometry (x=40, y=60, w=332) put the label's top corner at
     * radius 221 — outside the panel — which clipped the first
     * characters of the first line ("It" in "It does!", 2026-11-08).
     * Now: width 288 centered (x=62), top y=96 — corner radius 181,
     * and every line of the first screenful (to y=336) inside the
     * safe circle. */
    lv_obj_set_width(msg_label_, 288);
    lv_obj_set_pos(msg_label_, 62, 96);
    lv_label_set_long_mode(msg_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(msg_label_, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(msg_label_, lv_color_white(), 0);
    lv_label_set_text(msg_label_, "");

    /* Dismiss hint: child of the screen (not the scroll container) so it
     * stays fixed while the message scrolls. Shown/hidden with msg_cont_. */
    msg_hint_ = lv_label_create(scr);
    lv_label_set_text(msg_hint_, "click to dismiss");
    lv_obj_set_style_text_font(msg_hint_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(msg_hint_, lv_color_hex(0x666666), 0);
    lv_obj_align(msg_hint_, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(msg_hint_, LV_OBJ_FLAG_HIDDEN);
}

esp_err_t display_init(void)
{
    if (ready_) {
        return ESP_OK;
    }
    if (power_up_lcd() != ESP_OK) {
        return ESP_FAIL;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    if (panel_init(&panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed");
        return ESP_FAIL;
    }
    /* Keep the panel handle for the raw-fill diagnostic (works even if
     * LVGL below fails). */
    panel_ = panel;

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_priority = 4;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return ESP_FAIL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_HRES * LVGL_BUF_ROWS,
        .double_buffer = true,
        .hres = LCD_HRES,
        .vres = LCD_VRES,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    disp->driver->rounder_cb = rounder_cb;

    if (lvgl_port_lock(0)) {
        build_ui();
        /* Feed the touch panel to LVGL. The read callback gates on
         * kb_visible_, so LVGL only sees touches in KEYS mode. */
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touch_indev_read;
        lv_indev_drv_register(&indev_drv);
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL lock failed");
        return ESP_FAIL;
    }

    ready_ = true;
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

bool display_ready(void)
{
    return ready_;
}

/* Raw full-screen fill, bypassing LVGL. Diagnostic: if the panel and
 * backlight are alive this paints the screen regardless of LVGL state.
 *
 * Chunk sizing is adaptive: we size each DMA chunk from a quarter of the
 * largest currently-free DMA block (clamped to 1..64 rows), so a fragmented
 * heap degrades gracefully into smaller chunks instead of failing outright.
 * A single full-frame transfer (412x412x2 = 340 KiB) can never fit in the
 * S3's internal DMA RAM, so chunking is mandatory, not optional. */
static int dma_rows_per_chunk(void)
{
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t budget = largest / 4;
    int rows = (int)(budget / ((size_t)LCD_HRES * sizeof(uint16_t)));
    if (rows < 1) {
        rows = 1;
    }
    if (rows > 64) {
        rows = 64;
    }
    return rows;
}

static esp_err_t fill_rows(int y0, int y1, uint16_t rgb565)
{
    if (panel_ == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int rows_per_chunk = dma_rows_per_chunk();
    size_t chunk_px = (size_t)LCD_HRES * (size_t)rows_per_chunk;
    uint16_t *chunk = heap_caps_malloc(chunk_px * sizeof(uint16_t),
                                        MALLOC_CAP_DMA);
    if (!chunk) {
        return ESP_ERR_NO_MEM;
    }
    /* The panel expects pixels MSB-first on the wire; LVGL pre-swaps its
     * draw buffers via CONFIG_LV_COLOR_16_SWAP, so the raw path must swap
     * too or red/green/blue come out rotated. */
    uint16_t be = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    for (size_t i = 0; i < chunk_px; i++) {
        chunk[i] = be;
    }
    esp_err_t ret = ESP_OK;
    for (int y = y0; y <= y1 && ret == ESP_OK; y += rows_per_chunk) {
        int ye = y + rows_per_chunk - 1;
        if (ye > y1) {
            ye = y1;
        }
        /* draw_bitmap end coordinates are EXCLUSIVE (the driver sends
         * end-1 to CASET/RASET); passing inclusive ends leaves a 1px
         * gap at every chunk boundary. */
        ret = esp_lcd_panel_draw_bitmap(panel_, 0, y, LCD_HRES, ye + 1, chunk);
    }
    free(chunk);
    return ret;
}

esp_err_t display_fill(uint16_t rgb565)
{
    if (panel_ == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Bounded lock wait: if the LVGL task is wedged we report it instead
     * of hanging the serial console. */
    bool locked = !ready_ || lvgl_port_lock(pdMS_TO_TICKS(500));
    if (!locked) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = fill_rows(0, LCD_VRES - 1, rgb565);
    if (locked && ready_) {
        lvgl_port_unlock();
    }
    return ret;
}

/* SPI diagnostic: push a full screen's worth of pixels through the panel in
 * DMA-sized chunks -- the same shape as an LVGL flush. Returns the exact
 * esp_err_t so the host can see which layer rejects the transfer. */
esp_err_t display_test_fullframe(void)
{
    /* Deliberately no LVGL lock: the diagnostic must work even when the
     * LVGL task is stuck. draw_bitmap acquires the SPI bus itself. */
    return fill_rows(0, LCD_VRES - 1, 0x07E0); /* green */
}

void display_set_ble(bool connected)
{
    if (!ready_ || !ble_label_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_label_set_text(ble_label_,
                          connected ? "BLE  connected" : "BLE  waiting...");
        lv_obj_set_style_text_color(ble_label_,
                                    connected ? lv_color_hex(0x33dd66)
                                              : lv_color_hex(0xddaa33), 0);
        lvgl_port_unlock();
    }
}

void display_set_mode(const char *mode)
{
    if (!ready_ || !mode_label_ || !mode || !*mode) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_label_set_text(mode_label_, mode);
        lvgl_port_unlock();
    }
}

/** Stopwatch readout text ("MM:SS.cs"). Thread-safe; no-ops until
 *  display_init() has succeeded. */
void display_set_timer(const char *text)
{
    if (!ready_ || !timer_label_ || !text) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_label_set_text(timer_label_, text);
        lvgl_port_unlock();
    }
}

void display_set_timer_color(uint32_t rgb)
{
    if (!ready_ || !timer_label_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_obj_set_style_text_color(timer_label_, lv_color_hex(rgb), 0);
        lvgl_port_unlock();
    }
}

/** Show/hide the stopwatch readout (used on timer-mode enter/exit). */
void display_set_timer_visible(bool show)
{
    if (!ready_ || !timer_label_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        if (show) {
            lv_obj_clear_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(timer_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}

/** Show/hide the on-screen keyboard (KEYS mode). Hides the status screen
 *  while the keyboard is up, so the keys get the full display. Thread-safe;
 *  no-ops until display_init() has succeeded. */
void display_set_keyboard_visible(bool show)
{
    if (!ready_ || !kb_ || !status_cont_) {
        return;
    }
    kb_visible_ = show;
    if (lvgl_port_lock(0)) {
        if (show) {
            lv_obj_add_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(kb_, LV_OBJ_FLAG_HIDDEN);
            /* Start in lowercase text mode each time we enter. */
            lv_keyboard_set_mode(kb_, LV_KEYBOARD_MODE_TEXT_LOWER);
        } else {
            lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}

/** Show/hide the media remote UI (MEDIA mode). Hides the status screen
 *  while visible. Thread-safe; no-ops until display_init() has succeeded. */
void display_set_media_visible(bool show)
{
    if (!ready_ || !media_cont_ || !status_cont_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        if (show) {
            lv_obj_add_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
            kb_visible_ = false;
            lv_obj_clear_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}

/** Update the play/pause icon. Thread-safe; no-ops until display_init(). */
void display_set_media_playing(bool playing)
{
    if (!ready_ || !media_icon_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        /* Icon shows the CURRENT state: pause icon when playing (tap to
         * pause), play icon when paused (tap to play). */
        lv_label_set_text(media_icon_, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lvgl_port_unlock();
    }
}

/** Update the volume bar (0-100) and numeric readout. Thread-safe. */
void display_set_media_volume(int volume)
{
    if (!ready_ || !media_volbar_ || !media_vol_label_) {
        return;
    }
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 100) {
        volume = 100;
    }
    if (lvgl_port_lock(0)) {
        lv_bar_set_value(media_volbar_, volume, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", volume);
        lv_label_set_text(media_vol_label_, buf);
        lvgl_port_unlock();
    }
}

void display_set_wifi(bool connected, const char *ip)
{
    if (!ready_ || !wifi_label_) {
        return;
    }
    char buf[48];
    if (connected && ip && *ip) {
        snprintf(buf, sizeof(buf), "WiFi  %s", ip);
    } else if (connected) {
        snprintf(buf, sizeof(buf), "WiFi  connected");
    } else {
        snprintf(buf, sizeof(buf), "WiFi  ---");
    }
    if (lvgl_port_lock(0)) {
        lv_label_set_text(wifi_label_, buf);
        lv_obj_set_style_text_color(wifi_label_,
                                    connected ? lv_color_hex(0x33dd66)
                                              : lv_color_hex(0x888888), 0);
        lvgl_port_unlock();
    }
}

/* Maximum message length (host responses). 4KB is plenty for a spoken
 * reply; longer text is truncated. Buffer lives in PSRAM. */
#define MSG_MAX_LEN 4096

/** Show a full-screen scrollable message (host response text).
 *  Hides the current UI; dismiss restores it. Thread-safe. */
void display_show_message(const char *text)
{
    if (!ready_ || !msg_cont_ || !msg_label_ || !text) {
        return;
    }
    /* (Re)allocate the PSRAM text buffer. */
    if (!msg_text_) {
        msg_text_ = heap_caps_malloc(MSG_MAX_LEN, MALLOC_CAP_SPIRAM);
        if (!msg_text_) {
            return;
        }
    }
    if (!msg_shown_) {
        msg_shown_ = heap_caps_malloc(MSG_MAX_LEN, MALLOC_CAP_SPIRAM);
        if (!msg_shown_) {
            return;
        }
    }
    strncpy(msg_text_, text, MSG_MAX_LEN - 1);
    msg_text_[MSG_MAX_LEN - 1] = '\0';

    /* v0.4.45: (re)start the paced reveal from the top. */
    msg_reveal_len_ = 0;
    msg_reveal_done_ = false;
    msg_hide_at_us_ = 0;
    msg_reveal_start_us_ = esp_timer_get_time();
    msg_shown_[0] = '\0';
    if (!msg_timer_) {
        const esp_timer_create_args_t targs = {
            .callback = msg_tick_,
            .name = "msg_reveal",
        };
        esp_timer_create(&targs, &msg_timer_);
    }
    if (msg_timer_ && !esp_timer_is_active(msg_timer_)) {
        esp_timer_start_periodic(msg_timer_, 100 * 1000);
    }

    if (lvgl_port_lock(0)) {
        if (!msg_visible_) {
            /* Remember what to restore on dismiss. */
            if (kb_visible_) {
                msg_return_to_ = 1;
            } else if (!lv_obj_has_flag(media_cont_, LV_OBJ_FLAG_HIDDEN)) {
                msg_return_to_ = 2;
            } else {
                msg_return_to_ = 0;
            }
            lv_obj_add_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
            kb_visible_ = false;
        }
        lv_label_set_text(msg_label_, msg_shown_);
        /* Scroll back to the top for a new message. */
        lv_obj_scroll_to_y(msg_cont_, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(msg_cont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(msg_hint_, LV_OBJ_FLAG_HIDDEN);
        msg_visible_ = true;
        lvgl_port_unlock();
    }
}

/** Dismiss the message view, restoring the previous UI. Thread-safe. */
void display_hide_message(void)
{
    if (!ready_ || !msg_cont_ || !msg_visible_) {
        return;
    }
    /* v0.4.45: the reveal/hold timer's work is done. */
    if (msg_timer_ && esp_timer_is_active(msg_timer_)) {
        esp_timer_stop(msg_timer_);
    }
    msg_hide_at_us_ = 0;
    if (lvgl_port_lock(0)) {
        lv_obj_add_flag(msg_cont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(msg_hint_, LV_OBJ_FLAG_HIDDEN);
        msg_visible_ = false;
        /* Restore what was showing before the message. */
        if (msg_return_to_ == 1) {
            lv_obj_clear_flag(kb_, LV_OBJ_FLAG_HIDDEN);
            kb_visible_ = true;
        } else if (msg_return_to_ == 2) {
            lv_obj_clear_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
        }
        lvgl_port_unlock();
    }
}

/** True while the message view is showing. */
bool display_message_visible(void)
{
    return msg_visible_;
}

/** Scroll the message view by detents (positive = down). Thread-safe. */
void display_message_scroll(int detents)
{
    if (!ready_ || !msg_cont_ || !msg_visible_) {
        return;
    }
    /* v0.4.45: the user is reading — cancel the auto-dismiss. */
    msg_hide_at_us_ = 0;
    if (lvgl_port_lock(0)) {
        /* 40px per detent — about two lines at 22pt. */
        lv_obj_scroll_by(msg_cont_, 0, -detents * 40, LV_ANIM_OFF);
        lvgl_port_unlock();
    }
}

/* v0.4.45: paced-reveal tick (100 ms). Advances the visible prefix
 * at MSG_REVEAL_CPS, snapped forward to word boundaries; when the
 * text is fully shown, arms the auto-dismiss hold; when the hold
 * expires, dismisses the message. Self-stops when the view is gone
 * (click-dismiss also stops the timer in display_hide_message). */
static void msg_tick_(void *arg)
{
    (void)arg;
    if (!msg_visible_ || !msg_text_) {
        if (msg_timer_) {
            esp_timer_stop(msg_timer_);
        }
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!msg_reveal_done_) {
        size_t total = strlen(msg_text_);
        size_t target = (size_t)((now - msg_reveal_start_us_) / 1000 *
                                 MSG_REVEAL_CPS / 1000);
        if (target >= total) {
            target = total;
        } else {
            while (target < total && msg_text_[target] != ' ' &&
                   msg_text_[target] != '\n') {
                target++;
            }
            if (target < total) {
                target++; /* include the separator */
            }
        }
        if (target != msg_reveal_len_) {
            msg_reveal_len_ = target;
            memcpy(msg_shown_, msg_text_, target);
            msg_shown_[target] = '\0';
            if (lvgl_port_lock(0)) {
                lv_label_set_text(msg_label_, msg_shown_);
                /* Auto-scroll to follow the reveal frontier (John,
                 * 2026-11-08): a message longer than the screen
                 * reveals downward — keep the newest line on the
                 * glass. Label sits at y=96 in the full-screen scroll
                 * container; scroll just enough that the revealed
                 * bottom (+24 px) is visible, never backward. */
                lv_obj_update_layout(msg_label_);
                lv_coord_t content_bottom =
                    96 + lv_obj_get_height(msg_label_) + 24;
                if (content_bottom > 412) {
                    lv_coord_t target_y = content_bottom - 412;
                    if (target_y > lv_obj_get_scroll_y(msg_cont_)) {
                        lv_obj_scroll_to_y(msg_cont_, target_y, LV_ANIM_OFF);
                    }
                }
                lvgl_port_unlock();
            }
        }
        if (target >= total) {
            msg_reveal_done_ = true;
            if (msg_hide_at_us_ == 0) {
                msg_hide_at_us_ = now + (int64_t)MSG_HOLD_MS * 1000;
            }
        }
        return;
    }
    if (msg_hide_at_us_ && now >= msg_hide_at_us_) {
        msg_hide_at_us_ = 0;
        display_hide_message();
    }
}

/* ---- v0.4.46: visible-area ring diagnostic ---- */

static lv_obj_t *ring_cont_ = NULL;
static bool ring_visible_ = false;

/* Radius (px) -> color, outermost first. Distinct, nameable colors
 * so the visible/clipped boundary can be reported by eye. */
static const struct { int r; uint32_t color; } RING_TABLE[] = {
    {200, 0xFF0000}, {190, 0xFF7F00}, {180, 0xFFFF00}, {170, 0x7FFF00},
    {160, 0x00FF00}, {150, 0x00FF7F}, {140, 0x00FFFF}, {130, 0x007FFF},
    {120, 0x0000FF}, {110, 0x7F00FF}, {100, 0xFF00FF}, { 90, 0xFF007F},
    { 80, 0xFFFFFF}, { 70, 0xC0C0C0}, { 60, 0x808080}, { 50, 0x800000},
    { 40, 0x808000}, { 30, 0x000080},
};

static void ring_label_(int x, int y, int r, bool dark_text)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", r);
    lv_obj_t *l = lv_label_create(ring_cont_);
    lv_label_set_text(l, buf);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, dark_text ? lv_color_black()
                                             : lv_color_white(), 0);
}

static void ring_build_(void)
{
    ring_cont_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ring_cont_, 412, 412);
    lv_obj_set_pos(ring_cont_, 0, 0);
    lv_obj_set_style_bg_color(ring_cont_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ring_cont_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ring_cont_, 0, 0);
    lv_obj_set_style_pad_all(ring_cont_, 0, 0);
    lv_obj_clear_flag(ring_cont_, LV_OBJ_FLAG_SCROLLABLE);
    /* Rings, largest first so smaller ones stack on top. */
    for (size_t i = 0; i < sizeof(RING_TABLE) / sizeof(RING_TABLE[0]); i++) {
        int r = RING_TABLE[i].r;
        lv_obj_t *o = lv_obj_create(ring_cont_);
        lv_obj_set_size(o, 2 * r, 2 * r);
        lv_obj_set_pos(o, 206 - r, 206 - r);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(RING_TABLE[i].color), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    }
    /* Crosshair through the center (dark gray, visible on all rings). */
    lv_obj_t *h = lv_obj_create(ring_cont_);
    lv_obj_set_size(h, 412, 2);
    lv_obj_set_pos(h, 0, 205);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_t *v = lv_obj_create(ring_cont_);
    lv_obj_set_size(v, 2, 412);
    lv_obj_set_pos(v, 205, 0);
    lv_obj_set_style_bg_color(v, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(v, 0, 0);
    /* Radius numbers at 3 o'clock (every 20 px) and 12 o'clock
     * (every 40 px): text sits just inside its ring's edge. */
    for (size_t i = 0; i < sizeof(RING_TABLE) / sizeof(RING_TABLE[0]); i++) {
        int r = RING_TABLE[i].r;
        uint32_t c = RING_TABLE[i].color;
        /* Crude luminance: dark text on bright rings. */
        int lum = (int)((c >> 16 & 0xFF) * 3 + (c >> 8 & 0xFF) * 5 +
                        (c & 0xFF) * 2) / 10;
        if (r % 20 == 0) {
            ring_label_(206 + r - 36, 206 - 20, r, lum > 110);
        }
        if (r % 40 == 0) {
            ring_label_(206 - 13, 206 - r + 3, r, lum > 110);
        }
    }
    lv_obj_add_flag(ring_cont_, LV_OBJ_FLAG_HIDDEN);
}

void display_show_ring_test(void)
{
    if (!ready_) {
        return;
    }
    if (msg_visible_) {
        display_hide_message();
    }
    if (lvgl_port_lock(0)) {
        if (!ring_cont_) {
            ring_build_();
        }
        lv_obj_add_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(media_cont_, LV_OBJ_FLAG_HIDDEN);
        kb_visible_ = false;
        lv_obj_clear_flag(ring_cont_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ring_cont_);
        ring_visible_ = true;
        lvgl_port_unlock();
    }
}

void display_hide_ring_test(void)
{
    if (!ready_ || !ring_cont_ || !ring_visible_) {
        return;
    }
    if (lvgl_port_lock(0)) {
        lv_obj_add_flag(ring_cont_, LV_OBJ_FLAG_HIDDEN);
        ring_visible_ = false;
        lv_obj_clear_flag(status_cont_, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

bool display_ring_test_visible(void)
{
    return ring_visible_;
}


/* NCP-1 revB: Pairing modal with touchscreen Yes/No.
 * Three variants: NUMCMP (blue, 6 digits + buttons), PASSKEY (blue, 6 digits,
 * no buttons — host types it), CONSENT (amber, no number, explicit warning).
 * Buttons call hid_ble_pair_confirm directly (safe from LVGL task). */
static lv_obj_t *pair_modal_ = NULL;
static lv_obj_t *pair_title_ = NULL;
static lv_obj_t *pair_num_label_ = NULL;
static lv_obj_t *pair_sub_ = NULL;
static lv_obj_t *pair_footer_ = NULL;
static lv_obj_t *pair_btn_accept_ = NULL;
static lv_obj_t *pair_btn_reject_ = NULL;
static uint32_t pair_modal_gen_ = 0;
static int pair_selected_ = 0;  /* 0=Accept, 1=Reject (wheel nav) */
static display_pair_variant_t pair_variant_ = DISPLAY_PAIR_NUMCMP;

/* Forward: defined after display_pair_show. */
static void pair_update_selection(void);

static void pair_btn_cb(lv_event_t *e)
{
    bool accept = (bool)(uintptr_t)lv_event_get_user_data(e);
    /* Generation-guarded: stale taps after dismiss are dropped by
     * hid_ble_pair_confirm (returns INVALID_STATE). */
    esp_err_t rc = hid_ble_pair_confirm(pair_modal_gen_, accept);
    if (rc == ESP_OK) {
        ESP_LOGI("display", "pair %s (gen=%u)",
                 accept ? "accepted" : "rejected", (unsigned)pair_modal_gen_);
    }
    /* Dismiss immediately on tap — the ENC_CHANGE/DISCONNECT handler will
     * also dismiss, but the UI should feel instant. */
    display_pair_hide();
}

void display_pair_show(display_pair_variant_t variant, uint32_t gen, uint32_t num)
{
    if (!ready_) {
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        ESP_LOGW("display", "pair_show: lvgl lock timeout");
        return;
    }

    bool is_consent = (variant == DISPLAY_PAIR_CONSENT);
    bool show_buttons = (variant != DISPLAY_PAIR_PASSKEY);
    uint32_t accent = is_consent ? 0xffaa33 : 0x33ddff;  /* amber vs blue */

    if (!pair_modal_) {
        pair_modal_ = lv_obj_create(lv_scr_act());
        lv_obj_set_size(pair_modal_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_opa(pair_modal_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pair_modal_, 0, 0);
        lv_obj_set_style_pad_all(pair_modal_, 20, 0);

        pair_title_ = lv_label_create(pair_modal_);
        lv_obj_set_style_text_font(pair_title_, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(pair_title_, lv_color_hex(0xffffff), 0);
        lv_obj_align(pair_title_, LV_ALIGN_TOP_MID, 0, 30);

        pair_num_label_ = lv_label_create(pair_modal_);
        lv_obj_set_style_text_font(pair_num_label_, &lv_font_montserrat_32, 0);
        lv_obj_align(pair_num_label_, LV_ALIGN_CENTER, 0, -30);

        pair_sub_ = lv_label_create(pair_modal_);
        lv_obj_set_style_text_color(pair_sub_, lv_color_hex(0xaaaaaa), 0);
        lv_obj_align(pair_sub_, LV_ALIGN_CENTER, 0, 30);

        /* Footer hint (e.g. "Hold wheel to cancel" on PASSKEY). */
        pair_footer_ = lv_label_create(pair_modal_);
        lv_obj_set_style_text_color(pair_footer_, lv_color_hex(0x666666), 0);
        lv_obj_align(pair_footer_, LV_ALIGN_BOTTOM_MID, 0, -10);

        /* Accept/Reject buttons side by side. */
        pair_btn_accept_ = lv_btn_create(pair_modal_);
        lv_obj_set_size(pair_btn_accept_, 140, 60);
        lv_obj_align(pair_btn_accept_, LV_ALIGN_BOTTOM_LEFT, 20, -30);
        lv_obj_set_style_bg_color(pair_btn_accept_, lv_color_hex(0x22aa44), 0);
        lv_obj_t *al = lv_label_create(pair_btn_accept_);
        lv_label_set_text(al, "Accept");
        lv_obj_center(al);
        lv_obj_add_event_cb(pair_btn_accept_, pair_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)true);

        pair_btn_reject_ = lv_btn_create(pair_modal_);
        lv_obj_set_size(pair_btn_reject_, 140, 60);
        lv_obj_align(pair_btn_reject_, LV_ALIGN_BOTTOM_RIGHT, -20, -30);
        lv_obj_set_style_bg_color(pair_btn_reject_, lv_color_hex(0xaa2222), 0);
        lv_obj_t *rl = lv_label_create(pair_btn_reject_);
        lv_label_set_text(rl, "Reject");
        lv_obj_center(rl);
        lv_obj_add_event_cb(pair_btn_reject_, pair_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)false);
    }

    /* Configure per variant. */
    pair_modal_gen_ = gen;
    pair_variant_ = variant;
    pair_selected_ = 0;  /* default to Accept */
    lv_obj_set_style_bg_color(pair_modal_, lv_color_hex(is_consent ? 0x2a1a0a : 0x111122), 0);
    lv_obj_set_style_text_color(pair_num_label_, lv_color_hex(accent), 0);

    if (is_consent) {
        lv_label_set_text(pair_title_, "Unverified computer");
        lv_label_set_text(pair_num_label_, "!");
        lv_label_set_text(pair_sub_,
            "This computer can't confirm\na pairing code. Allow it to\nconnect and receive keystrokes?");
    } else if (variant == DISPLAY_PAIR_PASSKEY) {
        lv_label_set_text(pair_title_, "Pairing code");
        char buf[16];
        snprintf(buf, sizeof(buf), "%03u %03u",
                 (unsigned)(num / 1000u), (unsigned)(num % 1000u));
        lv_label_set_text(pair_num_label_, buf);
        lv_label_set_text(pair_sub_, "Type this number\non the computer");
        lv_label_set_text(pair_footer_, "Hold wheel to cancel");
    } else {  /* NUMCMP */
        lv_label_set_text(pair_title_, "Pair with computer?");
        char buf[16];
        snprintf(buf, sizeof(buf), "%03u %03u",
                 (unsigned)(num / 1000u), (unsigned)(num % 1000u));
        lv_label_set_text(pair_num_label_, buf);
        lv_label_set_text(pair_sub_,
            "Confirm the same number\nis on the computer screen");
    }

    /* Buttons only where the user must decide. */
    if (show_buttons) {
        lv_obj_clear_flag(pair_btn_accept_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pair_btn_reject_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pair_btn_accept_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(pair_btn_reject_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_clear_flag(pair_modal_, LV_OBJ_FLAG_HIDDEN);
    pair_update_selection();
    lvgl_port_unlock();
}

void display_pair_hide(void)
{
    if (!ready_ || !pair_modal_) {
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(500))) {
        ESP_LOGW("display", "pair_hide: lvgl lock timeout");
        return;
    }
    lv_obj_add_flag(pair_modal_, LV_OBJ_FLAG_HIDDEN);
    pair_modal_gen_ = 0;
    lvgl_port_unlock();
}

/* NCP-1: Wheel navigation fallback functions. */

bool display_pair_visible(void)
{
    if (!ready_ || !pair_modal_) {
        return false;
    }
    return !lv_obj_has_flag(pair_modal_, LV_OBJ_FLAG_HIDDEN);
}

static void pair_update_selection(void)
{
    /* Highlight selected button with a white border. */
    lv_obj_set_style_border_width(pair_btn_accept_,
                                  pair_selected_ == 0 ? 3 : 0, 0);
    lv_obj_set_style_border_color(pair_btn_accept_,
                                  lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(pair_btn_reject_,
                                  pair_selected_ == 1 ? 3 : 0, 0);
    lv_obj_set_style_border_color(pair_btn_reject_,
                                  lv_color_hex(0xffffff), 0);
}

void display_pair_wheel_nav(int detents)
{
    if (!display_pair_visible()) {
        return;
    }
    /* NCP-1 revC: Wheel nav only where there's a decision to make.
     * PASSKEY is display-only (no buttons). */
    if (pair_variant_ == DISPLAY_PAIR_PASSKEY) {
        return;
    }
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) {
        return;
    }
    /* Any rotation toggles selection (only 2 buttons). */
    if (detents != 0) {
        pair_selected_ = 1 - pair_selected_;
        pair_update_selection();
    }
    lvgl_port_unlock();
}

void display_pair_wheel_click(void)
{
    if (!display_pair_visible()) {
        return;
    }
    /* NCP-1 revC: No-op for PASSKEY (display-only, nothing to confirm). */
    if (pair_variant_ == DISPLAY_PAIR_PASSKEY) {
        return;
    }
    bool accept = (pair_selected_ == 0);
    esp_err_t rc = hid_ble_pair_confirm(pair_modal_gen_, accept);
    if (rc == ESP_OK) {
        ESP_LOGI("display", "pair wheel %s (gen=%u)",
                 accept ? "accepted" : "rejected", (unsigned)pair_modal_gen_);
    }
    display_pair_hide();
}
