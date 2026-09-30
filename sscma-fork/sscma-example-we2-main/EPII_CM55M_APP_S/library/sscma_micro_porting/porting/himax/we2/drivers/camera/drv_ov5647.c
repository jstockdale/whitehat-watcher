#include "drv_ov5647.h"

#include <WE2_core.h>
#include <WE2_device_addr.h>
#include <core/ma_debug.h>
#include <ctype.h>
#include <drv_shared_cfg.h>
#include <hx_drv_CIS_common.h>
#include <hx_drv_hxautoi2c_mst.h>
#include <hx_drv_inp.h>
#include <hx_drv_scu.h>
#include <hx_drv_scu_export.h>
#include <hx_drv_timer.h>
#include <ma_config_board.h>
#include <porting/ma_misc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drv_shared_cfg.h"
#include "drv_shared_state.h"

#define OV5647_MIPI_640X480   0
#define OV5647_MIPI_2592X1944 1
#define OV5647_MIPI_1296X972  2

#define OV5647_MIPI_MODE      OV5647_MIPI_1296X972

#if (OV5647_MIPI_MODE == OV5647_MIPI_640X480)
    #define OV5647_MIPI_CLOCK_FEQ 220  // MHz
    #define OV5647_SENSOR_WIDTH   640
    #define OV5647_SENSOR_HEIGHT  480
    #define OV5647_SUB_SAMPLE     INP_SUBSAMPLE_DISABLE
    #define OV5647_BINNING_0      INP_BINNING_DISABLE
    #define OV5647_BINNING_1      INP_BINNING_4TO2_B
    #define OV5647_BINNING_2      INP_BINNING_8TO2_B
#elif (OV5647_MIPI_MODE == OV5647_MIPI_2592X1944)
    #define OV5647_MIPI_CLOCK_FEQ 350  // MHz
    #define OV5647_SENSOR_WIDTH   2592
    #define OV5647_SENSOR_HEIGHT  1944
    #define OV5647_SUB_SAMPLE     INP_SUBSAMPLE_4TO2
    #define OV5647_BINNING_0      INP_BINNING_4TO2_B
    #define OV5647_BINNING_1      INP_BINNING_8TO2_B
    #define OV5647_BINNING_2      INP_BINNING_16TO2_B
#elif (OV5647_MIPI_MODE == OV5647_MIPI_1296X972)
    #define OV5647_MIPI_CLOCK_FEQ 350  // MHz
    #define OV5647_SENSOR_WIDTH   1296
    #define OV5647_SENSOR_HEIGHT  972
    #define OV5647_SUB_SAMPLE     INP_SUBSAMPLE_DISABLE
    #define OV5647_BINNING_0      INP_BINNING_4TO2_B
    #define OV5647_BINNING_1      INP_BINNING_8TO2_B
    #define OV5647_BINNING_2      INP_BINNING_16TO2_B
#else
    #error "OV5647_MIPI_MODE not defined"
#endif

#define OV5647_SENSOR_I2CID     0x36
#define OV5647_MIPI_LANE_CNT    2
#define OV5647_MIPI_DPP         10    // depth per pixel
#define OV5647_MIPITX_CNTCLK_EN 1     // continuous clock output enable
#define OV5647_MIPI_CTRL_OFF    0x01  // MIPI OFF
#define OV5647_MIPI_CTRL_ON     0x14  // MIPI ON
#define OV5647_LANE_NB          2

static HX_CIS_SensorSetting_t OV5647_init_setting[] = {
#if (OV5647_MIPI_MODE == OV5647_MIPI_640X480)
    #include "OV5647_mipi_2lane_640x480.i"
#elif (OV5647_MIPI_MODE == OV5647_MIPI_2592X1944)
    #include "OV5647_mipi_2lane_2592x1944.i"
#elif (OV5647_MIPI_MODE == OV5647_MIPI_1296X972)
    #include "OV5647_mipi_2lane_1296x972.i"
#else
    #error "OV5647_MIPI_MODE not defined"
#endif
};

#define CIS_I2C_ID                OV5647_SENSOR_I2CID
#define SENSORDPLIB_SENSOR_OV5647 (SENSORDPLIB_SENSOR_HM2130)

static HX_CIS_SensorSetting_t OV5647_stream_on[] = {
  {HX_CIS_I2C_Action_W, 0x4800, OV5647_MIPI_CTRL_ON},
  {HX_CIS_I2C_Action_W, 0x4202,                0x00},
};

static HX_CIS_SensorSetting_t OV5647_stream_off[] = {
  {HX_CIS_I2C_Action_W, 0x4800, OV5647_MIPI_CTRL_OFF},
  {HX_CIS_I2C_Action_W, 0x4202,                 0x0F},
};

static HX_CIS_SensorSetting_t OV5647_mirror_setting[] = {
  {HX_CIS_I2C_Action_W, 0x0101,       0x00},
  {HX_CIS_I2C_Action_W, 0x3821, 0b00000111},
};

ma_err_t drv_ov5647_probe() {
    if (hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)DEAULT_XHSUTDOWN_PIN, SENSORCTRL_MCLK_DIV3) != HX_CIS_NO_ERROR) {
        MA_LOGD(MA_TAG, "Camera CIS Init Fail");
        return MA_FAILED;
    }

    MA_CAMERA_PWR_CTRL_INIT_F;
    hx_drv_timer_cm55x_delay_ms(10, TIMER_STATE_DC);

    if (hx_drv_cis_set_slaveID(CIS_I2C_ID) != HX_CIS_NO_ERROR) {
        return MA_FAILED;
    }

    uint8_t data;
    if (hx_drv_cis_get_reg(0x0000, &data) != HX_CIS_NO_ERROR) {
        return MA_FAILED;
    }
    MA_LOGD(MA_TAG, "OV5647 ID: %02X", data);

    return MA_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 1: Hardware ROI (crop-zoom)                                    */
