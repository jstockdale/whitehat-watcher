idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_lcd esp_lvgl_port lvgl eyes touch hid_ble
                                espressif__esp_lcd_spd2010
                                esp_io_expander_pca95xx_16bit)
