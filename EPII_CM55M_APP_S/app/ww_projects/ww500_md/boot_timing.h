/*
 * boot_timing.h
 *
 *  Created on: 30 Sep 2026
 *      Author: Charles Palmer
 *
 *  Times the stages from the start of app_main() to the first frame after a boot, and prints them once (issue #249:
 *  how long after a motion wake the first photo is taken).
 *
 *  The time before the FreeRTOS scheduler starts comes from the core's cycle counter (DWT CYCCNT), which is reliable
 *  there as nothing sleeps. After the scheduler starts, the FreeRTOS tick count is added (1 ms resolution): the cycle
 *  counter stops while the core sleeps in (tickless) idle, but the tick count is kept correct.
 *
 *  The SD card points (mounted, CONFIG.TXT loaded, image directory ready) depend on the card: its speed, and what is
 *  on it, since directories are searched one entry at a time. Compare cards and card contents, not just builds.
 *
 *  Not counted: the boot ROM and bootloader before app_main(). On a motion wake the HM0360's SEN_INT stays asserted
 *  until the first frame arrives (image_task.c clears it there), so a scope on SEN_INT gives the whole time from
 *  motion to the first frame; the difference from the "frame ready" figure here is the time before app_main().
 */

#ifndef BOOT_TIMING_H_
#define BOOT_TIMING_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

/********************************************** Global Defines ***********************************************/

// 1 to record and print the boot timing, 0 to compile it all out
#define BOOT_TIMING_ENABLED				1

/*********************************************** Global Types ************************************************/

// The points timed, in the order they normally happen
typedef enum {
	BOOT_TIMING_CAMERAS_CHECKED,	// checkForCameras() finished (app_main())
	BOOT_TIMING_SCHEDULER,			// just before vTaskStartScheduler()
	BOOT_TIMING_SD_MOUNTED,			// FatFS task: SD card mounted (fatFsInit())
	BOOT_TIMING_SD_CONFIG_LOADED,	// FatFS task: CONFIG.TXT loaded (load_configuration())
	BOOT_TIMING_SD_IMAGE_DIR_READY,	// FatFS task: image directory set up (dir_mgr_init_image_dir())
	BOOT_TIMING_SD_READY,			// FatFS task: start-up finished, image task released (xSDInitDoneSemaphore)
	BOOT_TIMING_CAMERA_READY,		// image task: camera (and HM0360 for MD) initialised
	BOOT_TIMING_CAPTURE_START,		// image task: capture started (configure_image_sensor(CAMERA_CONFIG_RUN))
	BOOT_TIMING_FRAME_READY,		// image task: first frame ready
	BOOT_TIMING_NUM_POINTS
} BOOT_TIMING_POINT_E;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Starts the timing. Call first thing in app_main().
 */
void boot_timing_start(void);

/**
 * @brief Records the time of a point, the first time it is reached in this boot.
 */
void boot_timing_mark(BOOT_TIMING_POINT_E point);

/**
 * @brief Prints the times recorded, once per boot.
 */
void boot_timing_report(const char *wakeReason);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_TIMING_H_ */
