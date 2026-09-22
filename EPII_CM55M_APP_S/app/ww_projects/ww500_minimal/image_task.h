/*
 * image_task.h
 *
 *  Created on: 22 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that owns the HM0360 camera. A light-weight version of the image task in ww500_md, which does a
 *  great deal more (EXIF, neural network, exposure control, flash LED, timers).
 *
 *  What it does:
 *   - Initialises the HM0360 at start-up. The camera supply is always on, so the sensor keeps its registers (and its
 *     operating mode) through DPD: the long register table is written only after a cold boot. The data path in the
 *     HX6538 does not survive DPD, so it is initialised at every boot.
 *   - Leaves the sensor in a resting mode (default mode 2, MODE_SW_NFRAMES_SLEEP) whenever it is not taking a picture.
 *   - Takes a picture only when asked (the CLI 'capture' command): mode 2 for one frame, then the JPEG is passed to
 *     the FatFS task to be written as Bnnnnnnn.JPG, where nnnnnnn is the boot count (5 digits) and the number of the
 *     picture in this boot (2 digits). There is no EXIF.
 *   - Changes the resting mode when asked (the CLI 'cam' command), so the current in each mode can be measured.
 *   - Takes part in the shutdown barrier, so that DPD is never entered while a picture is being taken or written.
 *
 *  All the camera I2C and data path calls are made by this task.
 */

#ifndef IMAGE_TASK_H_
#define IMAGE_TASK_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ww500_minimal.h"

/********************************************** Global Defines ***********************************************/

// The value of the 'mode' in image_task_requestMode() that asks for the mode to be reported, not changed
#define IMAGE_TASK_MODE_REPORT			0xFF

/*********************************************** Global Types ************************************************/

// The states of the task. Values must match the strings in image_task.c
typedef enum {
	IMAGE_TASK_STATE_UNINIT,
	IMAGE_TASK_STATE_NO_CAMERA,
	IMAGE_TASK_STATE_IDLE,
	IMAGE_TASK_STATE_CAPTURING,
	IMAGE_TASK_STATE_WRITING,
	IMAGE_TASK_NUMSTATES
} IMAGE_TASK_STATE_E;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 */
TaskHandle_t image_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason);

/**
 * @brief Returns the internal state as a number.
 */
uint16_t image_task_getState(void);

/**
 * @brief Returns the internal state as a string.
 */
const char * image_task_getStateString(void);

/**
 * @brief Returns true while a picture is being taken or written.
 */
bool image_task_isBusy(void);

/**
 * @brief Asks the task to take a picture and save it. The result is printed when it is known.
 */
bool image_task_requestCapture(void);

/**
 * @brief Asks the task to set the resting mode of the HM0360, or (IMAGE_TASK_MODE_REPORT) to print the mode.
 */
bool image_task_requestMode(uint8_t mode);

/**
 * @brief Asks the task to write the HM0360 register table again, as after a cold boot.
 */
bool image_task_requestReinit(void);

/**
 * @brief Tells the task that all tasks are inactive, so it should finish what it is doing and get ready for DPD.
 */
void image_task_notifyInactivity(void);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_TASK_H_ */