/*                                                                      */
/* ROI coordinates are in sensor pixels for the active OV5647 mode      */
/* (1296x972 by default). The INP crop sits before binning in the      */
/* datapath (SENSOR -> INP_CROP -> INP_BINNING -> INP_SUBSAMPLE), so    */
/* binning factors are computed from the ROI dimensions.               */
/*                                                                      */
/* _roi_w == 0 means ROI disabled (full sensor). The INP crop operates  */
/* on Bayer data, so x/y/w/h are rounded down to even values.          */
/* ------------------------------------------------------------------ */
#define OV5647_ROI_MIN_SIZE 96

static uint16_t _roi_x = 0;
static uint16_t _roi_y = 0;
static uint16_t _roi_w = 0; /* 0 = disabled */
static uint16_t _roi_h = 0; /* 0 = disabled */

ma_err_t drv_ov5647_set_roi(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    /* w/h of 0 disables the ROI (full sensor) */
    if (w == 0 || h == 0) {
        _roi_x = 0;
        _roi_y = 0;
        _roi_w = 0;
        _roi_h = 0;
        MA_LOGD(MA_TAG, "ROI disabled (full sensor)");
        return MA_OK;
    }

    /* INP crop operates on Bayer data: coordinates must be even */
    if (x & 1) {
        MA_LOGW(MA_TAG, "ROI x %u odd, rounding down", x);
        x &= (uint16_t)~1u;
    }
    if (y & 1) {
        MA_LOGW(MA_TAG, "ROI y %u odd, rounding down", y);
        y &= (uint16_t)~1u;
    }
    if (w & 1) {
        MA_LOGW(MA_TAG, "ROI w %u odd, rounding down", w);
        w &= (uint16_t)~1u;
    }
    if (h & 1) {
        MA_LOGW(MA_TAG, "ROI h %u odd, rounding down", h);
        h &= (uint16_t)~1u;
    }

    if (w < OV5647_ROI_MIN_SIZE || h < OV5647_ROI_MIN_SIZE) {
        MA_LOGE(MA_TAG, "ROI %ux%u smaller than minimum %dx%d", w, h, OV5647_ROI_MIN_SIZE, OV5647_ROI_MIN_SIZE);
        return MA_EINVAL;
    }

    if (x >= OV5647_SENSOR_WIDTH || y >= OV5647_SENSOR_HEIGHT || w > (uint16_t)(OV5647_SENSOR_WIDTH - x) ||
        h > (uint16_t)(OV5647_SENSOR_HEIGHT - y)) {
        MA_LOGE(MA_TAG, "ROI (%u,%u,%u,%u) outside sensor %dx%d", x, y, w, h, OV5647_SENSOR_WIDTH,
                OV5647_SENSOR_HEIGHT);
        return MA_EINVAL;
    }

    _roi_x = x;
    _roi_y = y;
    _roi_w = w;
    _roi_h = h;
    MA_LOGD(MA_TAG, "ROI set to (%u,%u,%u,%u)", x, y, w, h);
    return MA_OK;
}

ma_err_t drv_ov5647_get_roi(uint16_t* x, uint16_t* y, uint16_t* w, uint16_t* h) {
    if (!x || !y || !w || !h) {
        return MA_EINVAL;
    }
    if (_roi_w == 0 || _roi_h == 0) {
        /* ROI disabled: report full sensor */
        *x = 0;
        *y = 0;
        *w = OV5647_SENSOR_WIDTH;
        *h = OV5647_SENSOR_HEIGHT;
    } else {
        *x = _roi_x;
        *y = _roi_y;
        *w = _roi_w;
        *h = _roi_h;
    }
    return MA_OK;
}

static uint32_t ov5647_set_dp_rc96() {
    SCU_PDHSC_DPCLK_CFG_T cfg;

    if (hx_drv_scu_get_pdhsc_dpclk_cfg(&cfg) != SCU_NO_ERROR) {
        MA_LOGE(MA_TAG, "Get SCU PDHSC DPCLK CFG failed");
    }

    cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_RC96M48M;
    cfg.mipiclk.hscmipiclkdiv = 0;
    cfg.dpclk                 = SCU_HSCDPCLKSRC_RC96M48M;

    if (hx_drv_scu_set_pdhsc_dpclk_cfg(cfg, 0, 1) != SCU_NO_ERROR) {
        MA_LOGE(MA_TAG, "Set SCU PDHSC DPCLK CFG failed");
    }

    uint32_t mipi_pixel_clk = 96;
    if (hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK, &mipi_pixel_clk) != SCU_NO_ERROR) {
        MA_LOGE(MA_TAG, "Get SCU HSC MIPI RXCLK failed");
    }
    mipi_pixel_clk = mipi_pixel_clk / 1000000;

    return mipi_pixel_clk;
}

