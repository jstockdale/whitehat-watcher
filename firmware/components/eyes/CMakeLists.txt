idf_component_register(SRCS "eyes.c" "png_stream.c" "png_crash_detector.c"
                       INCLUDE_DIRS "include"
                       REQUIRES sscma_client esp_io_expander_pca95xx_16bit task_psram
                                esp_http_client esp_timer json nvs_flash led)
