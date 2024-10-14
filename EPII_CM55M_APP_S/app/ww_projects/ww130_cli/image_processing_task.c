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
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

#include "image_processing_task.h"
#include "app_msg.h"
#include "ww130_cli.h"
#include "i2c_comm.h"
#include "CLI-commands.h"
// #ifdef IP_xdma
// #include "hx_drv_xdma.h"
#include "sensor_dp_lib.h"
// #endif

#include "WE2_debug.h"

#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"
#include "cisdp_sensor.h"
#include "cisdp_cfg.h"

#include "crc16_ccitt.h"

/*************************************** Definitions *******************************************/

// TODO sort out how to allocate priorities
#define image_task_PRIORITY (configMAX_PRIORITIES - 1)

#define IMAGE_TASK_QUEUE_LEN 10

#define DRV ""

/*************************************** Local Function Declarations *****************************/

static void vImageTask(void *pvParameters);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForCaptureIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T rxMessage);

static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage);

// Strings for each of these states. Values must match APP_IMAGE_CAPTURE_STATE_E in image_processing_task.h
const char *imageTaskStateString[APP_IMAGE_CAPTURE_NUMSTATE] = {
    "Image Capture Uninitialised",
    "Image Capture Initialised",
    "Image Capture Setup Capture Start",
    "Image Capture Setup Capture End",
    "Image Capture Recap Frame",
    "Image Capture Frame Ready",
    "Image Capture Stop Capture Start",
    "Image Capture Stop Capture End",
    "Image Capture Idle"};

// Strings for expected messages. Values must match messages directed to image Task in app_msg.h
const char *imageTaskEventString[APP_MSG_IMAGEEVENT_LAST - APP_MSG_IMAGEEVENT_FIRST] = {
    "Image Event Start Capture",
    "Image Event Stop Capture",
    "Image Event Start NN",
    "Image Event Stop NN",
    "Image Event ReCapture"};

/*************************************** External variables *******************************************/

extern SemaphoreHandle_t xI2CTxSemaphore;

/*************************************** Local variables *******************************************/

// This is the handle of the task
TaskHandle_t image_task_id;
QueueHandle_t xImageTaskQueue;
volatile APP_IMAGE_CAPTURE_STATE_E image_task_state = APP_IMAGE_CAPTURE_STATE_UNINIT;
volatile APP_IMAGE_NN_STATE_E image_task_nn_state = APP_IMAGE_NN_STATE_UNINIT;

// Image processing variables
static uint8_t g_frame_ready;
static uint32_t g_cur_jpegenc_frame;
static uint8_t g_spi_master_initial_status;
static uint8_t g_time;
extern uint32_t g_img_data = 0;
/*volatile*/ uint32_t jpeg_addr, jpeg_sz;

/*************************************** Local Functions *******************************************/

/**
 * Initialises image processing variables
 */
void image_var_int(void)
{
    g_frame_ready = 0;
    g_cur_jpegenc_frame = 0;
    g_spi_master_initial_status = 0;
}

/*
 * Callback from datapath processing
 *
 * This is registered as a callback by cisdp_dp_init() in app_start_state()
 * Events are sent from here to the dp_task queue
 *
 * The common event is SENSORDPLIB_STATUS_XDMA_FRAME_READY which results in a
 * APP_MSG_DPEVENT_XDMA_FRAME_READY message in dp_task queue
 *
 */