static void set_mipi_csirx_enable() {
    uint32_t bitrate_1lane  = OV5647_MIPI_CLOCK_FEQ * 2;
    uint32_t mipi_lnno      = OV5647_MIPI_LANE_CNT;
    uint32_t pixel_dpp      = OV5647_MIPI_DPP;
    uint32_t line_length    = OV5647_SENSOR_WIDTH;
    uint32_t frame_length   = OV5647_SENSOR_HEIGHT;
    uint32_t byte_clk       = bitrate_1lane / 8;
    uint32_t continuousout  = OV5647_MIPITX_CNTCLK_EN;
    uint32_t deskew_en      = 0;
    uint32_t mipi_pixel_clk = ov5647_set_dp_rc96();

    MA_LOGD(MA_TAG, "MIPI Bitrate: %d", bitrate_1lane);
    MA_LOGD(MA_TAG, "MIPI Lane: %d", mipi_lnno);
    MA_LOGD(MA_TAG, "MIPI Pixel Depth: %d", pixel_dpp);
    MA_LOGD(MA_TAG, "MIPI Line Length: %d", line_length);
    MA_LOGD(MA_TAG, "MIPI Frame Length: %d", frame_length);
    MA_LOGD(MA_TAG, "MIPI Byte Clock: %d", byte_clk);
    MA_LOGD(MA_TAG, "MIPI Continuous Clock: %d", continuousout);
    MA_LOGD(MA_TAG, "MIPI Pixel Clock: %d", mipi_pixel_clk);

    uint32_t n_preload = 15;
    uint32_t l_header  = 4;
    uint32_t l_footer  = 2;

    double t_input   = (double)(l_header + line_length * pixel_dpp / 8 + l_footer) / (mipi_lnno * byte_clk) + 0.06;
    double t_output  = (double)line_length / mipi_pixel_clk;
    double t_preload = (double)(7 + (n_preload * 4) / mipi_lnno) / mipi_pixel_clk;
    double delta_t   = t_input - t_output - t_preload;

    MA_LOGD(MA_TAG, "MIPI Input: %dns", (uint32_t)(t_input * 1000));
    MA_LOGD(MA_TAG, "MIPI Output: %dns", (uint32_t)(t_output * 1000));
    MA_LOGD(MA_TAG, "MIPI Preload: %dns", (uint32_t)(t_preload * 1000));

    uint16_t rx_fifo_fill = 0;
    uint16_t tx_fifo_fill = 0;

    if (delta_t <= 0) {
        delta_t      = 0 - delta_t;
        tx_fifo_fill = ceil(delta_t * byte_clk * mipi_lnno / 4 / (pixel_dpp / 2)) * (pixel_dpp / 2);
        rx_fifo_fill = 0;
    } else {
        rx_fifo_fill = ceil(delta_t * byte_clk * mipi_lnno / 4 / (pixel_dpp / 2)) * (pixel_dpp / 2);
        tx_fifo_fill = 0;
    }

    MA_LOGD(MA_TAG, "MIPI RX FIFO Fill: %d", rx_fifo_fill);
    MA_LOGD(MA_TAG, "MIPI TX FIFO Fill: %d", tx_fifo_fill);

    MA_LOGD(MA_TAG, "MIPI Reset");

    SCU_DP_SWRESET_T dp_swrst;
    if (drv_interface_get_dp_swreset(&dp_swrst) != 0) {
        MA_LOGE(MA_TAG, "Get DP SW Reset failed");
    }
    dp_swrst.HSC_MIPIRX = 0;
    dp_swrst.HSC_MIPITX = 0;

    if (hx_drv_scu_set_DP_SWReset(dp_swrst) != SCU_NO_ERROR) {
        MA_LOGE(MA_TAG, "Set DP SW Reset failed");
    }
    hx_drv_timer_cm55x_delay_us(50, TIMER_STATE_DC);

    dp_swrst.HSC_MIPIRX = 1;
    dp_swrst.HSC_MIPITX = 1;
    if (hx_drv_scu_set_DP_SWReset(dp_swrst) != SCU_NO_ERROR) {
        MA_LOGE(MA_TAG, "Set DP SW Reset failed");
    }

    MIPIRX_DPHYHSCNT_CFG_T hscnt_cfg;
    hscnt_cfg.mipirx_dphy_hscnt_clk_en = 0;
    hscnt_cfg.mipirx_dphy_hscnt_ln0_en = 1;
    hscnt_cfg.mipirx_dphy_hscnt_ln1_en = 1;

    hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
    hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x06;
    hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x06;

    sensordplib_csirx_set_hscnt(hscnt_cfg);

    if (pixel_dpp == 10 || pixel_dpp == 8) {
        sensordplib_csirx_set_pixel_depth(pixel_dpp);
    } else {
        MA_LOGE(MA_TAG, "Invalid pixel depth %d", pixel_dpp);
        return;
    }

    sensordplib_csirx_set_deskew(deskew_en);
    sensordplib_csirx_set_fifo_fill(rx_fifo_fill);
    sensordplib_csirx_enable(mipi_lnno);

    MA_LOGD(MA_TAG, "MIPI VMUTE: 0x%08X", *(uint32_t*)(SCU_LSC_ADDR + 0x408));
    MA_LOGD(MA_TAG, "0x53061000: 0x%08X", *(uint32_t*)(CSITX_REGS_BASE + 0x1000));
    MA_LOGD(MA_TAG, "0x53061004: 0x%08X", *(uint32_t*)(CSITX_REGS_BASE + 0x1004));
    MA_LOGD(MA_TAG, "0x53061008: 0x%08X", *(uint32_t*)(CSITX_REGS_BASE + 0x1008));
    MA_LOGD(MA_TAG, "0x5306100C: 0x%08X", *(uint32_t*)(CSITX_REGS_BASE + 0x100C));
    MA_LOGD(MA_TAG, "0x53061010: 0x%08X", *(uint32_t*)(CSITX_REGS_BASE + 0x1010));
}

