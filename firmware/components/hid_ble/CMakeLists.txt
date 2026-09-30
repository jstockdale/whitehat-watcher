idf_component_register(SRCS "hid_ble.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt nvs_flash led task_psram)
