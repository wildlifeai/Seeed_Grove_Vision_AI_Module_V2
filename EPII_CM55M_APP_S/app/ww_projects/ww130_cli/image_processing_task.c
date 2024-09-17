/*
 * image_processing_task.c
 *
 *  Created on: 16 Sept 2024
 *      Author: TP
 *
 * FreeRTOS task
 *
 * This task handles camera & NN operations. This involves setting up and operating as an I2C slave,
 * and operating PA0 as a bidirectional interrupt pin.
 */

/*************************************** Includes *******************************************/

#include <stdio.h>
#include <stdlib.h>
#include "WE2_device.h"
#include "WE2_device_addr.h"
#include "WE2_core.h"
#include "board.h"

#include "printf_x.h"
#include "xprintf.h"

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

#include "image_processing_task.h"
#include "task1.h"
#include "app_msg.h"
#include "if_task.h"
#include "fatfs_task.h"
#include "ww130_cli.h"
#include "i2c_comm.h"

#include "WE2_debug.h"

#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "cisdp_sensor.h"
#include "cisdp_cfg.h"

#include "crc16_ccitt.h"

/*************************************** Definitions *******************************************/

// TODO sort out how to allocate priorities
#define image_task_PRIORITY (configMAX_PRIORITIES - 4)

#define IMAGE_TASK_QUEUE_LEN 10

#define DRV ""

// TODO - is this the best way of managing errors?
#define APP_BLOCK_FUNC()          \
    do                            \
    {                             \
        __asm volatile("b    ."); \
    } while (0)

/*************************************** Local Function Declarations *****************************/

static void vImageTask(void *pvParameters);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForCaptureIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T rxMessage);

static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage);

// Strings for each of these states. Values must match APP_TASK1_STATE_E in task1.h
const char *imageTaskStateString[APP_IMAGE_NUMSTATES] = {
    "Image Capture Uninitialised",
    "Image Capture Idle",
    "Image Capturing",
    "NN Uninitialised",
    "NN Idle",
    "NN Processing"};

// Strings for expected messages. Values must match messages directed to image Task in app_msg.h
const char *imageTaskEventString[APP_MSG_IMAGETASK_LAST - APP_MSG_IMAGETASK_FIRST] = {
    "Start Capture",
    "Stop Capture",
    "Start NN",
    "Stop NN"};

/*************************************** External variables *******************************************/

extern SemaphoreHandle_t xI2CTxSemaphore;

/*************************************** Local variables *******************************************/

// This is the handle of the task
TaskHandle_t image_task_id;
QueueHandle_t xImageTaskQueue;
volatile APP_IMAGE_STATE_E image_task_state = APP_IMAGE_CAPTURE_STATE_UNINIT;

// Image processing variables
static uint8_t g_frame_ready;
static uint32_t g_cur_jpegenc_frame;
static uint8_t g_spi_master_initial_status;
uint32_t jpeg_addr, jpeg_sz;
uint32_t g_img_data = 0;

