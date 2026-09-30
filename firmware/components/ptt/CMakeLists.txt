idf_component_register(SRCS "ptt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver espressif__esp_codec_dev wheel link led json wakeword task_psram)
