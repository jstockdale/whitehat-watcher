#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Arm the crash detector before PNG encode (stage 1=bulk done, 2=encode, 3=streaming) */
void png_crash_detector_arm(uint32_t stage);

/* Disarm after successful completion */
void png_crash_detector_disarm(void);

/* Check on boot. Returns stage if crash detected, 0 otherwise. */
uint32_t png_crash_detector_check(void);

/* Print crash report on boot (call early in app_main) */
void png_crash_detector_report(void);

#ifdef __cplusplus
}
#endif
