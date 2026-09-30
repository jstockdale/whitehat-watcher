# PTT I2C Driver Conflict Fix (v0.3.0)

## Problem
Device boot-looped with:
```
E (1343) i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
abort() was called at PC 0x420757a3 on core 0
```

The `ptt` component uses `espressif/esp_codec_dev` for the ES7243 mic codec.
By default on IDF 5.3+, esp_codec_dev uses the new I2C driver (`driver_ng`,
`driver/i2c_master.h`). The rest of the firmware (eyes/I2C0, touch, wheel/PCA9535)
uses the legacy I2C driver (`driver/i2c.h`). ESP-IDF aborts when driver_ng is
used on a port where the legacy driver is installed.

## Fix (Option 1: use legacy driver in esp_codec_dev)
The esp_codec_dev component has a Kconfig option `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE`
that forces it to use the legacy `driver/i2c.h` API instead of driver_ng.

### Changes
1. **sdkconfig.defaults**: Added `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=y`
   - This makes `audio_codec_ctrl_i2c.c` take the `#else` branch (legacy driver)
   - `USE_IDF_I2C_MASTER` is NOT defined; no driver_ng calls are compiled
   - The legacy path uses `i2c_master_cmd_begin(I2C_NUM_0, ...)` on the port
     already installed by `eyes_init()` — no driver re-installation needed

2. **managed_components/espressif__esp_codec_dev/CMakeLists.txt** (local patch):
   Added `driver` to `priv_requires` unconditionally.
   - When BACKWARD_COMPATIBLE=y, the source includes `driver/i2c.h`, but the
     component's CMakeLists only listed `esp_driver_i2c` (new driver) for IDF>=5.3
   - Without this, build fails: "driver component(s) is not in the requirements list"
   - **NOTE**: This is a local patch to a managed component. If the component is
     re-downloaded (version change or directory deletion), re-apply this one-line fix.

### Verification
- `idf.py build`: clean, no errors
- Preprocessor check: `USE_IDF_I2C_MASTER` is NOT defined when compiling
  `audio_codec_ctrl_i2c.c` with the new config
- Merged binary rebuilt and verified (app magic 0xE9 at 0x20000)
- By code inspection: no `i2c_new_master_bus()` or `i2c_master_bus_add_device()`
  calls exist in the compiled I2C control path

## Why not migrate everything to driver_ng?
The task specified: "Do NOT migrate the rest of the firmware to driver_ng — that's
a much bigger change." The eyes, touch, and wheel components all use the legacy
driver extensively. Making PTT conform to the existing driver is the minimal,
safest fix.