static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage)
{
    APP_MSG_EVENT_E event;
    // uint32_t data;
    APP_MSG_DEST_T sendMsg;
    sendMsg.destination = NULL;

    event = rxMessage.msg_event;
    // data = rxMessage.msg_data;

    switch (event)
    {
    case APP_MSG_IMAGETASK_START_CAPTURE:
        // Start NN processing
        image_task_state = APP_IMAGE_CAPTURE_STATE_CAPTURING;

        // Should this be RESTART or ALLON?
        app_start_state(APP_STATE_RESTART);

        rxMessage.msg_data = 0;
        rxMessage.msg_event = APP_MSG_MAINEVENT_START_CAPTURE;
        if (xQueueSend(xImageTaskQueue, (void *)&rxMessage, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send rxMessage=0x%x fail\r\n", rxMessage.msg_event);
        }
        break;

    case APP_MSG_IMAGETASK_STOP_CAPTURE:
        // Stop NN processing
        image_task_state = APP_IMAGE_CAPTURE_STATE_IDLE;
        cisdp_sensor_stop();
        rxMessage.msg_data = rxMessage.msg_event;
        rxMessage.msg_event = APP_MSG_MAINEVENT_STOP_CAPTURE;
        if (xQueueSend(xImageTaskQueue, (void *)&rxMessage, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send rxMessage=0x%x fail\r\n", rxMessage.msg_event);
        }
        break;

    default:
        // Unexpected event
        flagUnexpectedEvent(rxMessage);
        break;
    }
    return sendMsg;
}

static APP_MSG_DEST_T handleEventForNNIdle(APP_MSG_T rxMessage)
{
    APP_MSG_EVENT_E event;
    // uint32_t data;
    APP_MSG_DEST_T sendMsg;
    sendMsg.destination = NULL;

    event = rxMessage.msg_event;
    // data = rxMessage.msg_data;

    switch (event)
    {
    case APP_MSG_IMAGETASK_START_NN:
        // Start NN processing
        image_task_state = APP_IMAGE_NN_STATE_PROCESSING;
        break;
    case APP_MSG_IMAGETASK_STOP_NN:
        // Stop NN processing
        image_task_state = APP_IMAGE_NN_STATE_IDLE;
        break;
    default:
        // Unexpected event
        flagUnexpectedEvent(rxMessage);
        break;
    }
    return sendMsg;
}

static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T rxMessage)
{
    APP_MSG_EVENT_E event;
    // uint32_t data;
    APP_MSG_DEST_T sendMsg;
    sendMsg.destination = NULL;

    event = rxMessage.msg_event;
    // data = rxMessage.msg_data;

    switch (event)
    {
    case APP_MSG_IMAGETASK_START_NN:
        // Start NN processing
        image_task_state = APP_IMAGE_NN_STATE_PROCESSING;
        break;
    case APP_MSG_IMAGETASK_STOP_NN:
        // Stop NN processing
        image_task_state = APP_IMAGE_NN_STATE_IDLE;
        break;
    default:
        // Unexpected event
        flagUnexpectedEvent(rxMessage);
        break;
    }
    return sendMsg;
}

static APP_MSG_DEST_T handleEventForCaptureIdle(APP_MSG_T rxMessage)
{
    APP_MSG_DEST_T txMessage;
    APP_MSG_EVENT_E event;
    uint32_t rxData;
    uint32_t length;

    event = rxMessage.msg_event;
    rxData = rxMessage.msg_data;

    length = rxMessage.msg_parameter;
    if (length > WW130_MAX_PAYLOAD_SIZE)
    {
        length = WW130_MAX_PAYLOAD_SIZE;
    }

    switch (event)
    {
    case APP_MSG_IMAGETASK_START_CAPTURE:
        // // Start capturing an image
        // image_task_state = APP_IMAGE_CAPTURE_STATE_CAPTURING;
        // // ADD CAPTURING FUNC HERE

        // // Set the state to capturing
        // image_task_state = APP_IMAGE_CAPTURE_STATE_IDLE;
        // // The message to send depends on the destination! In retrospect it would have been better
        // // if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
        // if (sendMsg.destination == xIfTaskQueue)
        // {
        //     sendMsg.message.msg_event = APP_MSG_IMAGETASK_CAPTURE_STARTED;
        // }
        // sendMsg.message.msg_data = 0;
        image_task_state = APP_IMAGE_CAPTURE_STATE_CAPTURING;
        break;

    case APP_MSG_IMAGETASK_START_NN:
        // // Start NN processing
        // image_task_state = APP_IMAGE_NN_STATE_PROCESSING;
        // // ADD NN FUNC HERE

        // // Set the state to processing
        // image_task_state = APP_IMAGE_NN_STATE_IDLE;
        // if (sendMsg.destination == xIfTaskQueue)
        // {
        //     sendMsg.message.msg_event = APP_MSG_IMAGETASK_NN_STARTED;
        // }
        // sendMsg.message.msg_data = 0;
        image_task_state = APP_IMAGE_NN_STATE_PROCESSING;
        break;

    default:
        // Unexpected event
        flagUnexpectedEvent(rxMessage);
        break;
    }

    return txMessage;
}

static void vImageTask(void *pvParameters)
{
    APP_MSG_T rxMessage;
    APP_MSG_DEST_T txMessage;
    QueueHandle_t targetQueue;

    APP_IMAGE_STATE_E old_state;
    const char *eventString;
    APP_MSG_EVENT_E event;
    uint32_t rxData;

    for (;;)
    {
        // Wait for a message in the queue
        if (xQueueReceive(xImageTaskQueue, &(rxMessage), __QueueRecvTicksToWait) == pdTRUE)
        {
            // convert event to a string
            event = rxMessage.msg_event;
            rxData = rxMessage.msg_data;

            // convert event to a string
            if ((event >= APP_MSG_IMAGETASK_FIRST) && (event < APP_MSG_IMAGETASK_LAST))
            {
                eventString = imageTaskStateString[event - APP_MSG_IMAGETASK_FIRST];
            }
            else
            {
                eventString = "Unexpected";
            }

            XP_LT_CYAN
            xprintf("\nIMAGE Task");
            XP_WHITE;
            xprintf(" received event '%s' (0x%04x). Value = 0x%08x\r\n", eventString, event, rxData);

            old_state = image_task_state;

            // switch on state - and call individual event handling functions
            switch (image_task_state)
            {
            case APP_IMAGE_CAPTURE_STATE_UNINIT:
                txMessage = flagUnexpectedEvent(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_IDLE:
                txMessage = handleEventForCaptureIdle(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_CAPTURING:
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_UNINIT:
                txMessage = flagUnexpectedEvent(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_IDLE:
                txMessage = handleEventForNNIdle(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_PROCESSING:
                txMessage = handleEventForNNProcessing(rxMessage);
                break;

            default:
                txMessage = flagUnexpectedEvent(rxMessage);
                break;
            }

            if (old_state != image_task_state)
            {
                XP_LT_CYAN;
                xprintf("IMAGE Task state changed ");
                XP_WHITE;
                xprintf("from '%s' (%d) to '%s' (%d)\r\n",
                        imageTaskStateString[old_state], old_state,
                        imageTaskStateString[image_task_state], image_task_state);
            }
        }
    }
}

/**
 * For state machine: Print a red message to see if there are unhandled events we should manage
 */
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage)
{
    APP_MSG_EVENT_E event;
    APP_MSG_DEST_T sendMsg;
    sendMsg.destination = NULL;

    event = rxMessage.msg_event;

    XP_LT_RED;
    if ((event >= APP_MSG_IMAGETASK_FIRST) && (event < APP_MSG_IMAGETASK_LAST))
    {
        xprintf("UNHANDLED event '%s' in '%s'\r\n", imageTaskEventString[event - APP_MSG_IMAGETASK_FIRST], imageTaskStateString[image_task_state]);
    }
    else
    {
        xprintf("UNHANDLED event 0x%04x in '%s'\r\n", event, imageTaskStateString[image_task_state]);
    }
    XP_WHITE;

    // If non-null then our task sends another message to another task
    return sendMsg;
}

/********************************** Public Functions  *************************************/

TaskHandle_t image_createTask(int8_t priority)
{
    // Create the task
    xTaskCreate(vImageTask, "ImageTask", configMINIMAL_STACK_SIZE, NULL, priority, &image_task_id);
    return image_task_id;
}

/**
 * Returns the internal state as a number
 */
uint16_t image_task_getState(void)
{
    return image_task_state;
}

/**
 * Returns the internal state as a string
 */
const char *image_task_getStateString(void)
{
    return *&imageTaskStateString[image_task_state];
}

/**
 * Initialises image processing variables
 */
void image_var_int(void)
{
    g_frame_ready = 0;
    g_cur_jpegenc_frame = 0;
    g_spi_master_initial_status = 0;
}

/**
 * Initialises camera capturing
 */
void app_start_state(APP_STATE_E state)
{
    image_var_int();

    if (state == APP_STATE_ALLON)
    {
        xprintf("APP_STATE_ALLON\n");
        if (cisdp_sensor_init(true) < 0)
        {
            xprintf("\r\nCIS Init fail\r\n");
            APP_BLOCK_FUNC();
        }
    }
    else if (state == APP_STATE_RESTART)
    {
        xprintf("APP_STATE_RESTART\n");
        if (cisdp_sensor_init(false) < 0)
        {
            xprintf("\r\nCIS Init fail\r\n");
            APP_BLOCK_FUNC();
        }
    }
    else if (state == APP_STATE_STOP)
    {
        xprintf("APP_STATE_STOP\n");
        cisdp_sensor_stop();
        return;
    }

    // if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
    //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
    if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0)
    {
        xprintf("\r\nDATAPATH Init fail\r\n");
        APP_BLOCK_FUNC();
    }

    cisdp_sensor_start();
}