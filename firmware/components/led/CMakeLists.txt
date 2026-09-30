idf_component_register(SRCS "led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES espressif__led_strip esp_timer)