static void set_mipi_csirx_disable() {
    MA_LOGD(MA_TAG, "MIPI Disable");

    volatile uint32_t* dphy_reg                  = CSIRX_DPHY_REG;
    volatile uint32_t* csi_static_cfg_reg        = (CSIRX_REGS_BASE + 0x08);
    volatile uint32_t* csi_dphy_lane_control_reg = (CSIRX_REGS_BASE + 0x40);
    volatile uint32_t* csi_stream0_control_reg   = (CSIRX_REGS_BASE + 0x100);
    volatile uint32_t* csi_stream0_data_cfg      = (CSIRX_REGS_BASE + 0x108);
    volatile uint32_t* csi_stream0_cfg_reg       = (CSIRX_REGS_BASE + 0x10C);

    sensordplib_csirx_disable();

    MA_LOGD(MA_TAG, "MIPI DPHY: 0x%08X 0x%08X", dphy_reg, *dphy_reg);
    MA_LOGD(MA_TAG, "MIPI Static CFG: 0x%08X 0x%08X", csi_static_cfg_reg, *csi_static_cfg_reg);
    MA_LOGD(MA_TAG, "MIPI Lane Control: 0x%08X 0x%08X", csi_dphy_lane_control_reg, *csi_dphy_lane_control_reg);
    MA_LOGD(MA_TAG, "MIPI Stream Data CFG: 0x%08X 0x%08X", csi_stream0_data_cfg, *csi_stream0_data_cfg);
    MA_LOGD(MA_TAG, "MIPI Stream Control: 0x%08X 0x%08X", csi_stream0_control_reg, *csi_stream0_control_reg);
    MA_LOGD(MA_TAG, "MIPI Stream CFG: 0x%08X 0x%08X", csi_stream0_cfg_reg, *csi_stream0_cfg_reg);
}

