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
// Create the task and all its support pieces
#include <stdlib.h>

#define APP_BLOCK_FUNC()          \
    do                            \
    {                             \
        __asm volatile("b    ."); \
    } while (0)

// The camera states for the image_processing_task
typedef enum
{
    APP_IMAGE_CAPTURE_STATE_UNINIT = 0x0000,
    APP_IMAGE_CAPTURE_STATE_INIT = 0x0001,
    APP_IMAGE_CAPTURE_STATE_SETUP_CAP_START = 0x0002,
    APP_IMAGE_CAPTURE_STATE_SETUP_CAP_END = 0x0003,
    APP_IMAGE_CAPTURE_STATE_RECAP_FRAME = 0x0004,
    APP_IMAGE_CAPTURE_STATE_CAP_FRAMERDY = 0x0005,
    APP_IMAGE_CAPTURE_STATE_STOP_CAP_START = 0x0006,
    APP_IMAGE_CAPTURE_STATE_STOP_CAP_END = 0x0007,
    APP_IMAGE_CAPTURE_STATE_IDLE = 0x0008,
    APP_IMAGE_CAPTURE_NUMSTATE = 0x0009,
    // APP_IMAGE_CAPTURE_STATE_ERROR,
} APP_IMAGE_CAPTURE_STATE_E;

// The NN states for the image_processing_task
typedef enum
{
    APP_IMAGE_NN_STATE_UNINIT = 0x0000,
    APP_IMAGE_NN_STATE_IDLE = 0x0001,
    APP_IMAGE_NN_STATE_PROCESSING = 0x0002,
    APP_IMAGE_NUMSTATES = 0x0003
} APP_IMAGE_NN_STATE_E;

// The states for the image
typedef enum
{
    APP_STATE_ALLON,
    APP_STATE_RESTART,
    APP_STATE_STOP,
} APP_STATE_E;

// Create the task and all its support pieces
TaskHandle_t image_createTask(int8_t priority);

// Return the internal state (as a number)
uint16_t image_task_getState(void);

// Return the internal state (as a string)
const char *image_task_getStateString(void);

// Set the state of capturing
void app_start_state(APP_STATE_E state);
void image_var_int(void);

#endif /* APP_WW_PROJECTS_WW130_CLI_IMAGETASK_H_ */