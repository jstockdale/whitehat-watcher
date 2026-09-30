idf_component_register(SRCS "wheel.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver espressif__knob eyes esp_io_expander_pca95xx_16bit)