ma_err_t drv_ov5647_init(uint16_t width, uint16_t height, int compression) {
    if (!width || !height || width > FRAME_WIDTH_MAX || height > FRAME_HEIGHT_MAX) {
        MA_LOGE(MA_TAG, "Invalid resolution %dx%d", width, height);
        return MA_EINVAL;
    }

    _drv_dp_event_cb_on_frame_ready = NULL;
    _drv_dp_on_next_stream          = NULL;

    ma_err_t    ret = MA_OK;
    int         ec;
    HW5x5_CFG_T hw5x5_cfg;
    JPEG_CFG_T  jpeg_cfg;
    uint16_t    start_x;
    uint16_t    start_y;
    uint16_t    w;
    uint16_t    h;
    INP_CROP_T  crop;

#if ENABLE_SENSOR_FAST_SWITCH
    MA_LOGD(MA_TAG, "Camera Initiated Before: %d", _initiated_before);
    if (!_initiated_before) {
#endif
        MA_LOGD(MA_TAG, "Camera Init Pin: %d", DEAULT_XHSUTDOWN_PIN);
        ec = hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)DEAULT_XHSUTDOWN_PIN, SENSORCTRL_MCLK_DIV3);
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Camera CIS Init Fail: %d", ec);
            ret = MA_EIO;
            goto err;
        }

        MA_CAMERA_PWR_CTRL_INIT_F;
        hx_drv_timer_cm55x_delay_ms(10, TIMER_STATE_DC);

        ec = hx_drv_cis_set_slaveID(CIS_I2C_ID);
        MA_LOGD(MA_TAG, "Camera Set Slave ID %d: %d", CIS_I2C_ID, ec);
        if (ec != HX_CIS_NO_ERROR) {
            ret = MA_EIO;
            goto err;
        }

        hx_drv_timer_cm55x_delay_ms(3, TIMER_STATE_DC);

        ec = hx_drv_cis_setRegTable(OV5647_stream_off, HX_CIS_SIZE_N(OV5647_stream_off, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Camera Set Stream Off Fail: %d", ec);
            ret = MA_EIO;
            goto err;
        }

        ec = hx_drv_cis_setRegTable(OV5647_init_setting, HX_CIS_SIZE_N(OV5647_init_setting, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Camera Set Init Setting Fail: %d", ec);
            ret = MA_EIO;
            goto err;
        }

        ec =
          hx_drv_cis_setRegTable(OV5647_mirror_setting, HX_CIS_SIZE_N(OV5647_mirror_setting, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Camera Set Mirror Setting Fail: %d", ec);
        }

        set_mipi_csirx_enable();
#if ENABLE_SENSOR_FAST_SWITCH
    }
#endif

    /* Phase 1: apply stored ROI as the INP crop. INP_CROP_T uses inclusive
       last coordinates (max HSIZE-1 per hx_drv_inp.h). The crop is applied
       before binning in the datapath, so binning factors are computed from
       the ROI dimensions rather than the full sensor. */
    uint16_t src_w = OV5647_SENSOR_WIDTH;
    uint16_t src_h = OV5647_SENSOR_HEIGHT;
    if (_roi_w && _roi_h) {
        crop.start_x = _roi_x;
        crop.start_y = _roi_y;
        crop.last_x  = (uint16_t)(_roi_x + _roi_w - 1);
        crop.last_y  = (uint16_t)(_roi_y + _roi_h - 1);
        src_w        = _roi_w;
        src_h        = _roi_h;
        MA_LOGD(MA_TAG, "ROI crop: start(%u,%u) last(%u,%u)", crop.start_x, crop.start_y, crop.last_x, crop.last_y);
    } else {
        crop.start_x = 0;
        crop.start_y = 0;
        crop.last_x  = 0;
        crop.last_y  = 0;
    }

    if (src_w < width || src_h < height) {
        MA_LOGE(MA_TAG, "ROI %ux%u smaller than requested output %ux%u", src_w, src_h, width, height);
        ret = MA_EINVAL;
        goto err;
    }

    int32_t factor_w = floor((float)src_w / (float)width);
    int32_t factor_h = floor((float)src_h / (float)height);
    int32_t min_f    = factor_w < factor_h ? factor_w : factor_h;

    if (min_f >= 8) {
        w  = (uint16_t)(src_w / 8);
        h  = (uint16_t)(src_h / 8);
        ec = sensordplib_set_sensorctrl_inp_wi_crop_bin(SENSORDPLIB_SENSOR_OV5647,
                                                        SENSORDPLIB_STREAM_NONEAOS,
                                                        OV5647_SENSOR_WIDTH,
                                                        OV5647_SENSOR_HEIGHT,
                                                        OV5647_SUB_SAMPLE,
                                                        crop,
                                                        OV5647_BINNING_2);
    } else if (min_f >= 4) {
        w  = (uint16_t)(src_w / 4);
        h  = (uint16_t)(src_h / 4);
        ec = sensordplib_set_sensorctrl_inp_wi_crop_bin(SENSORDPLIB_SENSOR_OV5647,
                                                        SENSORDPLIB_STREAM_NONEAOS,
                                                        OV5647_SENSOR_WIDTH,
                                                        OV5647_SENSOR_HEIGHT,
                                                        OV5647_SUB_SAMPLE,
                                                        crop,
                                                        OV5647_BINNING_1);
    } else if (min_f >= 2) {
        w  = (uint16_t)(src_w / 2);
        h  = (uint16_t)(src_h / 2);
        ec = sensordplib_set_sensorctrl_inp_wi_crop_bin(SENSORDPLIB_SENSOR_OV5647,
                                                        SENSORDPLIB_STREAM_NONEAOS,
                                                        OV5647_SENSOR_WIDTH,
                                                        OV5647_SENSOR_HEIGHT,
                                                        OV5647_SUB_SAMPLE,
                                                        crop,
                                                        OV5647_BINNING_0);
    } else {
        w  = src_w;
        h  = src_h;
        ec = sensordplib_set_sensorctrl_inp_wi_crop_bin(SENSORDPLIB_SENSOR_OV5647,
                                                        SENSORDPLIB_STREAM_NONEAOS,
                                                        OV5647_SENSOR_WIDTH,
                                                        OV5647_SENSOR_HEIGHT,
                                                        OV5647_SUB_SAMPLE,
                                                        crop,
                                                        INP_BINNING_DISABLE);
    }

    if (ec != 0) {
        MA_LOGE(MA_TAG, "Camera Set Sensor Control Fail: %d", ec);
        ret = MA_EIO;
        goto err;
    }

    start_x = (w - width) / 2;
    start_y = (h - height) / 2;

    MA_LOGD(MA_TAG, "Start X: %d Start Y: %d Width: %d Height: %d", start_x, start_y, width, height);

    _reset_all_wdma_buffer();

    _frame.width  = width;
    _frame.height = height;
    _frame.rotate = MA_PIXEL_ROTATE_0;
    _frame.format = MA_PIXEL_FORMAT_YUV422;
    _frame.size   = YUV422_SIZE_EXP(width, height);
    _frame.data   = _wdma3_baseaddr;
    _jpeg.width   = width;
    _jpeg.height  = height;
    _jpeg.rotate  = MA_PIXEL_ROTATE_0;
    _jpeg.format  = MA_PIXEL_FORMAT_JPEG;
    _jpeg.size    = JPEG_BASE_SIZE_EXP(width, height);
    _jpeg.data    = _wdma2_baseaddr;

    MA_LOGD(MA_TAG, "Frame Size: %d JPEG Size: %d", _frame.size, _jpeg.size);
    MA_LOGD(MA_TAG, "Frame Data: %x JPEG Data: %x", _frame.data, _jpeg.data);

#if ENABLE_SENSOR_FAST_SWITCH
    MA_LOGD(MA_TAG, "Camera Initiated Before: %d", _initiated_before);
    if (!_initiated_before) {
#endif
        sensordplib_set_xDMA_baseaddrbyapp(_wdma1_baseaddr, _wdma2_baseaddr, _wdma3_baseaddr);
        sensordplib_set_jpegfilesize_addrbyapp(_jpegsize_baseaddr);
#if ENABLE_SENSOR_FAST_SWITCH
    }
#endif

    hw5x5_cfg.hw5x5_path         = HW5x5_PATH_THROUGH_DEMOSAIC;
    hw5x5_cfg.demos_bndmode      = DEMOS_BNDODE_REFLECT;
    hw5x5_cfg.demos_color_mode   = DEMOS_COLORMODE_YUV422;
    hw5x5_cfg.demos_pattern_mode = DEMOS_PATTENMODE_GRBG;
    hw5x5_cfg.demoslpf_roundmode = DEMOSLPF_ROUNDMODE_ROUNDING;
    hw5x5_cfg.hw55_crop_stx      = start_x;
    hw5x5_cfg.hw55_crop_sty      = start_y;
    hw5x5_cfg.hw55_in_width      = width;
    hw5x5_cfg.hw55_in_height     = height;

    jpeg_cfg.jpeg_path    = JPEG_PATH_ENCODER_EN;
    jpeg_cfg.enc_width    = width;
    jpeg_cfg.enc_height   = height;
    jpeg_cfg.jpeg_enctype = JPEG_ENC_TYPE_YUV422;

    switch (compression) {
    case 10:
        jpeg_cfg.jpeg_encqtable = JPEG_ENC_QTABLE_10X;
        break;
    case 4:
        jpeg_cfg.jpeg_encqtable = JPEG_ENC_QTABLE_4X;
        break;
    default:
        MA_LOGE(MA_TAG, "Invalid compression %d", compression);
        ret = MA_EINVAL;
        goto err;
    }

    sensordplib_set_int_hw5x5_jpeg_wdma23(hw5x5_cfg, jpeg_cfg, 1, NULL);

#if ENABLE_SENSOR_FAST_SWITCH
    MA_LOGD(MA_TAG, "Camera Initiated Before: %d", _initiated_before);
    if (!_initiated_before) {
#endif

        hx_dplib_register_cb(_drv_dp_event_cb, SENSORDPLIB_CB_FUNTYPE_DP);

        ec = hx_drv_cis_setRegTable(OV5647_stream_on, HX_CIS_SIZE_N(OV5647_stream_on, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Camera Set Stream On Fail: %d", ec);
            ret = MA_EIO;
            goto err;
        }

        sensordplib_set_mclkctrl_xsleepctrl_bySCMode();

#if ENABLE_SENSOR_FAST_SWITCH
    }
#endif

    ec = sensordplib_set_sensorctrl_start();
    if (ec != 0) {
        MA_LOGE(MA_TAG, "Camera Set Sensor Control Start Fail: %d", ec);
        ret = MA_FAILED;
        goto err;
    }

    _frame_ready = false;
    ++_frame_count;
    sensordplib_retrigger_capture();

    _initiated_before = true;

    MA_LOGD(MA_TAG, "Camera Init Success!");

    return ret;

err:
    MA_LOGD(MA_TAG, "Camera Init Failed!");

#if ENABLE_SENSOR_FAST_SWITCH
    _initiated_before = false;

    sensordplib_stop_capture();
    sensordplib_start_swreset();
    sensordplib_stop_swreset_WoSensorCtrl();

    ec = hx_drv_cis_setRegTable(OV5647_stream_off, HX_CIS_SIZE_N(OV5647_stream_off, HX_CIS_SensorSetting_t));
    if (ec != HX_CIS_NO_ERROR) {
        MA_LOGE(MA_TAG, "Camera Set Stream Off Fail: %d", ec);
    }

    set_mipi_csirx_disable();

#endif

    hx_drv_sensorctrl_set_xSleep(1);

    return ret;
}

void drv_ov5647_deinit() {
    MA_LOGD(MA_TAG, "Camera Deinit");

    int ec;

    sensordplib_stop_capture();

#if !ENABLE_SENSOR_FAST_SWITCH
    _frame_count = 0;

    sensordplib_start_swreset();
    sensordplib_stop_swreset_WoSensorCtrl();

    ec = hx_drv_cis_setRegTable(OV5647_stream_off, HX_CIS_SIZE_N(OV5647_stream_off, HX_CIS_SensorSetting_t));
    if (ec != HX_CIS_NO_ERROR) {
        MA_LOGE(MA_TAG, "Camera Set Stream Off Fail: %d", ec);
    }

    set_mipi_csirx_disable();
#endif

    hx_drv_sensorctrl_set_xSleep(1);

    hx_drv_timer_cm55x_delay_ms(3, TIMER_STATE_DC);
}

#ifdef CAPTURE_ONLY_MODE
/* v0.4.4 experimental: full-frame raw capture support.
 * The raw frame buffer is allocated by the caller (CameraHimax) from the
 * heap (TFLM arena freed in capture-only mode) and registered here.
 * drv_ov5647_raw_dump() uses this buffer instead of the 600KB YUV buffer
 * when a raw buffer is registered. */
static uint8_t* _raw_frame_buf      = NULL;
static size_t   _raw_frame_buf_size = 0;
static uint8_t  _raw_frame_bits     = 0; /* 8 = RAW8, 10 = packed RAW10 */

void drv_ov5647_set_raw_buffer(uint8_t* buf, size_t size, uint8_t bits) {
    _raw_frame_buf      = buf;
    _raw_frame_buf_size = size;
    _raw_frame_bits     = bits;
    MA_LOGD(MA_TAG, "Raw buffer: %p %lu bytes %u-bit", buf, (unsigned long)size, bits);
}

void drv_ov5647_clear_raw_buffer(void) {
    _raw_frame_buf      = NULL;
    _raw_frame_buf_size = 0;
    _raw_frame_bits     = 0;
}

/* Initialize the sensor/INP for full-resolution raw output.
 * Unlike drv_ov5647_init(), this skips the HW5x5 (demosaic) and JPEG
 * datapath setup — the raw path goes Sensor->INP->WDMA2 directly.
 * The actual WDMA2 configuration happens in drv_ov5647_raw_dump() at
 * capture time. */
ma_err_t drv_ov5647_init_raw(uint16_t width, uint16_t height, uint8_t bits) {
    if (!width || !height || width > RAW_FRAME_WIDTH_MAX || height > RAW_FRAME_HEIGHT_MAX) {
        MA_LOGE(MA_TAG, "Invalid raw resolution %dx%d", width, height);
        return MA_EINVAL;
    }
    if (bits != 8 && bits != 10) {
        MA_LOGE(MA_TAG, "Invalid raw bits %u (must be 8 or 10)", bits);
        return MA_EINVAL;
    }

    ma_err_t ret = MA_OK;
    int      ec;

#if ENABLE_SENSOR_FAST_SWITCH
    if (!_initiated_before) {
#endif
        MA_LOGD(MA_TAG, "Raw: Camera Init Pin: %d", DEAULT_XHSUTDOWN_PIN);
        ec = hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)DEAULT_XHSUTDOWN_PIN, SENSORCTRL_MCLK_DIV3);
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Raw: Camera CIS Init Fail: %d", ec);
            return MA_EIO;
        }

        MA_CAMERA_PWR_CTRL_INIT_F;
        hx_drv_timer_cm55x_delay_ms(10, TIMER_STATE_DC);

        ec = hx_drv_cis_set_slaveID(CIS_I2C_ID);
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Raw: Camera Set Slave ID Fail: %d", ec);
            return MA_EIO;
        }

        hx_drv_timer_cm55x_delay_ms(3, TIMER_STATE_DC);

        ec = hx_drv_cis_setRegTable(OV5647_stream_off, HX_CIS_SIZE_N(OV5647_stream_off, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Raw: Camera Set Stream Off Fail: %d", ec);
            return MA_EIO;
        }

        ec = hx_drv_cis_setRegTable(OV5647_init_setting, HX_CIS_SIZE_N(OV5647_init_setting, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Raw: Camera Set Init Setting Fail: %d", ec);
            return MA_EIO;
        }

        ec = hx_drv_cis_setRegTable(OV5647_mirror_setting, HX_CIS_SIZE_N(OV5647_mirror_setting, HX_CIS_SensorSetting_t));
        if (ec != HX_CIS_NO_ERROR) {
            MA_LOGE(MA_TAG, "Raw: Camera Set Mirror Setting Fail");
        }

        set_mipi_csirx_enable();
#if ENABLE_SENSOR_FAST_SWITCH
    }
#endif

    /* For full-resolution raw (1296x972 from a 1296x972 sensor), use no
     * binning (1:1). The INP crop is disabled (full sensor). */
    INP_CROP_T crop;
    crop.start_x = 0;
    crop.start_y = 0;
    crop.last_x  = 0;
    crop.last_y  = 0;

    ec = sensordplib_set_sensorctrl_inp_wi_crop_bin(SENSORDPLIB_SENSOR_OV5647,
                                                    SENSORDPLIB_STREAM_NONEAOS,
                                                    OV5647_SENSOR_WIDTH,
                                                    OV5647_SENSOR_HEIGHT,
                                                    OV5647_SUB_SAMPLE,
                                                    crop,
                                                    INP_BINNING_DISABLE);
    if (ec != 0) {
        MA_LOGE(MA_TAG, "Raw: Camera Set Sensor Control Fail: %d", ec);
        return MA_EIO;
    }

    /* Record the frame dimensions for drv_ov5647_raw_dump(). */
    _frame.width  = width;
    _frame.height = height;
    _frame.rotate = MA_PIXEL_ROTATE_0;
    /* No RAW8/RAW10 in ma_pixel_format_t; use GRAYSCALE (1B/px) for RAW8
     * and UNKNOWN for packed RAW10. The actual packing is described by
     * _raw_frame_bits and the AT+RAWCAP response. */
    _frame.format = (bits == 8) ? MA_PIXEL_FORMAT_GRAYSCALE : MA_PIXEL_FORMAT_UNKNOWN;
    _frame.size   = (bits == 8) ? RAW8_FRAME_SIZE(width, height) : RAW10_FRAME_SIZE(width, height);
    /* _frame.data is set at capture time from the registered raw buffer. */

    MA_LOGD(MA_TAG, "Raw init: %ux%u %u-bit, frame size %lu", width, height, bits, (unsigned long)_frame.size);

#if ENABLE_SENSOR_FAST_SWITCH
    if (!_initiated_before) {
#endif
        /* Register the WDMA base addresses. WDMA2 will be re-pointed to the
         * raw buffer at capture time by drv_ov5647_raw_dump(). */
        sensordplib_set_xDMA_baseaddrbyapp(_wdma1_baseaddr, _wdma2_baseaddr, _wdma3_baseaddr);
        sensordplib_set_jpegfilesize_addrbyapp(_jpegsize_baseaddr);
#if ENABLE_SENSOR_FAST_SWITCH
    }
#endif

    return ret;
}
#endif /* CAPTURE_ONLY_MODE */

