idf_component_register(SRCS "link.c"
                       INCLUDE_DIRS "include"
                       REQUIRES hid_ble eyes net json display touch wheel input mode timer led wakeword task_psram seal)