void os_app_dplib_cb(SENSORDPLIB_STATUS_E event)
{
    APP_MSG_T dp_msg;
    BaseType_t xHigherPriorityTaskWoken;
    /* We have not woken a task at the start of the ISR. */
    xHigherPriorityTaskWoken = pdFALSE;
    dbg_printf(DBG_LESS_INFO, "os_app_dplib_cb event = %d\n", event);
    switch (event)
    {
    case SENSORDPLIB_STATUS_ERR_FS_HVSIZE:
    case SENSORDPLIB_STATUS_ERR_FE_TOGGLE:
    case SENSORDPLIB_STATUS_ERR_FD_TOGGLE:
    case SENSORDPLIB_STATUS_ERR_FS_TOGGLE:
    case SENSORDPLIB_STATUS_ERR_BLANK_ERR: /*reg_inpparser_stall_error*/
    case SENSORDPLIB_STATUS_ERR_CRC_ERR:   /*reg_inpparser_crc_error*/
    case SENSORDPLIB_STATUS_ERR_FE_ERR:    /*reg_inpparser_fe_cycle_error*/
    case SENSORDPLIB_STATUS_ERR_HSIZE_ERR: /*reg_inpparser_hsize_error*/
    case SENSORDPLIB_STATUS_ERR_FS_ERR:    /*reg_inpparser_fs_cycle_error*/
        hx_drv_inp1bitparser_clear_int();
        dp_msg.msg_event = APP_MSG_DPEVENT_1BITPARSER_ERR;
        break;
    case SENSORDPLIB_STATUS_EDM_WDT1_TIMEOUT:
        dp_msg.msg_event = APP_MSG_DPEVENT_EDM_WDT1_TIMEOUT;
        break;
    case SENSORDPLIB_STATUS_EDM_WDT2_TIMEOUT:
        dp_msg.msg_event = APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT;
        break;
    case SENSORDPLIB_STATUS_EDM_WDT3_TIMEOUT:
        dp_msg.msg_event = APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT;
        break;
    case SENSORDPLIB_STATUS_SENSORCTRL_WDT_OUT:
        dp_msg.msg_event = APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT;
        break;

    case SENSORDPLIB_STATUS_CDM_FIFO_OVERFLOW:
        dp_msg.msg_event = APP_MSG_DPEVENT_CDM_FIFO_OVERFLOW;
        break;
    case SENSORDPLIB_STATUS_CDM_FIFO_UNDERFLOW:
        dp_msg.msg_event = APP_MSG_DPEVENT_CDM_FIFO_UNDERFLOW;
        break;

    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL1:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL1;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL2:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL2;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL3:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL3;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL4:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL4;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL5:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL5;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL6:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL6;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL7:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL7;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL8:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL8;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_ABNORMAL9:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL9;
        break;

    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL1:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL1;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL2:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL2;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL3:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL3;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL4:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL4;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL5:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL5;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL6:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL6;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL7:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL7;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL1:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL1;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL2:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL2;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL3:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL3;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL4:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL4;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL5:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL5;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL6:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL6;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL7:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL7;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL8:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL8;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL9:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL9;
        break;

    case SENSORDPLIB_STATUS_XDMA_RDMA_ABNORMAL1:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL1;
        break;
    case SENSORDPLIB_STATUS_XDMA_RDMA_ABNORMAL2:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL2;
        break;
    case SENSORDPLIB_STATUS_XDMA_RDMA_ABNORMAL3:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL3;
        break;
    case SENSORDPLIB_STATUS_XDMA_RDMA_ABNORMAL4:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL4;
        break;
    case SENSORDPLIB_STATUS_XDMA_RDMA_ABNORMAL5:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL5;
        break;

    case SENSORDPLIB_STATUS_CDM_MOTION_DETECT:
        dp_msg.msg_event = APP_MSG_DPEVENT_CDM_MOTION_DETECT;
        break;
    case SENSORDPLIB_STATUS_XDMA_FRAME_READY:
        dp_msg.msg_event = APP_MSG_DPEVENT_XDMA_FRAME_READY;
        break;
    case SENSORDPLIB_STATUS_XDMA_WDMA1_FINISH:
    case SENSORDPLIB_STATUS_XDMA_WDMA2_FINISH:
    case SENSORDPLIB_STATUS_XDMA_WDMA3_FINISH:
    case SENSORDPLIB_STATUS_XDMA_RDMA_FINISH:
        break;

    case SENSORDPLIB_STATUS_RSDMA_FINISH:
        break;
    case SENSORDPLIB_STATUS_HOGDMA_FINISH:
        break;
    case SENSORDPLIB_STATUS_TIMER_FIRE_APP_NOTREADY:
        dp_msg.msg_event = APP_MSG_DPEVENT_TIMER_FIRE_APP_NOTREADY;
        break;
    case SENSORDPLIB_STATUS_TIMER_FIRE_APP_READY:
        dp_msg.msg_event = APP_MSG_DPEVENT_TIMER_FIRE_APP_READY;
        break;
    default:
        dp_msg.msg_event = APP_MSG_DPEVENT_UNKOWN;
        break;
    }

    dp_msg.msg_data = 0;
    dbg_printf(DBG_LESS_INFO, "Send to dp task 0x%x\r\n", dp_msg.msg_event);
    xQueueSendFromISR(xImageTaskQueue, &dp_msg, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
        taskYIELD();
    }
}

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
        // DP Event Start
    case APP_MSG_DPEVENT_ERR_FS_HVSIZE: /*!< [8] reg_inpparser_fs_hsize_vsize_error_int_en*/
    case APP_MSG_DPEVENT_ERR_FE_TOGGLE: /*!< [7] reg_inpparser_wait_fe_toggle_error_int_en*/
    case APP_MSG_DPEVENT_ERR_FD_TOGGLE: /*!< [6] reg_inpparser_wait_fd_toggle_error_int_en*/
    case APP_MSG_DPEVENT_ERR_FS_TOGGLE: /*!< [5] reg_inpparser_wait_fs_toggle_error_int_en*/
    case APP_MSG_DPEVENT_ERR_BLANK_ERR: /*!< [4] reg_inpparser_blank_toggle_error_int_en*/
    case APP_MSG_DPEVENT_ERR_CRC_ERR:   /*!< [3] reg_inpparser_crc_error_int_en*/
    case APP_MSG_DPEVENT_ERR_FE_ERR:    /*!< [2] reg_inpparser_fe_cycle_error_int_en*/
    case APP_MSG_DPEVENT_ERR_HSIZE_ERR: /*!< [1] reg_inpparser_hsize_error_int_en*/
    case APP_MSG_DPEVENT_ERR_FS_ERR:    /*!< [0] reg_inpparser_fs_cycle_error_int_en*/
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", rxMessage.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_VSYNC:            /*!< EDM [0]:vsync_active error*/
    case APP_MSG_DPEVENT_HSYNC:            /*!< EDM [1]:hsync_active error*/
    case APP_MSG_DPEVENT_OVERLAP_VH:       /*!< EDM [2]:overlap_vh error*/
    case APP_MSG_DPEVENT_OVERLAP_HD:       /*!< EDM [3]:overlap_hd error*/
    case APP_MSG_DPEVENT_OVERLAP_DF:       /*!< EDM [4]:overlap_df error*/
    case APP_MSG_DPEVENT_HSYNC_FP:         /*!< EDM [5]:hsync_front_porch error*/
    case APP_MSG_DPEVENT_HSYNC_BP:         /*!< EDM [6]:hsync_back_porch error*/
    case APP_MSG_DPEVENT_FE:               /*!< EDM [7]:frame_end_timing error*/
    case APP_MSG_DPEVENT_CON_VSYNC:        /*!< EDM [8]:con_vsync_active error*/
    case APP_MSG_DPEVENT_CON_HSYNC:        /*!< EDM [9]:con_hsync_active error*/
    case APP_MSG_DPEVENT_CH0_DE_LESS:      /*!< EDM [10]:ch0_de_less error*/
    case APP_MSG_DPEVENT_CH0_DE_MORE:      /*!< EDM [11]:ch0_de_more error*/
    case APP_MSG_DPEVENT_CH1_DE_LESS:      /*!< EDM [12]:ch1_de_less error*/
    case APP_MSG_DPEVENT_CH1_DE_MORE:      /*!< EDM [13]:ch1_de_more error*/
    case APP_MSG_DPEVENT_CH2_DE_LESS:      /*!< EDM [14]:ch2_de_less error*/
    case APP_MSG_DPEVENT_CH2_DE_MORE:      /*!< EDM [15]:ch2_de_more error*/
    case APP_MSG_DPEVENT_CONV_DE_LESS:     /*!< EDM [16]:con_de_less error*/
    case APP_MSG_DPEVENT_CONV_DE_MORE:     /*!< EDM [17]:con_de_more error*/
    case APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT: /*!< EDM WDT3 Timeout*/
    case APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT: /*!< EDM WDT2 Timeout*/
    case APP_MSG_DPEVENT_EDM_WDT1_TIMEOUT: /*!< EDM WDT1 Timeout*/
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_SENSOR_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT: /*!< Sensor Control Timeout (not used in current code)*/
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_SENSOR_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;
    case APP_MSG_DPEVENT_CDM_FIFO_OVERFLOW:  /*!< CDM Abnormal OVERFLOW*/
    case APP_MSG_DPEVENT_CDM_FIFO_UNDERFLOW: /*!< CDM Abnormal UnderFLOW */
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL1: /*!< XDMA_WDMA1STATUS_ERR_FE_COUNT_NOT_REACH */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL2: /*!< XDMA_WDMA1STATUS_ERR_DIS_BEFORE_FINISH */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL3: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH1_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL4: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH2_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL5: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH3_MISMATCH*/
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL6: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH1_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL7: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH2_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL8: /*!< XDMA_WDMA1STATUS_ERR_FIFO_CH3_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA1_ABNORMAL9: /*!< XDMA_WDMA1STATUS_ERR_BUS */

    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL1: /*!< XDMA_WDMA2STATUS_ERR_FE_COUNT_NOT_REACH */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL2: /*!< XDMA_WDMA2STATUS_ERR_DIS_BEFORE_FINISH */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL3: /*!< XDMA_WDMA2STATUS_ERR_FIFO_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL4: /*!< XDMA_WDMA2STATUS_ERR_FIFO_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL5: /*!< XDMA_WDMA2STATUS_ERR_BUS */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL6: /*!< WDMA2 Abnormal Case6 */
    case APP_MSG_DPEVENT_XDMA_WDMA2_ABNORMAL7: /*!< WDMA2 Abnormal Case7 */

    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL1: /*!< XDMA_WDMA3STATUS_ERR_FE_COUNT_NOT_REACH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL2: /*!< XDMA_WDMA3STATUS_ERR_DIS_BEFORE_FINISH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL3: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH1_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL4: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH2_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL5: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH3_MISMATCH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL6: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH1_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL7: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH2_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL8: /*!< XDMA_WDMA3STATUS_ERR_FIFO_CH3_OVERFLOW */
    case APP_MSG_DPEVENT_XDMA_WDMA3_ABNORMAL9: /*!< XDMA_WDMA3STATUS_ERR_BUS */

    case APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL1: /*!< XDMA_RDMASTATUS_ERR_DIS_BEFORE_FINISH */
    case APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL2: /*!< XDMA_RDMASTATUS_ERR_BUS */
    case APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL3: /*!< RDMA Abnormal Case3 */
    case APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL4: /*!< RDMA Abnormal Case4 */
    case APP_MSG_DPEVENT_XDMA_RDMA_ABNORMAL5: /*!< RDMA Abnormal Case5 */
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_CDM_MOTION_DETECT:
        sendMsg.message.msg_data = 0;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_MOTION_DETECT;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_XDMA_WDMA1_FINISH: /*!< xDMA1 WDMA1 FINISH */
    case APP_MSG_DPEVENT_XDMA_WDMA2_FINISH: /*!< xDMA1 WDMA2 FINISH */
    case APP_MSG_DPEVENT_XDMA_WDMA3_FINISH: /*!< xDMA1 WDMA3 FINISH */
    case APP_MSG_DPEVENT_XDMA_RDMA_FINISH:  /*!< xDMA1 RDMA FINISH */
        break;

    case APP_MSG_DPEVENT_XDMA_FRAME_READY:
        image_task_state = APP_IMAGE_CAPTURE_STATE_CAP_FRAMERDY;
        // app_dump_jpeginfo();
        g_cur_jpegenc_frame++;
        g_frame_ready = 1;
        dbg_printf(DBG_LESS_INFO, "SENSORDPLIB_STATUS_XDMA_FRAME_READY %d \n", g_cur_jpegenc_frame);
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_CAP_FRAME_DONE;
        sendMsg.message.msg_data = 0;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_DPEVENT_TIMER_FIRE_APP_NOTREADY: /*!< Timer Fire but app not ready for frame */
        break;

    case APP_MSG_DPEVENT_TIMER_FIRE_APP_READY: /*!< Timer Fire and app ready for frame */
        break;

    case APP_MSG_DPEVENT_UNKOWN: /*!< DP Unknown */
        // error
        sendMsg.message.msg_data = 0;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_DP_ERROR;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_IMAGEEVENT_STARTCAPTURE:
        // Start Capture Event
        image_task_state = APP_IMAGE_CAPTURE_STATE_SETUP_CAP_START;
        // Should this be RESTART or ALLON?
        // app_start_state(APP_STATE_ALLON);

        // Should data be 0 or msg_event?
        // sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_data = 0;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_START_CAPTURE;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send rxMessage=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_IMAGEEVENT_STOPCAPTURE:
        image_task_state = APP_IMAGE_CAPTURE_STATE_STOP_CAP_START;
        cisdp_sensor_stop();
        // app_start_state(APP_STATE_STOP);
        sendMsg.message.msg_data = rxMessage.msg_event;
        sendMsg.message.msg_event = APP_MSG_MAINEVENT_STOP_CAPTURE;
        if (xQueueSend(xImageTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
        {
            dbg_printf(DBG_LESS_INFO, "send sendMsg=0x%x fail\r\n", sendMsg.message.msg_event);
        }
        break;

    case APP_MSG_IMAGEEVENT_RECAPTURE:
        image_task_state = APP_IMAGE_CAPTURE_STATE_RECAP_FRAME;
        sensordplib_retrigger_capture();
        break;

    default:
        // Unexpected event
        flagUnexpectedEvent(rxMessage);
        break;
    }
    return sendMsg;
}