/* Phase 3 (AT+RAWDUMP): WDMA2 raw format probe.
 * See phase3-rawdump-design.md for the full design. */

static volatile bool _raw_dump_frame_done = false;

static void _raw_dump_dp_cb(SENSORDPLIB_STATUS_E event) {
    if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
        _raw_dump_frame_done = true;
    }
}

ma_err_t drv_ov5647_raw_dump(drv_raw_dump_result_t* out) {
    ma_err_t ret = MA_OK;

    if (out == NULL) {
        return MA_EINVAL;
    }
    if (!_initiated_before) {
        MA_LOGE(MA_TAG, "RawDump: camera not initialized");
        return MA_EPERM;
    }

    uint16_t w = _frame.width;
    uint16_t h = _frame.height;
    if (w == 0 || h == 0) {
        MA_LOGE(MA_TAG, "RawDump: invalid INP output size %ux%u", w, h);
        return MA_EINVAL;
    }

#ifdef CAPTURE_ONLY_MODE
    /* v0.4.4: use the registered full-frame raw buffer if available,
     * otherwise fall back to the YUV422 frame buffer. */
    uint8_t* buf;
    uint32_t buf_sz;
    if (_raw_frame_buf != NULL && _raw_frame_buf_size > 0) {
        buf    = _raw_frame_buf;
        buf_sz = (uint32_t)_raw_frame_buf_size;
        MA_LOGD(MA_TAG, "RawDump: using registered raw buffer %p %lu bytes", buf, (unsigned long)buf_sz);
    } else {
        /* Capture into the YUV422 frame buffer: 600KB, unused while the raw
         * path is active. Worst case (16-bit) needs w*h*2 bytes. */
        buf    = (uint8_t*)_wdma3_baseaddr;
        buf_sz = YUV422_BASE_SIZE;
    }
#else
    /* Capture into the YUV422 frame buffer: 600KB, unused while the raw
     * path is active. Worst case (16-bit) needs w*h*2 bytes. */
    uint8_t* buf    = (uint8_t*)_wdma3_baseaddr;
    uint32_t buf_sz = YUV422_BASE_SIZE;
#endif
    uint32_t need_max = (uint32_t)w * (uint32_t)h * 2u;
    if (need_max > buf_sz) {
        MA_LOGE(MA_TAG, "RawDump: %ux%u needs %lu > %lu buffer", w, h,
                (unsigned long)need_max, (unsigned long)buf_sz);
        return MA_ENOMEM;
    }

    memset(buf, 0xAA, buf_sz);

    /* Halt the JPEG datapath, point WDMA2 at our buffer, install one-shot
     * callback, then bring up Sensor->INP->WDMA2. */
    sensordplib_stop_capture();
#ifdef CAPTURE_ONLY_MODE
    if (_raw_frame_buf != NULL && _raw_frame_buf_size > 0) {
        /* Point WDMA2 at the registered raw buffer. */
        sensordplib_set_xDMA_baseaddrbyapp(_wdma1_baseaddr, (uint32_t)_raw_frame_buf, _wdma3_baseaddr);
    } else {
        sensordplib_set_xDMA_baseaddrbyapp(_wdma1_baseaddr, _wdma3_baseaddr, _wdma3_baseaddr);
    }
#else
    sensordplib_set_xDMA_baseaddrbyapp(_wdma1_baseaddr, _wdma3_baseaddr, _wdma3_baseaddr);
#endif
    _raw_dump_frame_done = false;
    hx_dplib_register_cb(_raw_dump_dp_cb, SENSORDPLIB_CB_FUNTYPE_DP);
    sensordplib_set_raw_wdma2(w, h, NULL);

    int ec = sensordplib_set_sensorctrl_start();
    if (ec != 0) {
        MA_LOGE(MA_TAG, "RawDump: sensorctrl_start failed %d", ec);
        ret = MA_EIO;
        goto restore_cb;
    }
    sensordplib_retrigger_capture();

    /* Wait for frame-ready, 2s timeout in 5ms steps. */
    uint32_t waited_ms = 0;
    while (!_raw_dump_frame_done && waited_ms < 2000) {
        hx_drv_timer_cm55x_delay_ms(5, TIMER_STATE_DC);
        waited_ms += 5;
    }
    sensordplib_stop_capture();

    if (!_raw_dump_frame_done) {
        MA_LOGE(MA_TAG, "RawDump: frame-ready timeout");
        ret = MA_ETIMEOUT;
        goto restore_cb;
    }

    /* Measure actual bytes written: scan back over the 0xAA fill. */
    uint32_t n = buf_sz;
    while (n > 0 && buf[n - 1] == 0xAA) {
        --n;
    }

    out->width         = w;
    out->height        = h;
    out->bytes_written = n;
    out->capture_ms    = waited_ms;
    out->min_val       = 0xFF;
    out->max_val       = 0x00;
    for (int i = 0; i < DRV_RAW_DUMP_HIST_BINS; ++i) {
        out->hist[i] = 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t v = buf[i];
        out->hist[v >> 4]++;
        if (v < out->min_val) out->min_val = v;
        if (v > out->max_val) out->max_val = v;
    }
    uint32_t head_n = n < DRV_RAW_DUMP_HEAD_BYTES ? n : DRV_RAW_DUMP_HEAD_BYTES;
    memcpy(out->head, buf, head_n);
    if (head_n < DRV_RAW_DUMP_HEAD_BYTES) {
        memset(out->head + head_n, 0, DRV_RAW_DUMP_HEAD_BYTES - head_n);
    }

    MA_LOGD(MA_TAG, "RawDump: %ux%u -> %lu bytes in %lums", w, h,
            (unsigned long)n, (unsigned long)waited_ms);

restore_cb:
    hx_dplib_register_cb(_drv_dp_event_cb, SENSORDPLIB_CB_FUNTYPE_DP);
    return ret;
}
