#ifndef __SSCMA_H__
#define __SSCMA_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef FREERTOS
    #include "FreeRTOS.h"
    #include "queue.h"
    #include "task.h"
    #include "timers.h"
#endif

#ifdef TRUSTZONE_SEC
    #if (__ARM_FEATURE_CMSE & 1) == 0
        #error "Need ARMv8-M security extensions"
    #elif (__ARM_FEATURE_CMSE & 2) == 0
        #error "Compile with --cmse"
    #endif
    #include "arm_cmse.h"
    #ifdef NSC
        #include "veneer_table.h"
    #endif

    #ifndef TRUSTZONE_SEC_ONLY

        #include "secure_port_macros.h"
    #endif
#endif

int app_main(void);

#ifdef __cplusplus
}
#endif

#endif