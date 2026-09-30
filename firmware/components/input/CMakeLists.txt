idf_component_register(SRCS "input.c"
                       INCLUDE_DIRS "include"
                       REQUIRES touch wheel hid_ble mode freertos timer display ptt led task_psram)