static void vImageTask(void *pvParameters)
{
    APP_MSG_T rxMessage;
    APP_MSG_DEST_T txMessage;
    QueueHandle_t targetQueue;

    APP_IMAGE_CAPTURE_STATE_E old_state;
    const char *eventString;
    APP_MSG_EVENT_E event;
    uint32_t rxData;

    // Init camera on task creation
    image_task_state = APP_IMAGE_CAPTURE_STATE_INIT;

    for (;;)
    {
        // Wait for a message in the queue
        if (xQueueReceive(xImageTaskQueue, &(rxMessage), __QueueRecvTicksToWait) == pdTRUE)
        {
            // convert event to a string
            event = rxMessage.msg_event;
            rxData = rxMessage.msg_data;

            // convert event to a string
            if ((event >= APP_MSG_IMAGEEVENT_FIRST) && (event < APP_MSG_IMAGEEVENT_LAST))
            {
                eventString = imageTaskStateString[event - APP_MSG_IMAGEEVENT_FIRST];
            }
            else
            {
                eventString = "Unexpected";
            }

            XP_LT_CYAN
            xprintf("\nIMAGE Task");
            XP_WHITE;
            xprintf(" received event '%s' (0x%04x). Value = 0x%08x\r\n", eventString, event, rxData);

            // Hacky solution to capture just a single frame thourgh "snapshot"
            if (g_cur_jpegenc_frame == 1)
            {
                image_task_state = APP_IMAGE_CAPTURE_STATE_STOP_CAP_END;
                event = APP_MSG_IMAGEEVENT_STOPCAPTURE;
                g_cur_jpegenc_frame = 0;
            }
            old_state = image_task_state;

            // switch on state - needs to be reviewed as all events are redirected to the "capturing" event handler
            switch (image_task_state)
            {
            case APP_IMAGE_CAPTURE_STATE_UNINIT:
                txMessage = flagUnexpectedEvent(rxMessage);
                break;

            case APP_MSG_IMAGEEVENT_STARTCAPTURE:
                app_start_state(APP_STATE_ALLON);
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_MSG_IMAGEEVENT_STOPCAPTURE:
                app_start_state(APP_STATE_STOP);
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_RECAP_FRAME:
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_CAP_FRAMERDY:
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_STOP_CAP_START:
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_STOP_CAP_END:
                txMessage = handleEventForCapturing(rxMessage);
                break;

            case APP_IMAGE_CAPTURE_STATE_IDLE:
                txMessage = handleEventForCapturing(rxMessage);
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
    sendMsg.message.msg_data = rxMessage.msg_event;
    sendMsg.message.msg_event = APP_MSG_MAINEVENT_DP_ERROR;
    // image_task_state = APP_IMAGE_CAPTURE_STATE_ERROR;

    XP_LT_RED;
    if ((event >= APP_MSG_IMAGEEVENT_FIRST) && (event < APP_MSG_IMAGEEVENT_LAST))
    {
        xprintf("UNHANDLED event '%s' in '%s'\r\n", imageTaskEventString[event - APP_MSG_IMAGEEVENT_FIRST], imageTaskStateString[image_task_state]);
    }
    else
    {
        xprintf("UNHANDLED event 0x%04x in '%s'\r\n", event, imageTaskStateString[image_task_state]);
    }
    XP_WHITE;

    // If non-null then our task sends another message to another task
    return sendMsg;
}

/**
 * Initialises camera capturing
 */
void app_start_state(APP_STATE_E state)
{
    image_var_int();

    // if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
    //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
    if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0)
    {
        xprintf("\r\nDATAPATH Init fail\r\n");
        APP_BLOCK_FUNC();
    }

    if (state == APP_STATE_ALLON)
    {
        xprintf("APP_STATE_ALLON\n");
        if (cisdp_sensor_init(true) < 0)
        {
            xprintf("\r\nCIS Init fail\r\n");
            APP_BLOCK_FUNC();
        }
        else
        {
            xprintf("CISDP Sensor Start\n");
            cisdp_sensor_start();
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
        else
        {
            xprintf("CISDP Sensor Restarted\n");
            cisdp_sensor_start();
        }
    }
    else if (state == APP_STATE_STOP)
    {
        xprintf("APP_STATE_STOP\n");
        cisdp_sensor_stop();
        return;
    }
}

/********************************** Public Functions  *************************************/

/**
 * Placeholder for code to send EXIF data to the WW130
 */
void image_task_sendExif(void)
{
    dbg_printf(DBG_LESS_INFO, "Not yet implemented\r\n");
}

/**
 * Creates the image task
 * Returns the task priority
 */
TaskHandle_t image_createTask(int8_t priority)
{
    if (priority < 0)
    {
        priority = 0;
    }

    xImageTaskQueue = xQueueCreate(IMAGE_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
    if (xImageTaskQueue == 0)
    {
        xprintf("Failed to create xImageTaskQueue\n");
        configASSERT(0);
        while (1)
            ;
    }
    if (xTaskCreate(vImageTask, "ImageTask",
                    3 * configMINIMAL_STACK_SIZE + CLI_CMD_LINE_BUF_SIZE + CLI_OUTPUT_BUF_SIZE,
                    NULL, priority, &image_task_id) != pdPASS)
    {
        xprintf("Failed to create vFatFsTask\n");
        configASSERT(0); // TODO add debug messages?
    }

    // Create the task
    // xTaskCreate(vImageTask, "ImageTask", configMINIMAL_STACK_SIZE, NULL, priority, &image_task_id);
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