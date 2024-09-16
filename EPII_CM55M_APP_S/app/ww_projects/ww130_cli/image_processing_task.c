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

#include "crc16_ccitt.h"

/*************************************** Local Function Declarations *****************************/

static void vImageTask(void *pvParameters);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T rxMessage);

/*************************************** External variables *******************************************/

extern SemaphoreHandle_t xI2CTxSemaphore;

/*************************************** Local variables *******************************************/

// This is the handle of the task
TaskHandle_t image_task_id;
extern QueueHandle_t xImageTaskQueue;
volatile APP_IF_STATE_E if_task_state = APP_IF_STATE_UNINIT;

void handleEventForIdle(APP_MSG_T rxMessage)
{
    APP_MSG_DEST_T txMessage;
    APP_MSG_EVENT_E event;
    uint32_t rxData;

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
        // Start capturing an image
        image_task_state = APP_IMAGE_CAPTURE_STATE_CAPTURING;
        // ADD CAPTURING FUNC HERE

        // Set the state to capturing
        image_task_state = APP_IMAGE_CAPTURE_STATE_IDLE;
        // The message to send depends on the destination! In retrospect it would have been better
        // if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
        if (sendMsg.destination == xIfTaskQueue)
        {
            sendMsg.message.msg_event = APP_MSG_IMAGETASK_CAPTURE_STARTED;
        }
        sendMsg.message.msg_data = 0;
        break;

    case APP_MSG_IMAGETASK_START_NN:
        // Start NN processing
        image_task_state = APP_IMAGE_NN_STATE_PROCESSING;
        // ADD NN FUNC HERE

        // Set the state to processing
        image_task_state = APP_IMAGE_NN_STATE_IDLE;
        if (sendMsg.destination == xIfTaskQueue)
        {
            sendMsg.message.msg_event = APP_MSG_IMAGETASK_NN_STARTED;
        }
        sendMsg.message.msg_data = 0;
        break;

    default:
        // Unexpected event
        txMessage.msg_event = APP_MSG_IMAGETASK_UNEXPECTED_EVENT;
        txMessage.msg_data = 0;
        break;
    }

    return txMessage;
}

void vImageTask(void *pvParameters)
{
    APP_MSG_T rxMessage;
    APP_MSG_DEST_T txMessage;
    QueueHandle_t targetQueue;
    APP_MSG_T send_msg;

    APP_FATFS_STATE_E old_state;
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
                eventString = ifTaskEventString[event - APP_MSG_IMAGETASK_FIRST];
            }
            else
            {
                eventString = "Unexpected";
            }

            XP_LT_CYAN
            xprintf("\nIF Task");
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
                txMessage = handleEventForIdle(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_CAPTURING:
                txMessage = handleEventForStateI2CRx(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_UNINIT:
                txMessage = handleEventForStateI2CTx(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_IDLE:
                txMessage = handleEventForStatePA0(rxMessage);
                break;

            case APP_IMAGE_NN_STATE_PROCESSING:
                txMessage = handleEventForStateDiskOp(rxMessage);
                break;

            default:
                // should not happen
                txMessage = flagUnexpectedEvent(rxMessage);
                break;
            }

            if (old_state != if_task_state)
            {
                // state has changed
                XP_LT_CYAN;
                xprintf("IF Task state changed ");
                XP_WHITE;
                xprintf("from '%s' (%d) to '%s' (%d)\r\n",
                        ifTaskStateString[old_state], old_state,
                        ifTaskStateString[if_task_state], if_task_state);
            }
        }
    }
}