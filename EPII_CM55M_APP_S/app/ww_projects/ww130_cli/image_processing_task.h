/*
 * image_processing_task.h
 *
 *  Created on: 16 Sept 2024
 *      Author: TP
 *
 * This task handles camera & NN operations. This involves setting up and operating as an I2C slave,
 * and operating PA0 as a bidirectional interrupt pin.
 */

#ifndef APP_WW_PROJECTS_WW130_CLI_IMAGETASK_H_
#define APP_WW_PROJECTS_WW130_CLI_IMAGETASK_H_

#include <stdio.h>
#include <stdlib.h>

// The states for the image_processing_task
// APP_FATFS_STATE_NUMSTATES is only used to establish the number of states
typedef enum
{
    APP_IMAGE_CAPTURE_STATE_UNINIT = 0x0000,
    APP_IMAGE_CAPTURE_STATE_IDLE = 0x0001,
    APP_IMAGE_CAPTURE_STATE_CAPTURING = 0x0002,
    APP_IMAGE_NN_STATE_UNINIT = 0x0003,
    APP_IMAGE_NN_STATE_IDLE = 0x0004,
    APP_IMAGE_NN_STATE_PROCESSING = 0x0005,
    APP_IMAGE_NUMSTATES = 0x0006
} APP_IMAGE_STATE_E;

// Create the task and all its support pieces
TaskHandle_t image_createTask(int8_t priority);

// Return the internal state (as a number)
uint16_t image_task_getState(void);

// Return the internal state (as a string)
const char *image_task_getStateString(void);

#endif /* APP_WW_PROJECTS_WW130_CLI_IMAGETASK_H_ */