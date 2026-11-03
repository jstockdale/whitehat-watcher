set(srcs "src/sscma_client_ops.c"
         "src/sscma_client_io.c"
         "src/sscma_client_io_i2c.c"
         "src/sscma_client_io_spi.c"
         "src/sscma_client_io_uart.c"
         "src/sscma_client_flasher.c"
         "src/sscma_client_flasher_we2_uart.c"
         "src/sscma_client_flasher_we2_spi.c"
         )
set(includes "include" "interface")
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(json_component "espressif__cjson")
    set(driver_components "driver" "esp_driver_gpio" "esp_driver_i2c" "esp_driver_spi" "esp_driver_uart")
else()
    set(json_component "json")
    set(driver_components "driver")
endif()
set(require "${json_component}" "mbedtls" "esp_timer")
set(priv_requires ${driver_components})

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes}
                       REQUIRES ${require}
                       PRIV_REQUIRES ${priv_requires}
                       )
# v0.4.14 Phase 1: sscma_client_read_bulk() has no in-tree caller yet (the
# frame-pull path will call it). The xtensa GCC 13.2 ignores
# __attribute__((retain)) and __attribute__((used)) does not survive
# --gc-sections, so force the linker to keep the symbol via -u.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,sscma_client_read_bulk")
