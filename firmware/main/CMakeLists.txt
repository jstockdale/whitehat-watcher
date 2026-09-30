idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES hid_ble link eyes net display nvs_flash json mode timer led ptt wakeword hal)
