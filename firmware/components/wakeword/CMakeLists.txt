idf_component_register(SRCS "wakeword.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash driver espressif__esp-sr task_psram)
