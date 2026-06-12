/*
 * image_task.c
 *
 *  Created on: 12 Aug 2024
 *      Author: CGP
 *
 * FreeRTOS task
 *
 * This task handles capturing images and running the NN processing
 *
 */

/*************************************** Includes *******************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <sys/time.h>
#include <time.h>

#include "WE2_device.h"
#include "WE2_debug.h"
#include "WE2_core.h"
#include "board.h"
#include "printf_x.h"
#include "hx_drv_pwm.h"

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

#ifndef TRUSTZONE_SEC_ONLY
/* FreeRTOS includes. */
#include "secure_port_macros.h"
#endif

#include "image_task.h"
#include "fatfs_task.h"
#include "app_msg.h"
#include "CLI-commands.h"
#include "ff.h"
#include "cisdp_sensor.h"
#include "app_msg.h"
#include "hx_drv_pmu.h"
#include "sleep_mode.h"

#include "driver_interface.h"
#include "cvapp.h"
#include "common_config.h"

#include "exif_utc.h"
#include "fatfs_task.h"
#include "ww500_md.h"

#include "cis_file.h"
#include "c_api_types.h" // Tensorflow errors
#include "hx_drv_scu.h"

#include "hm0360_md.h"
#include "hm0360_regs.h"

#include "ledFlash.h"
#include "pinmux_cfg.h"
#include "barrier.h"

#include "selfTest.h"

#include "ledFlash.h"
#include "pinmux_cfg.h"
#include "barrier.h"

#include "selfTest.h"
#include "exif_gps.h"
#include "exif_builder.h"

/*************************************** Definitions *******************************************/

// Enable/disable writing per-class confidence/label EXIF tags (0xF300+)
// CGP 24/1/26 - Disable this. All processing of NN output tensor including labels is moved to cvapp.cpp
// When debugged, remove this code
#ifdef USE_PERCENTAGE
#define ENABLE_EXIF_CONFIDENCE
#endif // USE_PERCENTAGE

/* Experiment in controlling multiple images captures and LED flash for the HM0360
 * using 2 compiler switches:
 *
 * USE_HM0360_CAPTURE_TIMER
 * How to set the delay between multiple images? e.g. take 6 images as 1s intervals
 *   set: 		The internal timer in the HM0360 is used. After the first image is captured,
 *   IMAGE Task state changes from 'NN Processing' to 'Capturing' and the HM0360 next
 *   VSYNC results in a 'Image Event Frame Ready' event.
 *
 *   not set:	FreeRTOS captureTimer is used. After the first image is captured,
 *   IMAGE Task state changes from 'NN Processing' to 'Wait for Timer' then explicitly starts an HM0360 capture.
 *
 * STROBE_CONTROLS_FLASH
 * How to turn on the Flash LED?
 * 	set:	The HM0360 STROBE pin turns on just before taking an image and the LED comes on.
 * 	The LED is turned off by the HM0360 at the end of VSYNC
 *
 * 	not set:  The LED is turned on by ledFlashEnable() and turned off by the state machine
 * 	when APP_MSG_IMAGETASK_FRAME_READY arrives (as a safeguard also flashOffTimer in ledFlash.c).
 *
 * Results:
 * Option | USE_HM0360_CAPTURE_TIMER | STROBE_CONTROLS_FLASH | Result
 * -------|--------------------------|-----------------------|-----------------------
 *    1   | Enabled                  | Enabled		 		| OK
 *    2   | Enabled                  | Disabled    		   	| LED on too soon - not usable
 *    3   | Disabled                 | Enabled				| OK
 *    4   | Disabled                 | Disabled				| OK - LED on 15ms before VSYNC, 3ms after.
 *
 * Probably best to use option 1? Less chance that LED is stuck on.
 *
 * Known problems: sometimes there is a WDT event as 'Image Event Frame Ready' event is missing.
 * This can be detected: use g_wdt_event to force a retry
 */

#ifdef USE_HM0360
// If uncommented then use the HM0360 internal timer to capture multiple images
// else use captureTimer
// Check both options before cleaning code
#define USE_HM0360_CAPTURE_TIMER

// If defined then use the HM0360 STROBE pin to control the flash
// else use the timer as is the case with RP3 camera
#define STROBE_CONTROLS_FLASH
#endif 	// USE_HM0360

// If uncommented, the image file is save as .bmp instead of .jpeg
#define INVESTIGATE_BMP

// If uncommented brightness will increment after each image of an image sequence
#define INVESTIGATE_FLASH_BRIGHTNESS

// If uncommented the tone mapping registers will change after each image of an image sequence
// Since there are 4 options, choose to take 4 images
#define INVESTIGATE_TONE_MAPPING

// TODO sort out how to allocate priorities
#define image_task_PRIORITY (configMAX_PRIORITIES - 2)

#define IMAGE_TASK_QUEUE_LEN 10

// This is experimental. TODO check it is ok
#define MSGTOMASTERLEN 150

// defaults for PWM output on PB9 for Flash LED brightness
// default 20kHz
#define FLASHLEDFREQ 20000

// enable this to leave the PWM running so we can watch it on the scope.
#define FLASHLEDTEST 1

// Warning: if using 8.3 file names then this applies to directories also

#ifdef USE_HM0360
// Name of file containing extra HM0360 register settings
#define CAMERA_EXTRA_FILE "HM0360EX.BIN"
#elif defined(USE_RP2)
// Name of file containing extra RP2 register settings
#define CAMERA_EXTRA_FILE "RPV2_EX.BIN"
#elif defined(USE_RP3)
// Name of file containing extra RP3 register settings
#define CAMERA_EXTRA_FILE "RPV3_EX.BIN"
#else
// Should not happen. Add something anyway
#define CAMERA_EXTRA_FILE "CAMERA_EX.BIN"
#endif // USE_HM0360

// Define this to add extra EXIF field (AE gain values)
#define EXIF_MAKER_NOTES


/*************************************** Local Function Declarations *****************************/

// This is the FreeRTOS task
static void vImageTask(void *pvParameters);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForInit(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForWaitForTimer(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForNNUpdate(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForSaveState(APP_MSG_T img_recv_msg);

// This is to process an unexpected event
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage);

static bool configure_image_sensor(CAMERA_CONFIG_E operation);

static void setupLEDFlash(void);

// Send unsolicited message to the master
static void sendMsgToMaster(char *str);

// When final activity from the FatFS Task and IF Task are complete, enter DPD
static void sleepWhenPossible(void);

static void captureSequenceComplete(uint32_t accumulatedTime);

static void changeEnableState(bool setEnabled);

static void processNNOutput(int8_t * outCategories, uint8_t classCount);

static void prepareJpegFile(int8_t * outCategories, uint8_t classCount, fileBufferInfo_t * extraBlock);


#ifdef INVESTIGATE_BMP
#define BMP_GRAY8_HEADER_SIZE 1078
static void prepareBmpFile(fileBufferInfo_t * extraBlock);
static uint32_t bmp_create_gray8_header(uint8_t *buf,  uint32_t width, uint32_t height);
#endif // INVESTIGATE_BMP


/*************************************** External variables *******************************************/

extern QueueHandle_t xFatTaskQueue;
extern QueueHandle_t xIfTaskQueue;

extern SemaphoreHandle_t xI2CTxSemaphore;
extern Barrier_t shutdownBarrier;

extern SemaphoreHandle_t xSDInitDoneSemaphore;

extern Barrier_t startupBarrier; // Object that calls a function when all tasks are ready

/*************************************** Local variables *******************************************/

static APP_WAKE_REASON_E woken;

// This is the handle of the task
TaskHandle_t image_task_id;
QueueHandle_t xImageTaskQueue;

volatile APP_IMAGE_TASK_STATE_E image_task_state = APP_IMAGE_TASK_STATE_UNINIT;

// For the current request increments
static uint32_t g_cur_jpegenc_frame;
// For current request captures to take
static uint32_t g_captures_to_take;
// For the accumulative total captures
// TODO perhaps it should be reset at the start of each day? It is only used in filenames
static uint32_t g_frames_total;
static uint32_t g_timer_period; // Interval between pictures in ms
static bool g_wdt_event; 		// A watchdog timer event occurred while waiting for FRAME_READY

static TimerHandle_t captureTimer;

static fileOperation_t fileOp;

// This is a value passed to cisdp_dp_init()
// where the comment is "JPEG Encoding quantization table Selection (4x or 10x)"
// the cisdp_dp_init() code says this can be 4 for JPEG_ENC_QTABLE_4X else JPEG_ENC_QTABLE_10X
// Experimentally, x4 gives bigger files and better quality
uint32_t g_jpg_ratio;

// Semaphore to ensure JPEG buffer is not reused until disk write completes
// TODO - I suspect this actually has no effect...
static SemaphoreHandle_t xJpegBufferSemaphore = NULL;

// Strings for each of these states. Values must match APP_IMAGE_TASK_STATE_E in image_task.h
const char *imageTaskStateString[APP_IMAGE_TASK_STATE_NUMSTATES] = {
    "Uninitialised",
    "Init",
    "Capturing",
    "NN Processing",
    "Wait For Timer",
    "Save State",
    "Updating NN",
};

// Strings for expected messages.
// IMPORTANT! Values must match messages directed to image Task in app_msg.h
const char *imageTaskEventString[APP_MSG_IMAGETASK_LAST - APP_MSG_IMAGETASK_FIRST] = {
    "Image Event Inactivity",
    "Image Event Start Capture",
    "Image Event Stop Capture",
    "Image Event ReCapture",
    "Image Event Frame Ready",
    "Image Event Done",
	"Image Event Disk Write Complete",
	"Image Event Disk Read Complete",
    "Image Event Change Enable",
    "Image Event NN Update Model",
    "Image Event NN Model Updated",
    "Image Event NN Erase Model",
    "Image Event NN Model Erased",
    "Image Event Flash Off",
    "Image Event Error"
};

// There is only one file name for images - this can be declared here - does not need malloc
static char g_imageFileName[IMAGEFILENAMELEN];

// This is the most recently written file name
static char lastImageFileName[IMAGEFILENAMELEN] = "";

// Buffer for messages to be sent via I2C to the BLE processor
static char msgToMaster[MSGTOMASTERLEN];

// Not used
// static bool nnPositive;

// True means we capture images and run NN processing and report results.
// TODO - this is probably redundant. We are probably better to use op_parameter[OP_PARAMETER_CAMERA_ENABLED];
static uint8_t cameraSystemEnabled = 0; // 0 = disabled 1 = enabled

// Measure interval between events
static TickType_t startTime;

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
	// HM0360 AE registers
    HM0360_GAIN_T gain;
#endif

/********************************** Local Functions  *************************************/

#ifdef INVESTIGATE_FLASH_BRIGHTNESS

// percentage increment for each image
// 17 means grab 7 images to test at the full range of brightnesses.
#define FLASH_BRIGHTNESS_INCREMENT 17

/**
 * Experimental feature that sets the LED flash brightness
 * and increments flash brightness for successive images.
 *
 * Initial brightness (after warm boot) is set to 0,
 * but the percentage is only approximate and 0 will cause flash be on.
 *
 * Change brightness after each APP_MSG_IMAGETASK_FRAME_READY event
 */
static void incrementBrightness(void) {
	static uint8_t changingBrightness = 0; // set to 0% at warm boot.

	ledFlashBrightness(changingBrightness);

	// increment the brightness for the next image
	changingBrightness += FLASH_BRIGHTNESS_INCREMENT;
	if (changingBrightness >= 100) {
		changingBrightness = 100;
	}
}
#endif // INVESTIGATE_FLASH_BRIGHTNESS

#if defined(INVESTIGATE_TONE_MAPPING) && defined(USE_HM0360)

/**
 * Experimental feature that increments tone mapping value for each successive image.
 *
 * See data sheet section 4.10
 * There are 4 tone types
 *
 * Call before taking the first image of the sequence then again after each frame ready event
 *
 * Change after each APP_MSG_IMAGETASK_FRAME_READY event
 */
static void incrementToneMapping(void) {
	static TONE_CONFIG_E changingTone = TONE_MAPPING_FLAT; // = 0

	xprintf("DEBUG: Tone is now %d\n", changingTone);
	cisdp_sensor_set_tone(changingTone);

	// increment the tone for next time
	changingTone++;
	if (changingTone >= TONE_MAPPING_NUMBER) {
		// incremented too far - back off
		changingTone = TONE_MAPPING_NUMBER - 1;
	}
}

#endif // INVESTIGATE_TONE_MAPPING

/**
 * This is the local callback that executes when the captureTimer expires
 *
 * It calls this function that was registered in the initialisation process.
 *
 * This sends a APP_MSG_IMAGETASK_RECAPTURE event
 */
static void capture_timer_callback(TimerHandle_t xTimer) {
    APP_MSG_T send_msg;

    // Send back to our own queue
    send_msg.msg_data = 0;
    send_msg.msg_parameter = 0;
    send_msg.msg_event = APP_MSG_IMAGETASK_CAPTURE_TIMER;

    // Timer callbacks run in the context of a task, so the non ISR version can be used
    if (xQueueSend(xImageTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
        xprintf("send_msg=0x%x fail\r\n", send_msg.msg_event);
    }
}

/*
 * Callback from datapath processing
 *
 * This is registered as a callback by cisdp_dp_init() in app_start_state()
 * Events are sent from here to the dp_task queue
 *
 * The common event is SENSORDPLIB_STATUS_XDMA_FRAME_READY which results in a
 * APP_MSG_DPEVENT_XDMA_FRAME_READY message in dp_task queue
 */
void os_app_dplib_cb(SENSORDPLIB_STATUS_E event) {
    APP_MSG_T dp_msg;
    BaseType_t xHigherPriorityTaskWoken;

    /* We have not woken a task at the start of the ISR. */
    xHigherPriorityTaskWoken = pdFALSE;

    dp_msg.msg_event = APP_MSG_DPEVENT_UNKOWN;

    // dbg_printf(DBG_LESS_INFO, "os_app_dplib_cb event = %d\n", event);

    switch (event)   {
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
        dp_msg.msg_event = APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT;
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
        dp_msg.msg_event = APP_MSG_IMAGETASK_FRAME_READY;
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
    dp_msg.msg_parameter = 0;
    xQueueSendFromISR(xImageTaskQueue, &dp_msg, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken)  {
        taskYIELD();
    }
}

/**
 * Implements state machine when in APP_IMAGE_TASK_STATE_INIT
 *
 * This is the state when we are idle and waiting for an instruction.
 *
 * Expected events:
 * 		APP_MSG_IMAGETASK_STARTCAPTURE - from CLI
 * 		APP_MSG_IMAGETASK_INACTIVITY - from inactivity detector
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForInit(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;
    uint16_t requested_captures;
    uint16_t requested_period;
    bool setEnabled;

    APP_MSG_T internal_msg;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event)  {

    case APP_MSG_IMAGETASK_STARTCAPTURE:
        // MD or timer or the CLI task has asked us to start capturing
        requested_captures = (uint16_t)img_recv_msg.msg_data;
        requested_period = img_recv_msg.msg_parameter;

        if (!cameraSystemEnabled) {
        	xprintf("Can't capture - camera system not enabled\n");
        	snprintf(msgToMaster, MSGTOMASTERLEN, "Camera system not enabled");
        	sendMsgToMaster(msgToMaster);
        }
        // Check parameters are acceptable
        else if ((requested_captures < MIN_IMAGE_CAPTURES) || (requested_captures > MAX_IMAGE_CAPTURES) ||
            (requested_period < MIN_IMAGE_INTERVAL) || (requested_period > MAX_IMAGE_INTERVAL))  {
            xprintf("Invalid parameter values %d or %d\n", requested_captures, requested_period);
        }
        else  {
            g_captures_to_take = requested_captures;
            g_timer_period = requested_period;
            XP_LT_GREEN
			xprintf("Images to capture: %d\n", g_captures_to_take);
            xprintf("Interval: %dms\n", g_timer_period);

#ifdef INVESTIGATE_FLASH_BRIGHTNESS
            if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_FLASH_BRIGHTNESS) {
            	xprintf("LED Flash brightness increments after each image.\n");
            	incrementBrightness(); // This sets the brightness
            }
#endif // INVESTIGATE_FLASH_BRIGHTNESS

#if defined(USE_HM0360)
#ifdef INVESTIGATE_TONE_MAPPING
            if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_TONE_MAPPING) {
            	xprintf("Tone mapping values change after each image.\n");
            	incrementToneMapping();
            }
#else
            // This is the default which seems to give good results.
            // TODO when finished testing this could be moved to an HM0360 init() call
            cisdp_sensor_set_tone(TONE_MAPPING_LOW);
#endif // INVESTIGATE_TONE_MAPPING
#endif // defined(USE_HM0360) || defined(USE_HM0360_MD)
            XP_WHITE;

            // Now start the image sensor.
            configure_image_sensor(CAMERA_CONFIG_RUN);
            // Record image capture start time
            startTime = xTaskGetTickCount();

            // The next thing we expect is a frame ready message: APP_MSG_IMAGETASK_FRAME_READY
            image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
        }
        break;

    case APP_MSG_IMAGETASK_CHANGE_ENABLE:
    	// We have received an instruction to enable or disable the NN processing system
        setEnabled = (bool)img_recv_msg.msg_data;
        changeEnableState(setEnabled); // 0 means disabled; 1 means enabled
        if (setEnabled) {
            xprintf("DEBUG: Time to start capturing images!\n");
            // Pass the parameters in the ImageTask message queue
            internal_msg.msg_data = fatfs_getOperationalParameter(OP_PARAMETER_NUM_PICTURES);
            internal_msg.msg_parameter = fatfs_getOperationalParameter(OP_PARAMETER_PICTURE_INTERVAL);
            internal_msg.msg_event = APP_MSG_IMAGETASK_STARTCAPTURE;

            if (xQueueSend(xImageTaskQueue, (void *)&internal_msg, __QueueSendTicksToWait) != pdTRUE)
            {
                xprintf("Failed to send 0x%x to imageTask\r\n", internal_msg.msg_event);
            }
        }
        break;

    case APP_MSG_IMAGETASK_INACTIVITY:
        // Inactivity detected. Prepare to enter DPD, saving state if possible.

        if (fatfs_mounted())  {
            // TODO - can we call this without the if() and expect fatfs_task to handle it?
            // Ask the FatFS task to save state onto the SD card
            send_msg.destination = xFatTaskQueue;
            send_msg.message.msg_event = APP_MSG_FATFSTASK_SAVE_STATE;
            send_msg.message.msg_data = 0;
            image_task_state = APP_IMAGE_TASK_STATE_SAVE_STATE;
        }
        else {
            // No SD card therefore can't save state
            // Wait till the IF Task is also ready, then sleep
            sleepWhenPossible(); // does not return
            image_task_state = APP_IMAGE_TASK_STATE_UNINIT;
        }

        break;

    case APP_MSG_IMAGETASK_NN_UPDATE_MODEL:
    	// App tries to update model
        image_task_state = APP_IMAGE_TASK_STATE_UPDATING_NN;

        uint16_t project_id = (uint16_t)img_recv_msg.msg_data;
        uint16_t deploy_version = img_recv_msg.msg_parameter;
        xprintf("Request to update to %dV%d.TFL\n", project_id, deploy_version);
        // Now we must initiate the update.
        configure_image_sensor(CAMERA_CONFIG_STOP);
        cv_newModel(project_id, deploy_version);
        break;

    case APP_MSG_IMAGETASK_NN_ERASE_MODEL:
    	// App tries to erase model
        image_task_state = APP_IMAGE_TASK_STATE_UPDATING_NN;

        xprintf("Request to erase model\n");
        // Now we must initiate the erase.
        configure_image_sensor(CAMERA_CONFIG_STOP);
        cv_eraseModel();
        break;

    default:
        flagUnexpectedEvent(img_recv_msg);

    } // switch

    return send_msg;
}

/**
 * Implements state machine when in APP_IMAGE_TASK_STATE_CAPTURING
 *
 * This is the state when we are in the process of capturing an image.
 *
 * Expected events:
 * 		APP_MSG_IMAGETASK_FRAME_READY
 * 		Some error events
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;

    static fileBufferInfo_t extraBlock;	// for writing multiple blocks to the same file

    TfLiteStatus ret;
    bool setEnabled;
    bool skip_nn = false;
    // Signed integers
    // Can we use a pointer to the output tensor instead?
    uint8_t classCount;
    int8_t outCategories[MAX_CLASSES];

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event) {

    case APP_MSG_IMAGETASK_FRAME_READY:
        // Here when the image sub-system has captured an image - via os_app_dplib_cb() callback.

        g_cur_jpegenc_frame++; // The number in this sequence
        g_frames_total++;      // The number since the start of time.

#ifdef USE_HM0360_CAPTURE_TIMER
        if (g_cur_jpegenc_frame == g_captures_to_take) {
        	// Turn of the HM0360 ASAP to supress unwanted cycles, esp. of the LED flash
        	hm0360_md_setMode(CONTEXT_A, MODE_SLEEP, 0, 0);
        	//hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0);
        }
#endif // USE_HM0360_CAPTURE_TIMER

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
        // By deferring the clearing of the interrupt till here we can measure the latency of interrupt to image captured.
        // This writes to register 0x2065 - we could put this into the big config file?
        hm0360_md_clearInterrupt(0xff); // clear all bits
#endif

#ifdef INVESTIGATE_FLASH_BRIGHTNESS
        if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_FLASH_BRIGHTNESS) {
        	incrementBrightness(); // This sets the LED flash brightness, then increments it for teh next image
        }
#endif // INVESTIGATE_FLASH_BRIGHTNESS

        ledFlashDisable(); // finished with the LED flash. Turn it off.

        // measure time for the frame capture just completed
        xprintf("Image capture %d/%d took %dms\n\n", g_cur_jpegenc_frame, g_captures_to_take, app_getElapsedMs(startTime));

        // Now measure NN duration
        startTime = xTaskGetTickCount();

        // run NN processing only if model is loaded
        // This gets the input image address and dimensions from:
        // app_get_raw_addr(), app_get_raw_width(), app_get_raw_height()
        if (cv_modelLoaded())  {
        	ret = cv_run(outCategories, &classCount);
        	xprintf("DEBUG: cv_run says there are %d classes\n", classCount);
        }
        else  {
        	xprintf("Skipping NN processing (no model loaded).\n");
        	// TBP - this is hacky but it works for now, could get revised
        	ret = kTfLiteOk; // Treat as successful but no predictions
        	skip_nn = true;
        }

        if (!skip_nn)  {
        	if (ret == kTfLiteOk)  {
        		// This sends 'NN+' or 'NN-'
        		processNNOutput(outCategories, classCount) ;
        		xprintf("NN processing took %dms\n\n", app_getElapsedMs(startTime));
        	}
        	else  {
        		// NN error.
        		// What do we do here?
        		// TODO this is suitable for person detection only - revisit!
        		XP_RED;
        		xprintf("NN error %d. NN processing took %dms\n\n", ret, app_getElapsedMs(startTime));
        		XP_WHITE;
        	}
        } //   if (!skip_nn)

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
        // This is a test to see if/how these change with illumination
        hm0360_md_getGainRegs(&gain);
        // TODO - this is probably the wrong place... just a placeholder to test compilation
        ledFlashNewAEValues(&gain);		// see if these values affect LED flash operation

        snprintf(msgToMaster, MSGTOMASTERLEN, "HM0360 AE regs:\n  Integration time = %d lines\n  Analog gain = %d\n  Digital gain = %d\n  AE Mean = %d\n  AEConverged?: %c",
        		gain.integration,
				gain.analogGain,
				gain.digitalGain,
				gain.aeMean,
				(gain.aeConverged == 1)?'Y':'N');

        XP_LT_GREY;
        // print to console
        xprintf("%s\n", msgToMaster);
        XP_WHITE;

        // and send to BLE
        sendMsgToMaster(msgToMaster);

#if 1
		// This is a test of reading and printing the 32 MD registers

		uint8_t roiOut[ROIOUTENTRIES];
		uint8_t mdBlocks;
		uint16_t offset = 0;

		mdBlocks = hm0360_md_getMDOutput(roiOut, ROIOUTENTRIES);

		offset += snprintf(msgToMaster + offset,
		                   MSGTOMASTERLEN - offset,
		                   "HM0360 motion in %d blocks:\n",
		                   mdBlocks);

		for (uint8_t i = 0; i < ROIOUTENTRIES; i++) {
		    offset += snprintf(msgToMaster + offset,
		                       MSGTOMASTERLEN - offset,
		                       "%02x ",
		                       roiOut[i]);

		    if (offset >= MSGTOMASTERLEN)
		        break;
		}

        XP_LT_GREY;
        // print to console
        xprintf("%s\n", msgToMaster);

        // and send to BLE
        sendMsgToMaster(msgToMaster);

        // Now re-use msgToMaster to print (locally) a 16x16 grid
        // We will do this in two chunks as MSGTOMASTERLEN is too small for all characters
        hm0360_md_printGrid(roiOut, 128, msgToMaster, MSGTOMASTERLEN);
        xprintf("%s", msgToMaster);
        hm0360_md_printGrid(&roiOut[16], 128, msgToMaster, MSGTOMASTERLEN);
        xprintf("%s\n", msgToMaster);

        XP_WHITE;

//		XP_LT_GREY;
//		xprintf("HM0360 motion in %d: \n", mdBlocks);
//        for (uint8_t i=0; i < ROIOUTENTRIES; i++) {
//        	xprintf("%02x ", roiOut[i]);
//        	if ((i % 8) == 7) {
//        		// newline after 8 bytes
//        		xprintf("\n");
//        	}
//        	//
//        }
//		XP_WHITE;
//
#endif // 0
#endif // #if defined(USE_HM0360) || defined(USE_HM0360_MD)

        if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_SKIP_FILE_CREATION) {
        	// Don't save to a file. This allows faster streaming of MD and AE data to the app
        	fileOp.fileName = NULL; // skip file write!
        	fileOp.senderQueue = xImageTaskQueue; // necessary so the response comes to this task.
        	xprintf("Skipping file save.\n");
        }
        else {
        	// Normal processing: create the jpg or bmp file

#ifdef INVESTIGATE_BMP
        	if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_SAVE_BMP) {
        		// alternate between JPG and BMP files, to tell the difference
        		if ((g_cur_jpegenc_frame % 2) == 0) {
#if defined(INVESTIGATE_TONE_MAPPING) && defined(USE_HM0360)
        			if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_TONE_MAPPING) {
        				incrementToneMapping();
        			}
#endif // INVESTIGATE_TONE_MAPPING
        			prepareBmpFile(&extraBlock);
        		}
        		else {
        			prepareJpegFile(outCategories, classCount, &extraBlock);
        		}
        	}
        	else {
#if defined(INVESTIGATE_TONE_MAPPING) && defined(USE_HM0360)
        		if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_TONE_MAPPING) {
        			incrementToneMapping();
        		}
#endif // INVESTIGATE_TONE_MAPPING

        		prepareJpegFile(outCategories, classCount, &extraBlock);
        	}

#else
#if defined(INVESTIGATE_TONE_MAPPING) && (defined(USE_HM0360))
        	if (fatfs_getOperationalParameter(OP_PARAMETER_TEST_MODE_BITS) & TEST_BIT_TONE_MAPPING) {
        		incrementToneMapping();
        	}
#endif // INVESTIGATE_TONE_MAPPING

        	prepareJpegFile(outCategories, classCount, &extraBlock);
#endif // INVESTIGATE_BMP

        }

        // Proceed to write the jpeg file, even if there is no SD card
        // since the fatfs_task will handle that.

    	send_msg.message.msg_data = (uint32_t)&fileOp;
        send_msg.destination = xFatTaskQueue;
        send_msg.message.msg_event = APP_MSG_FATFSTASK_WRITE_IMAGE;

        // extraBlock.length & extraBlock.buffer has been initialised by prepareImageForDisk()
        send_msg.message.msg_parameter = (uint32_t)&extraBlock;

        // Wait in NN_PROCESSING state till the disk write completes - expect APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE
        image_task_state = APP_IMAGE_TASK_STATE_NN_PROCESSING;

        break;

    case APP_MSG_IMAGETASK_CHANGE_ENABLE:
    	// We have received an instruction to enable or disable the NN processing system
    	setEnabled = (bool)img_recv_msg.msg_data;
    	changeEnableState(setEnabled); // 0 means disabled; 1 means enabled
    	break;

    case APP_MSG_IMAGETASK_INACTIVITY:
        // I have seen this, followed soon after by the WDT messages.
        // This should not happen in this state as APP_MSG_IMAGETASK_FRAME_READY should arrive
    	XP_RED;
    	dbg_printf(DBG_LESS_INFO, "Inactive - expect WDT timeout soon?\n");
    	XP_WHITE;
    	break;

    case APP_MSG_DPEVENT_EDM_WDT1_TIMEOUT:   // 0x011d
    case APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT:   // 0x011c
    case APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT:   // 0x011b
    case APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT: // 0x011e
        // Unfortunately I see this sometimes: APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT (0x011c) followed by APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT (0x011b)
        // APP_MSG_IMAGETASK_FRAME_READY does not arrive. timeout WDT_TIMEOUT_PERIOD seems to be 5s
    	XP_RED;
        dbg_printf(DBG_LESS_INFO, ">>>> Received a timeout event 0x%04x after %dms <<<<\n", event, app_getElapsedMs(startTime));
        dbg_printf(DBG_LESS_INFO, ">>>> TODO - re-initialise camera? <<<<\n", event);
        XP_WHITE;

        // Fault detected. Prepare to enter DPD
    	g_wdt_event = true;
        configure_image_sensor(CAMERA_CONFIG_STOP); // run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360

        if (fatfs_mounted())  {
            // TODO - can we call this without the if() and expect fatfs_task to handle it?
            // Ask the FatFS task to save state onto the SD card
            send_msg.destination = xFatTaskQueue;
            send_msg.message.msg_event = APP_MSG_FATFSTASK_SAVE_STATE;
            send_msg.message.msg_data = 0;
            image_task_state = APP_IMAGE_TASK_STATE_SAVE_STATE;
        }
        else  {
        	// No SD card therefore can't save state
        	// Wait till the IF Task is also ready, then sleep
        	sleepWhenPossible(); // does not return
            image_task_state = APP_IMAGE_TASK_STATE_UNINIT;
        }
        break;

    default:
    	flagUnexpectedEvent(img_recv_msg);
    }

    return send_msg;
}

/**
 * Implements state machine when in APP_IMAGE_TASK_STATE_NN_PROCESSING
 *
 * This is the state when we are waiting for a disk write to finish.
 *
 * Expected events:
 * 		APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;
    uint32_t diskStatus;

    bool setEnabled;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;
    diskStatus = img_recv_msg.msg_data;

    switch (event)   {

    case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
        // Increment sequence number BEFORE releasing semaphore
        // This prevents race condition where next capture reads old sequence number
        if (diskStatus == 0)  {
            fatfs_incrementOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER);
        }
        else  {
            dbg_printf(DBG_LESS_INFO, "Image not written. Error: %d\n", diskStatus);
        }

        // Release semaphore - JPEG buffer can now be safely reused
        xSemaphoreGive(xJpegBufferSemaphore);

        // This represents the point at which an image has been captured and processed.

        if (g_cur_jpegenc_frame == g_captures_to_take) {
        	captureSequenceComplete(img_recv_msg.msg_parameter);
        	// Stop the image sensor.
        	// move to earlier: configure_image_sensor(CAMERA_CONFIG_STOP);
        	image_task_state = APP_IMAGE_TASK_STATE_INIT;
        }
        else  {
#ifdef USE_HM0360_CAPTURE_TIMER
        	// The HM0360 uses an internal timer to determine the time for the next image
        	// Re-start the image sensor.
        	configure_image_sensor(CAMERA_CONFIG_CONTINUE);
        	// Expect another frame ready event
        	image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
#else
        	// Start a timer that delays for the defined interval.
        	// When it expires, switch to CAPTURUNG state and request another image
        	if (captureTimer != NULL)  {
        		// Change the period and start the timer
        		// The callback issues a APP_MSG_IMAGETASK_CAPTURE_TIMER event
        		xTimerChangePeriod(captureTimer, pdMS_TO_TICKS(g_timer_period), 0);
        		// Expect a APP_MSG_IMAGETASK_CAPTURE_TIMER event from the capture_timer
        		image_task_state = APP_IMAGE_TASK_STATE_WAIT_FOR_TIMER;
        	}
        	else  {
        		// error
        		flagUnexpectedEvent(img_recv_msg);
        		image_task_state = APP_IMAGE_TASK_STATE_INIT;
        	}
#endif // USE_HM0360_CAPTURE_TIMER
        }
        break;

    case APP_MSG_IMAGETASK_CHANGE_ENABLE:
    	// We have received an instruction to enable or disable the NN processing system
        setEnabled = (bool)img_recv_msg.msg_data;
        changeEnableState(setEnabled); // 0 means disabled; 1 means enabled
        break;

    default:
        flagUnexpectedEvent(img_recv_msg);
        break;
    }

    return send_msg;
}


/**
 * Implements state machine for APP_IMAGE_TASK_STATE_UPDATING_NN
 *
 * This is the state when we are erasing the XIF flash and/or writing a new model to the XIP flash
 *
 * Expected events:
 * 		APP_MSG_IMAGETASK_NN_MODEL_UPDATED
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForNNUpdate(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;
    uint32_t data;

    event = img_recv_msg.msg_event;
    data = img_recv_msg.msg_data;
    send_msg.destination = NULL;

    switch (event) {

    case APP_MSG_IMAGETASK_NN_MODEL_UPDATED:
    	if (data == 0) {
    		// success - notify BLE
    		// TODO we might get 0 even if no model file was found
            snprintf(msgToMaster, MSGTOMASTERLEN, "Updated OK");
    	}
    	else {
    		// fail
            snprintf(msgToMaster, MSGTOMASTERLEN, "Update failed");
    	}
        sendMsgToMaster(msgToMaster);

        image_task_state = APP_IMAGE_TASK_STATE_INIT;
    	break;

    case APP_MSG_IMAGETASK_NN_MODEL_ERASED:
    	snprintf(msgToMaster, MSGTOMASTERLEN, "Erased OK");
        sendMsgToMaster(msgToMaster);

        image_task_state = APP_IMAGE_TASK_STATE_INIT;
    	break;

    default:
        flagUnexpectedEvent(img_recv_msg);
        break;
    }

    return send_msg;
}


/**
 * Implements state machine for APP_IMAGE_TASK_STATE_WAIT_FOR_TIMER
 *
 * This is the state when we are waiting for the capture_timer to expire
 *
 * Expected events:
 * 		APP_MSG_IMAGETASK_CAPTURE_TIMER
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForWaitForTimer(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;

    bool setEnabled;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event)  {

    case APP_MSG_IMAGETASK_CAPTURE_TIMER:
        // here when the captureTimer expires

#ifdef USE_HM0360
    	// We must be using captureTimer for HM0360, so force a HM0360 capture now
    	XP_LT_GREY;
    	hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0);
    	XP_WHITE;
#ifdef STROBE_CONTROLS_FLASH
		// Do nothing as the STROBE has been set up
#else
    	ledFlashActivate();	// Turn on Flash LED (conditionally)
#endif //  STROBE_CONTROLS_FLASH
#else
    	// RP3 only
    	ledFlashActivate();	// Turn on Flash LED (conditionally)
#endif // USE_HM0360

    	sensordplib_retrigger_capture();

    	// Record image capture start time
    	startTime = xTaskGetTickCount();

        image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
        break;

    case APP_MSG_IMAGETASK_INACTIVITY:
    	// Probably the timer interval is greater than the inactivity interval = bad planning
    	// but we better deal with it properly.

    	xTimerStop(captureTimer, portMAX_DELAY);

        // Inactivity detected. Prepare to enter DPD, saving state if possible.
    	// run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360
        configure_image_sensor(CAMERA_CONFIG_STOP);

        if (fatfs_mounted())  {
            // TODO - can we call this without the if() and expect fatfs_task to handle it?
            // Ask the FatFS task to save state onto the SD card
            send_msg.destination = xFatTaskQueue;
            send_msg.message.msg_event = APP_MSG_FATFSTASK_SAVE_STATE;
            send_msg.message.msg_data = 0;
            image_task_state = APP_IMAGE_TASK_STATE_SAVE_STATE;
        }
        else {
            // No SD card therefore can't save state
            // Wait till the IF Task is also ready, then sleep
            sleepWhenPossible(); // does not return
            image_task_state = APP_IMAGE_TASK_STATE_UNINIT;
        }

    	break;

    case APP_MSG_IMAGETASK_CHANGE_ENABLE:
        // We have received an instruction to enable or disable the NN processing system
        setEnabled = (bool)img_recv_msg.msg_data;
        changeEnableState(setEnabled); // 0 means disabled; 1 means enabled
        break;

    default:
        flagUnexpectedEvent(img_recv_msg);
        break;
    }

    return send_msg;
}

/**
 * Implements state machine for APP_IMAGE_TASK_STATE_SAVE_STATE
 *
 * This is the state when we preparing to enter DPD
 *
 * Expected events:
 *		APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForSaveState(APP_MSG_T img_recv_msg)
{
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;

    bool setEnabled;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event)  {

    case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
        // Here when the FatFS task has saved state
        // Wait till the IF Task is also ready, then sleep
        sleepWhenPossible(); // does not return
        image_task_state = APP_IMAGE_TASK_STATE_UNINIT;
        break;

    case APP_MSG_IMAGETASK_CHANGE_ENABLE:
        // We have received an instruction to enable or disable the NN processing system
        setEnabled = (bool)img_recv_msg.msg_data;
        changeEnableState(setEnabled); // 0 means disabled; 1 means enabled
        break;

    case APP_MSG_DPEVENT_EDM_WDT1_TIMEOUT:   // 0x011d
    case APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT:   // 0x011c
    case APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT:   // 0x011b
    case APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT: // 0x011e
    	// If APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT occured earlier then we receive APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT
    	// soon after - i.e. in this state
    	break;

    default:
        flagUnexpectedEvent(img_recv_msg);
        break;
    }

    return send_msg;
}


/**
 * For state machine: Print a red message to see if there are unhandled events we should manage
 *
 * TODO - what should we really do in this case?
 *
 * Parameters: APP_MSG_T img_recv_msg
 * Returns: APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T img_recv_msg)
{
    APP_MSG_EVENT_E event;
    APP_MSG_DEST_T send_msg;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    XP_LT_RED;
    if ((event >= APP_MSG_IMAGETASK_FIRST) && (event < APP_MSG_IMAGETASK_LAST))
    {
        xprintf("IMAGE task unhandled event '%s' in '%s'\r\n", imageTaskEventString[event - APP_MSG_IMAGETASK_FIRST], imageTaskStateString[image_task_state]);
    }
    else
    {
        xprintf("IMAGE task unhandled event 0x%04x in '%s'\r\n", event, imageTaskStateString[image_task_state]);
    }
    XP_WHITE;

    // If non-null then our task sends another message to another task
    return send_msg;
}

/**
 * When the desired number of images have been captured
 *
 * Placed here as a separate routine as it is called from 2 places.
 */
static void captureSequenceComplete(uint32_t accumulatedTime) {
    uint16_t averageTime;

    averageTime = (g_captures_to_take == 0) ? 0 : (accumulatedTime / g_captures_to_take);

    XP_GREEN;
    xprintf("Current captures completed: %d\n", g_captures_to_take);
    xprintf("Average file write time %dms\n", averageTime);

    xprintf("Total frames captured since last reset: %d\n", g_frames_total);
    XP_WHITE;

    // Inform BLE processor
    snprintf(msgToMaster, MSGTOMASTERLEN, "Captured %d images. Last is %s (File write %dms avg.)",
             (int)g_captures_to_take, lastImageFileName, averageTime);

    sendMsgToMaster(msgToMaster);

    // Reset counters
    g_captures_to_take = 0;
    g_cur_jpegenc_frame = 0;
}

/**
 * Enable or disable the camera system.
 *
 * True means we capture images and run NN processing and report results.
 *
 * @param setEnabled enable if true
 */
static void changeEnableState(bool setEnabled) {

    if (setEnabled){
        cameraSystemEnabled = 1;
        fatfs_setOperationalParameter(OP_PARAMETER_CAMERA_ENABLED, 1);
    }
    else {
        cameraSystemEnabled = 0;
        fatfs_setOperationalParameter(OP_PARAMETER_CAMERA_ENABLED, 0);
    }
}

/********************************** FreeRTOS Task  *************************************/

/**
 * Image processing task.
 *
 * This is called when the scheduler starts.
 * Various entities have already be set up by image_createTask()
 *
 * After some one-off activities it waits for events to arrive in its xImageTaskQueue
 *
 */
static void vImageTask(void *pvParameters) {
    APP_MSG_T img_recv_msg;
    APP_MSG_T internal_msg;
    APP_MSG_DEST_T send_msg;
    QueueHandle_t target_queue;
    APP_IMAGE_TASK_STATE_E old_state;
    const char *event_string;
    APP_MSG_EVENT_E event;
    uint32_t recv_data;
    bool cameraInitialised = false;

    g_frames_total = 0;
    g_cur_jpegenc_frame = 0;
    g_captures_to_take = 0;
    g_timer_period = 0;

// JPEG_COMPRESSION is defined in cisdp_cfg.h as 4 or 10
#if (JPEG_COMPRESSION == 10)
    // selects JPEG_ENC_QTABLE_10X
    g_jpg_ratio = 0;
#elif (JPEG_COMPRESSION == 4)
    // selects JPEG_ENC_QTABLE_4X = better quality
    g_jpg_ratio = 4;
#else
    #error "Unsupported JPEG quality setting"
#endif

    g_wdt_event = false;

    int  nnStatus = -1;	// -1 means disabled
    uint16_t interval;

    // Don't proceed until the SD initialisation is done.
    xSemaphoreTake(xSDInitDoneSemaphore, portMAX_DELAY);

    // True means we capture images and run NN processing and report results.
    cameraSystemEnabled = fatfs_getOperationalParameter(OP_PARAMETER_CAMERA_ENABLED);

    XP_CYAN;
    // Observing these messages confirms the initialisation sequence
    xprintf("Starting Image Task\n");
    XP_WHITE;

    // Sanity check: these are defined in cisdp_cfg.h
    // The JPEG buffer seems much larger than necessary
    xprintf("Image %d x %d. Raw buffer %d. JPEG buffer %d. JPEG compression x%d\n",
    		SENCTRL_SENSOR_WIDTH, SENCTRL_SENSOR_HEIGHT,
			RAW_BUFSIZE, JPEG_BUFSIZE,
			JPEG_COMPRESSION );

    if (cameraSystemEnabled) {
    	// The CONFIG.TXT intends the came system to operate,
    	// So let's see if the hardware is present and working
    	// The next lines should initialise the camera but not start taking images
#ifdef USE_HM0360
    	// HM0360 is the main cameras
    	hm0360_md_setIsMainCamera(true);
    	if (woken == APP_WAKE_REASON_COLD) {
    		cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_COLD);
    	}
    	else {
    		cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_WARM);
    	}

#else
    	hm0360_md_setIsMainCamera(false);
    	// For RP camera the SENSOR_ENABLE signal has been turned off during DPD, so must re-initialise all registers
    	cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_COLD);
#endif // USE_HM0360

    	if (!cameraInitialised)  {
    		selfTest_setErrorBits(1 << SELF_TEST_AI_NO_CAM);
    		xprintf("\nDisabling camera functions because there is no camera!\n\n");
    		cameraSystemEnabled = 0;
    	}
    }
    else {
    	xprintf("\nCamera system is disabled.\n\n");
    }

#ifndef USE_HM0360
#ifdef USE_HM0360_MD
    // The HM0360 is not our main camera but we are using it for motion detection.
    // There is some more initialisation required.
    if (woken == APP_WAKE_REASON_COLD) {
    	if (hm0360_md_isHM0360Present()) {
    		hm0360_md_init();	// Initialise for MD only

    		// Turn off the LED flashes, controlled by the HM0360 STROBE output.
    		// If LED flash is required this will be controlled by code when the main camera takes a pic
    		hm0360_md_configureStrobe(false);
    	}
    	else {
    		xprintf("HM0360 missing...\n");
    	}
    }

#endif // USE_HM0360_MD
#endif // USE_HM0360

	// Initialise NN but only of the camera system is enabled
	startTime = xTaskGetTickCount();

	if (cameraSystemEnabled) {
		nnStatus = cv_init(true, true,
				fatfs_getOperationalParameter(OP_PARAMETER_MODEL_PROJECT),
				fatfs_getOperationalParameter(OP_PARAMETER_MODEL_VERSION),
				woken);

		if (nnStatus < 0) {
			xprintf("No model found.\n");
			// TODO - do we do this?
			//selfTest_setErrorBits(1 << SELF_TEST_AI_NN_ERROR);
		}
		else {
			xprintf("Initialised neural network.\n");
		}
	}

    xprintf("NN Initialisation took %dms TODO - consider doing this after taking the picture!\n\n", app_getElapsedMs(startTime));

    // Initial state of the image task (initialized)
    image_task_state = APP_IMAGE_TASK_STATE_INIT;

    // Report now:
    XP_LT_BLUE;
    xprintf("Image sensor and data path initialised for camera ");
#ifdef USE_HM0360
	xprintf("HM0360\n");
#elif defined (USE_RP2)
	xprintf("RP v2\n");
#elif defined (USE_RP3)
	xprintf("RP v3\n");
#else
	xprintf("Unknown\n");
#endif	// USE_HM0360

    xprintf("  Camera system %s.\n", (cameraSystemEnabled > 0) ? "enabled": "disabled");
    xprintf("  Neural network %s.\n", (nnStatus < 0) ? "disabled" : "enabled");
    xprintf("  Flash LED(s) in use: %d\n", fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED));
    xprintf("  Flash brightness: %d%%\n", (uint8_t) fatfs_getOperationalParameter(OP_PARAMETER_LED_BRIGHTNESS_PERCENT));

#ifdef USE_HM0360
    // Flash duration is not used when HM0360 is the main camera
#else
    xprintf("  Flash duration %dms\r\n", fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
#endif // USE_HM0360

    interval = fatfs_getOperationalParameter(OP_PARAMETER_MD_INTERVAL);

    if (interval > 0) {
    	xprintf("  MD sampling: %dms.\n", interval);
    }
    else {
    	xprintf("  MD disabled.\n");
    }
    interval = fatfs_getOperationalParameter(OP_PARAMETER_TIMELAPSE_INTERVAL);
    if (interval > 0) {
    	xprintf("  Timelapse interval: %ds.\n", interval);
    }
    else {
    	xprintf("  Timelapse disabled.\n");
    }

    XP_WHITE;

    // If we woke because of motion detection or timer then let's send ourselves an initial
    // message to take some photos.

    // But only if nnSystemEnabled and cameraInitialised!

    if ((cameraSystemEnabled == 1)  && cameraInitialised && ((woken == APP_WAKE_REASON_MD) || (woken == APP_WAKE_REASON_TIMER))) {
        // Pass the parameters in the ImageTask message queue
        internal_msg.msg_data = fatfs_getOperationalParameter(OP_PARAMETER_NUM_PICTURES);
        internal_msg.msg_parameter = fatfs_getOperationalParameter(OP_PARAMETER_PICTURE_INTERVAL);
        internal_msg.msg_event = APP_MSG_IMAGETASK_STARTCAPTURE;

        if (xQueueSend(xImageTaskQueue, (void *)&internal_msg, __QueueSendTicksToWait) != pdTRUE) {
            xprintf("Failed to send 0x%x to imageTask\r\n", internal_msg.msg_event);
        }
    }

    barrier_ready(&startupBarrier); // Call a function when every task reaches this point

    // Loop forever, taking events from xImageTaskQueue as they arrive
    for (;;)  {
    	// Wait for a message in the queue
    	if (xQueueReceive(xImageTaskQueue, &(img_recv_msg), __QueueRecvTicksToWait) == pdTRUE) {
    		event = img_recv_msg.msg_event;
    		recv_data = img_recv_msg.msg_data;

    		// convert event to a string
    		if ((event >= APP_MSG_IMAGETASK_FIRST) && (event < APP_MSG_IMAGETASK_LAST))  {
    			event_string = imageTaskEventString[event - APP_MSG_IMAGETASK_FIRST];
    		}
    		else   {
    			event_string = "Unrecognised";
    		}

    		XP_LT_CYAN
			xprintf("IMAGE Task ");
    		XP_WHITE;
    		xprintf("received event '%s' (0x%04x). Rx data = 0x%08x\r\n", event_string, event, recv_data);

    		old_state = image_task_state;

    		// Special case (temporary?) to ensure the LED is switched off regardless of the state:
    		if (img_recv_msg.msg_event == APP_MSG_IMAGETASK_FLASH_OFF) {
    			ledFlashDisable();
				send_msg.destination = NULL;
    		}
    		else {
    			switch (image_task_state)   {
    			case APP_IMAGE_TASK_STATE_UNINIT:
    				send_msg = flagUnexpectedEvent(img_recv_msg);
    				break;

    			case APP_IMAGE_TASK_STATE_INIT:
    				send_msg = handleEventForInit(img_recv_msg);
    				break;

    			case APP_IMAGE_TASK_STATE_CAPTURING:
    				send_msg = handleEventForCapturing(img_recv_msg);
    				break;
    				//
					//            case APP_IMAGE_TASK_STATE_BUSY:
						//                send_msg = handleEventForBusy(img_recv_msg);
    				//                break;

    			case APP_IMAGE_TASK_STATE_NN_PROCESSING:
    				send_msg = handleEventForNNProcessing(img_recv_msg);
    				break;

    			case APP_IMAGE_TASK_STATE_WAIT_FOR_TIMER:
    				send_msg = handleEventForWaitForTimer(img_recv_msg);
    				break;

    			case APP_IMAGE_TASK_STATE_SAVE_STATE:
    				send_msg = handleEventForSaveState(img_recv_msg);
    				break;

    			case APP_IMAGE_TASK_STATE_UPDATING_NN:
    				send_msg = handleEventForNNUpdate(img_recv_msg);
    				break;

    			default:
    				send_msg = flagUnexpectedEvent(img_recv_msg);
    				break;
    			} // switch
    		}

    		if (old_state != image_task_state)   {
    			// state has changed
    			XP_LT_CYAN;
    			xprintf("IMAGE Task state changed ");
    			XP_WHITE;
    			xprintf("from '%s' (%d) to '%s' (%d)\r\n",
    					imageTaskStateString[old_state], old_state,
						imageTaskStateString[image_task_state], image_task_state);
    		}

            // Passes message to other tasks if required (commonly fatfs)
            if (send_msg.destination != NULL)   {
                target_queue = send_msg.destination;
                if (xQueueSend(target_queue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE)
                {
                    xprintf("IMAGE task sending event 0x%x failed\r\n", send_msg.message.msg_event);
                }
                else
                {
                    xprintf("IMAGE task sending event 0x%04x. Rx data = 0x%08x\r\n", send_msg.message.msg_event, send_msg.message.msg_data);
                }
            }

            xprintf("\r\n");
        }
    }
}


/**
 * Initialises camera capturing
 *
 * For RP camera expect:
 * 	- at cold boot or warm boot: CAMERA_CONFIG_INIT_COLD (since registers are lost in DPD)
 * 	- at APP_MSG_IMAGETASK_STARTCAPTURE event: CAMERA_CONFIG_RUN
 * 	- at APP_MSG_IMAGETASK_INACTIVITY event: CAMERA_CONFIG_STOP
 *
 * For HM0360 camera expect:
 * 	- at cold boot CAMERA_CONFIG_INIT_COLD (loads initial registers and extra regs from files)
 * 	- at warm boot boot CAMERA_CONFIG_INIT_WARM (since registers are retained in DPD)
 * 	- at APP_MSG_IMAGETASK_STARTCAPTURE event: CAMERA_CONFIG_RUN (selects CONTEXT_A registers)
 *	- at APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE event: CAMERA_CONFIG_CONTINUE (calls cisdp_dp_init() again)
 * 	- at APP_MSG_IMAGETASK_INACTIVITY event: CAMERA_CONFIG_STOP (STOP mode)
 * 	- just before DPD: CAMERA_CONFIG_MD (selects CONTEXT_B registers)
 *
 * @param CAMERA_CONFIG_E operation
 * @return true if initialised. false if no working camera
 */
static bool configure_image_sensor(CAMERA_CONFIG_E operation) {
    bool processedOK = true;

    // Print in grey as there is lots of output for some sensors
    XP_LT_GREY;

    // Check i2c address
    uint8_t cameraID;
    hx_drv_cis_get_slaveID(&cameraID);
    //xprintf("Camera ID 0x%02x\n", cameraID);

    switch (operation)  {

    case CAMERA_CONFIG_INIT_COLD:
        // Called when image task starts - only at cold boot for H0360
        // but also at warm boot for RP cameras

#if defined(USE_RP2) || defined(USE_RP3)
        // Only needed if using a RP camera
        rp_sensor_enable(true);
#endif

        if (!cameraSystemEnabled) {
        	processedOK = false;
        }
        else if (cisdp_sensor_init(true) != 0)    {
        	// HM0360 reported: Memory allocated: 921600 for raw buffer, 76800 for JPEG, 100 for JPEG header
        	// until I changed RAW_BUFSIZE. Then:
        	// Memory allocated: 460800 for raw buffer, 76800 for JPEG, 100 for JPEG header
        	// Then I reduced JPEG buffer to that of the RP3 (probably still too big).
        	// RP3 camera reports: Memory allocated: 460800 for raw buffer, 46256 for JPEG, 100 for JPEG header
        	xprintf("\r\nCIS Init fail\r\n");
        	processedOK = false;
        }
        else  {
#ifdef USE_HM0360
        	cisdp_sensor_set_md_sensitivity(fatfs_getOperationalParameter(OP_PARAMETER_MD_SENSITIVITY));
#endif // USE_HM0360
        	// Initialise extra registers from file
        	cis_file_process(CAMERA_EXTRA_FILE);

        	// if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
        	//  Datapath events give callbacks to os_app_dplib_cb()
        	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_jpg_ratio, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
        		xprintf("\r\nDATAPATH Init fail\r\n");
        		return false;
        	}
            setupLEDFlash();

        }
        break;

    case CAMERA_CONFIG_INIT_WARM:
        // Called at warm boot, only for HM0360

        if (!cameraSystemEnabled) {
        	processedOK = false;
        }
        else if (cisdp_sensor_init(false) != 0)  {
            xprintf("\r\nCIS Init fail\r\n");
            processedOK = false;
        }
        else  {

#ifdef USE_HM0360
        	cisdp_sensor_set_md_sensitivity(fatfs_getOperationalParameter(OP_PARAMETER_MD_SENSITIVITY));

#endif // USE_HM0360
        	// if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
            //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
            if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_jpg_ratio, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0)  {
                xprintf("\r\nDATAPATH Init fail\r\n");
                return false;
            }
            setupLEDFlash();
        }
        break;

    case CAMERA_CONFIG_RUN:
    	if (!cameraSystemEnabled) {
    		processedOK = false;
    	}
    	else if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_jpg_ratio, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
    		xprintf("\r\nDATAPATH Init fail\r\n");
    		processedOK = false;
    	}
    	else  {
#ifdef USE_HM0360
    		XP_LT_GREY;
#ifdef USE_HM0360_CAPTURE_TIMER
    		// Will use HM0360 internal timer to trigger each capture
    		hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, g_timer_period);
#else
    		// Will use captureTimer to trigger each capture
    		hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, 0);
#endif // USE_HM0360_CAPTURE_TIMER
    		XP_WHITE;
#ifdef STROBE_CONTROLS_FLASH
    		// The HM0360 STROBE pin drives drive the LED
    		hm0360_md_configureStrobe(ledFlashIsActive());
#else
    		ledFlashActivate();	// Turn on Flash LED (conditionally)
#endif //  STROBE_CONTROLS_FLASH
#else
    		// turn on the LED for the RP camera
    		ledFlashActivate();	// Turn on Flash LED (conditionally)
#endif // USE_HM0360
    		cisdp_sensor_start(); // Starts data path sensor control block
    	}
    	break;

    case CAMERA_CONFIG_CONTINUE:
    	// We are ONLY here if the HM0360 is the main camera
    	// and the HM0360 internal timer will start the next capture
    	if (!cameraSystemEnabled) {
    		processedOK = false;
    	}
    	else if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_jpg_ratio, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
    		xprintf("\r\nDATAPATH Init fail\r\n");
    		processedOK = false;
    	}
    	else {
    		// Wait for the HM0360 internal time to capture the next image.
    		// Sometimes I get a WDT timeout instead of FRAME_READY - I considered adding this:
    		// hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, g_timer_period);
    		// but that removes the delay (say 1s) between images....

#ifdef STROBE_CONTROLS_FLASH
    		// Do nothing as the STROBE has been set up
#else
    		// We must do manual control, but this is unusable: the LED goes on now
    		// but the image is not captured for another g_timer_period
    		ledFlashActivate();	// Turn on Flash LED (conditionally)
#endif //  STROBE_CONTROLS_FLASH

    		cisdp_sensor_start(); // Starts data path sensor control block
    	}
    	break;

    case CAMERA_CONFIG_STOP:
    	xprintf("Stopping sensor\n");
    	if (!cameraSystemEnabled) {
    		processedOK = false;
    	}
    	else {
    		cisdp_sensor_stop(); // run some sensordplib_stop functions then run IMX708_stream_off commands to the IMX708 (or other sensor)

#if defined(USE_RP2) || defined(USE_RP3)
    		// Only needed if using a RP camera
    		// Explicitly take SENSOR_ENABLE low. (Previously I think this relied on the HX6538 powering off)
    		rp_sensor_enable(false);
#endif
    	}
    	break;

    default:
        // should not happen
        processedOK = false;
        break;
    }

    XP_WHITE;

    return processedOK;
}

/**
 * Common code to prepare the LED flash after cold and warm boots
 */
static void setupLEDFlash(void) {
	uint8_t brightnessPercent;
	rtc_time time;

	brightnessPercent = (uint8_t)fatfs_getOperationalParameter(OP_PARAMETER_LED_BRIGHTNESS_PERCENT);
	ledFlashBrightness(brightnessPercent);

	// Set the LED Flash mode
	ledFlashSetFlashModeFromOpParam(
			fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED),
			fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED_START_TIME),
			fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED_DURATION));

	ledFlashDisable(); // This writes the control bits to the PCA9574

    // AFTER calling ledFlashSetFlashModeFromOpParam(), send the ledFlash code the time
	exif_utc_get_rtc_as_time(&time);
	ledFlashNewTime(time);
}

/**
 * Send an unsolicited message to the MKL62BA.
 *
 */
static void sendMsgToMaster(char *str) {
    APP_MSG_T send_msg;

	// Wait till previous I2C comms transmission is done.
	xSemaphoreTake(xI2CTxSemaphore, portMAX_DELAY);

    // Send back to MKL62BA - msg_data is the string
    send_msg.msg_data = (uint32_t)str;
    send_msg.msg_parameter = strnlen(str, MSGTOMASTERLEN);
    send_msg.msg_event = APP_MSG_IFTASK_MSG_TO_MASTER;

    if (xQueueSend(xIfTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
        xprintf("send_msg=0x%x fail\r\n", send_msg.msg_event);
    }
}

/**
 * When final activity from the FatFS Task and IF Task are complete, enter DPD
 *
 * A message has also been sent to the If Tasks asking it to send its final message.
 * When complete it will give its semaphore.
 */
static void sleepWhenPossible(void) {
	xprintf("Image task ready to sleep.\n");
	barrier_ready(&shutdownBarrier);
}

/**
 * Prepares to write a JPEG file to the disk.
 *
 * This code does not actually perform the write: it sets up buffers and the fileOp object
 * which is passed to the fatfs_task later.
 *
 * New image write scheme: prepare two buffers:
 *  - EXIF data (plus the JPEG file's initial SOE 0xFFd8) is provided in fileOp.buffer and fileOp.length
 *  - actual JPEG data (excluding the initial 0xFFd8) is provided in extraBuffer.buffer and extraBuffer.length
 * Then the fatfs ? function writes the 2 buffers one after the other.
 *
 * @param outCategories - array of NN output values
 * @param classCount - number of entries in outCategories
 * @param extraBlock - structure for the second buffer
 */
static void prepareJpegFile(int8_t * outCategories, uint8_t classCount, fileBufferInfo_t * extraBlock) {

	uint32_t exifLength = 0;
	uint32_t jpegBuffer;
	uint32_t jpegLength;

	// Take semaphore FIRST before modifying jpeg_exif_buf
	// This prevents jpeg_exif_buf from being overwritten before previous write completes
	xSemaphoreTake(xJpegBufferSemaphore, portMAX_DELAY);

	cisdp_get_jpginfo(&jpegLength, &jpegBuffer);

	// Gets JPEG buffer from hardware encoder
	// Clearing cache between each capture
	SCB_InvalidateDCache_by_Addr((void *)jpegBuffer, jpegLength);

	// ignore the first 2 bytes (JPEG SOE 0xffd8) as these will be added by build_exif_segment()
	extraBlock->buffer = ((uint8_t *)jpegBuffer) + 2;
	extraBlock->length = jpegLength - 2;

	/* Assemble ExifInput_t from local state, then call exif_build_segment(). */
	ExifInput_t exif_input;
	static uint8_t nnData[MAX_CLASSES + 2];
	char deployment_id[UUIDLENGTH];

	exif_input.width  = (uint16_t)app_get_raw_width();
	exif_input.height = (uint16_t)app_get_raw_height();
	exif_utc_get_rtc_as_exif_string(exif_input.timestamp, sizeof(exif_input.timestamp));

	/* NN data: [total_bytes][count][score...] */
	if (classCount > MAX_CLASSES) {
		nnData[0] = 1;
		nnData[1] = 0;
	} else {
		nnData[0] = classCount + 1u;
		nnData[1] = classCount;
		for (uint8_t i = 0; i < classCount; i++) {
			nnData[i + 2] = (uint8_t)outCategories[i];
		}
	}
	exif_input.nn_data = nnData;

	/* UserComment — NN label summary */
#ifdef ENABLE_EXIF_CONFIDENCE
	static ClassConfidenceData confidence_data;
	static char user_comment[EXIF_COMMENT_LENGTH];
	bool has_confidence_data;
	memset(&confidence_data, 0, sizeof(confidence_data));
	memset(user_comment, 0, sizeof(user_comment));
	cv_get_confidence_data(&confidence_data);
	has_confidence_data = ((confidence_data.class_count > 0) &&
	                       (confidence_data.class_count <= MAX_CLASSES));
	if (has_confidence_data) {
		char *uc_ptr   = user_comment;
		int   uc_rem   = sizeof(user_comment) - 1;
		for (uint8_t i = 0; i < confidence_data.class_count && i < EXIF_MAX_DYNAMIC_CLASSES; i++) {
			const char *label = confidence_data.labels[i] ? confidence_data.labels[i] : "Unknown";
			int written = snprintf(uc_ptr, uc_rem, "%s: %d%%; ", label,
			                       confidence_data.confidence_percent[i]);
			if (written > 0 && written < uc_rem) {
				uc_ptr += written;
				uc_rem -= written;
			} else {
				break;
			}
		}
	}
	exif_input.user_comment = (has_confidence_data && user_comment[0] != '\0') ? user_comment : NULL;
#else
	char user_comment[EXIF_COMMENT_LENGTH];
	user_comment[0] = '\0';
	if (cv_modelLoaded()) {
		size_t uc_offset = 0;
		for (uint8_t i = 0; i < classCount; i++) {
			int written = snprintf(user_comment + uc_offset,
			                       EXIF_COMMENT_LENGTH - uc_offset,
			                       "%s: %d; ", cv_getLabel(i), outCategories[i]);
			if (written < 0) {
				break;
			}
			if ((size_t)written >= EXIF_COMMENT_LENGTH - uc_offset) {
				uc_offset = EXIF_COMMENT_LENGTH - 1u;
				break;
			}
			uc_offset += (size_t)written;
		}
	}
	exif_input.user_comment = (user_comment[0] != '\0') ? user_comment : NULL;
#endif /* ENABLE_EXIF_CONFIDENCE */

	/* MakerNote — AE register CSV (formatted here; exif_builder.c has no HM0360 types) */
#ifdef EXIF_MAKER_NOTES
#define MAKERDATALEN 32
	char maker_note[MAKERDATALEN];
	snprintf(maker_note, sizeof(maker_note), "%d, %d, %d, %d, %c",
	         gain.integration, gain.analogGain, gain.digitalGain, gain.aeMean,
	         (gain.aeConverged == 1) ? 'Y' : 'N');
	exif_input.maker_note = maker_note;
#else
	exif_input.maker_note = NULL;
#endif /* EXIF_MAKER_NOTES */

	/* Deployment ID */
	fatfs_getDeploymentId(deployment_id, sizeof(deployment_id));
	exif_input.deployment_id =
		(strcmp(deployment_id, DEPLOYMENT_ID_ZERO_UUID) != 0) ? deployment_id : NULL;

	/* Sector-alignment COM padding is applied inside exif_build_segment(). */
	exifLength = exif_build_segment(&exif_input);

	fileOp.buffer = (uint8_t *)exif_get_buffer();
	fileOp.length = exifLength;

	if (exifLength > 0)  {
		//SCB_CleanDCache_by_Addr((void *)exif_buffer, exif_len);
	}
	else {
		xprintf("EXIF insertion failed, using original JPEG buffer\n");
		// TODO handle this!!
	}
#if 0
	// Check by printing some of the buffer that includes the EXIF
	uint16_t bytesToPrint = exifLength + 8;

	XP_LT_GREY;
	xprintf("JPEG & EXIF buffer (%d bytes) begins:\n", exifLength + jpegLength);
	printf_x_printBuffer((uint8_t *)exif_get_buffer(), bytesToPrint);

	XP_WHITE;
#endif

	dir_mgr_generateImageFilename(g_imageFileName, IMAGEFILENAMELEN, "JPG");

	fileOp.fileName = g_imageFileName;	// a global
	fileOp.senderQueue = xImageTaskQueue;
	fileOp.closeWhenDone = true;

	// The JPEG buffer seems much much larger than necessary...
	dbg_printf(DBG_LESS_INFO, "Writing %d bytes (%d + %d) to '%s' from jpeg buffer of %d bytes\n",
			(jpegLength + exifLength), jpegLength, exifLength, fileOp.fileName, JPEG_BUFSIZE);

	// Save the file name as the most recent image
	snprintf(lastImageFileName, IMAGEFILENAMELEN, "%s", fileOp.fileName);
}

#ifdef INVESTIGATE_BMP

/**
 * Prepares to write a BMP file to the disk.
 *
 * This code does not actually perform the write: it sets up buffers and the fileOp object
 * which is passed to the fatfs_task later.
 *
 * New image write scheme: prepare two buffers:
 *  - BMP header is provided in fileOp.buffer and fileOp.length
 *  - actual bitmap is provided in extraBuffer.buffer and extraBuffer.length
 * Then the fatfs ? function writes the 2 buffers one after the other.
 *
 * @param extraBlock - structure for the second buffer
 */
static void prepareBmpFile(fileBufferInfo_t * extraBlock) {
	uint32_t bitMapLength = 0;
	uint32_t jpegBuffer;
	uint32_t headerLength;

	// Take semaphore FIRST before modifying jpeg_exif_buf
	// This prevents jpeg_exif_buf from being overwritten before previous write completes
	xSemaphoreTake(xJpegBufferSemaphore, portMAX_DELAY);

	// Only to find the address of the JPEG buffer which we will reuse for the BMP header
	cisdp_get_jpginfo(&headerLength, &jpegBuffer);

	// Gets JPEG buffer from hardware encoder
	// Clearing cache between each capture - almost certainly not required?
	//SCB_InvalidateDCache_by_Addr((void *)jpegBuffer, jpegLength);

	// prepare a buffer here with the BMP data.
	// We will re-use the JPEG buffer to be the bmp header buffer
	uint8_t *bmp_header = (uint8_t *)jpegBuffer;

	// headerLength is set to BMP_GRAY8_HEADER_SIZE = 1078
	headerLength = bmp_create_gray8_header(bmp_header, app_get_raw_width(), app_get_raw_height());
	// This is the first buffer to write: the .bmp header
	fileOp.buffer = bmp_header;
	fileOp.length = headerLength;

	// This is the second buffer to write - the raw data
	uint8_t *imageBuffer = (uint8_t *)app_get_raw_addr();
	extraBlock->buffer = imageBuffer;
	bitMapLength = app_get_raw_height() * app_get_raw_width();
	extraBlock->length = bitMapLength;
#if 1
	// Check by printing some of the buffer that includes the BMP header
	uint16_t bytesToPrint = 100;

	XP_LT_GREY;
	xprintf("BMP header buffer (%d bytes) begins:\n", headerLength);
	printf_x_printBuffer((uint8_t *) jpegBuffer, bytesToPrint);

	XP_WHITE;
#endif

	dir_mgr_generateImageFilename(g_imageFileName, IMAGEFILENAMELEN, "BMP");

	fileOp.fileName = g_imageFileName;	// a global
	fileOp.senderQueue = xImageTaskQueue;
	fileOp.closeWhenDone = true;

	dbg_printf(DBG_LESS_INFO, "Writing %d bytes (%d + %d) to '%s'\n",
			(headerLength + bitMapLength), headerLength, bitMapLength, fileOp.fileName);

	// Save the file name as the most recent image
	snprintf(lastImageFileName, IMAGEFILENAMELEN, "%s", fileOp.fileName);
}


/* write helpers used by bmp_create_gray8_header (duplicated from exif_builder.c;
 * acceptable since BMP support is experimental and kept under #ifdef INVESTIGATE_BMP) */
static void write16_le(uint8_t *ptr, uint16_t val) {
    ptr[0] = val & 0xFF;
    ptr[1] = val >> 8;
}
static void write32_le(uint8_t *ptr, uint32_t val) {
    ptr[0] =  val        & 0xFF;
    ptr[1] = (val >>  8) & 0xFF;
    ptr[2] = (val >> 16) & 0xFF;
    ptr[3] = (val >> 24) & 0xFF;
}

/**
 * create a bmp header
 *
 * @return BMP_GRAY8_HEADER_SIZE
 */
uint32_t bmp_create_gray8_header(uint8_t *buf,  uint32_t width, uint32_t height) {
    uint32_t rowSize;
    uint32_t imageSize;
    uint32_t fileSize;
    uint8_t *p;
    uint32_t i;

    /* rows must be 4-byte aligned */
    rowSize = (width + 3) & ~3;

    imageSize = rowSize * height;
    fileSize  = BMP_GRAY8_HEADER_SIZE + imageSize;

    /* --- BITMAPFILEHEADER --- */

    write16_le(buf + 0, 0x4D42);                 /* "BM" */
    write32_le(buf + 2, fileSize);
    write16_le(buf + 6, 0);
    write16_le(buf + 8, 0);
    write32_le(buf +10, BMP_GRAY8_HEADER_SIZE);  /* pixel data offset */

    /* --- BITMAPINFOHEADER --- */

    write32_le(buf +14, 40);                     /* header size */
    write32_le(buf +18, width);
    write32_le(buf +22, (uint32_t)(-(int32_t)height));  /* top-down bitmap */
    write16_le(buf +26, 1);                      /* planes */
    write16_le(buf +28, 8);                      /* 8-bit pixels */
    write32_le(buf +30, 0);                      /* BI_RGB (no compression) */
    write32_le(buf +34, imageSize);
    write32_le(buf +38, 2835);                   /* 72 DPI */
    write32_le(buf +42, 2835);
    write32_le(buf +46, 256);                    /* palette entries */
    write32_le(buf +50, 256);

    /* --- grayscale palette (256 × 4 bytes) --- */

    p = buf + 54;

    for (i = 0; i < 256; i++)  {
        p[i*4 + 0] = (uint8_t)i;   /* blue  */
        p[i*4 + 1] = (uint8_t)i;   /* green */
        p[i*4 + 2] = (uint8_t)i;   /* red   */
        p[i*4 + 3] = 0;
    }

    return BMP_GRAY8_HEADER_SIZE;
}
#endif // INVESTIGATE_BMP

/**
 * Do something with the NN output
 *
 * TODO should this be in cvapp.cpp?
 * TODO this is suitable for person detection only - revisit!
 *
 * @param outCategories - array of logit values
 * @param classCount - number of classes
 */
static void processNNOutput(int8_t * outCategories, uint8_t classCount) {
	uint8_t threshold;

	threshold = fatfs_getOperationalParameter(OP_PARAMETER_MODEL_THRESHOLD);

	fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_NN_ANALYSES);

	for (uint8_t i=0; i < classCount; i++) {
		xprintf("Class %d '%s' = logit %d\n", i, cv_getLabel(i), outCategories[i]);
	}

	// TODO This only works for the person detection
	if (outCategories[1] > threshold)  {
		XP_LT_GREEN;
		xprintf("TARGET OBJECT DETECTED!\n");

		fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_POSITIVE_NN_ANALYSES);

		// Send a message to the BLE processor so it can inform the user on the app immediately.
		// Also can be used to flash an LED.
		snprintf(msgToMaster, MSGTOMASTERLEN, "NN+");
		sendMsgToMaster(msgToMaster);
	}
	else  {
		XP_LT_RED;
		xprintf("Target object not detected.\n");
		XP_WHITE;
		// Send a message to the BLE processor so it can inform the user on the app immediately.
		// Also can be used to flash an LED.
		snprintf(msgToMaster, MSGTOMASTERLEN, "NN-");
		sendMsgToMaster(msgToMaster);
	}

	XP_WHITE;
	xprintf("Score %d/128 (Threshold %d)\n", outCategories[1], threshold);
}

/********************************** Public Functions  *************************************/

/**
 * Creates the image task.
 *
 * The task itself initialises the Image sensor and then manages requests to access it.
 */
TaskHandle_t image_createTask(int8_t priority, APP_WAKE_REASON_E wakeReason) {

    if (priority < 0)  {
        priority = 0;
    }

    // Save this. Determines sensor initialisation and whether we take photos
    woken = wakeReason;

    image_task_state = APP_IMAGE_TASK_STATE_UNINIT;

    xImageTaskQueue = xQueueCreate(IMAGE_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
    if (xImageTaskQueue == 0)  {
        xprintf("Failed to create xImageTaskQueue\n");
        configASSERT(0); // TODO add debug messages?
    }

    // Create binary semaphore to protect JPEG buffer from being reused before write completes
    // semaphore vs mutex: https://chatgpt.com/share/69706528-d250-8005-973b-6ab43c1b4629
    xJpegBufferSemaphore = xSemaphoreCreateBinary();
    if (xJpegBufferSemaphore == NULL)   {
        xprintf("Failed to create xJpegBufferSemaphore\n");
        configASSERT(0);
    }
    // Initialize semaphore as available (give it once)
    xSemaphoreGive(xJpegBufferSemaphore);

    // Could be redundant if the HM0360 internal timer is used for successive captures
    captureTimer = xTimerCreate("CaptureTimer",
                                pdMS_TO_TICKS(1000), // initial dummy period
                                pdFALSE,             // one-shot timer
                                NULL,                // timer ID (optional)
                                capture_timer_callback);

    if (captureTimer == NULL) {
        xprintf("Failed to create captureTimer\n");
        configASSERT(0); // TODO add debug messages?
    }

    if (xTaskCreate(vImageTask, /*(const char *)*/ "IMAGE",
                    3 * configMINIMAL_STACK_SIZE,
                    NULL, priority,
                    &image_task_id) != pdPASS) {
        xprintf("Failed to create vImageTask\n");
        configASSERT(0); // TODO add debug messages?
    }

    // return the task handle
    return image_task_id;
}


/**
 * Returns the internal state as a number
 */
uint16_t image_getState(void)
{
    return image_task_state;
}

/**
 * Returns the internal state as a string
 */
const char *image_getStateString(void)
{
    return *&imageTaskStateString[image_task_state];
}

/**
 * Returns the name of the most recently written file as a string
 */
const char *image_getLastImageFile(void)
{
    return lastImageFileName;
}

// Not used...
// bool image_nnDetected(void) {
//	return nnPositive;
//}

/**
 * Returns whether the camera system is enabled.
 *
 * This is the "getter".
 * There is no explicit "setter" - this is done by sending messages to the image task queue.
 *
 * @return state of the nnSystemEnabled variable
 */
bool image_getEnabled(void) {
    if (cameraSystemEnabled == 0) {
        return false;
    }
    else {
        return true;
    }
}

/**
 * Enter DPD
 *
 * This is a separate routine from sleepWhenPossible since it is called from 2 places
 *
 * Callback for when certain tasks have completed their work prior to entering DPD
 *
 * When the barrier mechanism determines all tasks are ready to sleep.
 *
 * We need fatfs_task to save state to SD card
 * We need if_task to notify BLE processor it is sleeping
 *
 */
void image_sleepNow(void) {
    uint32_t timelapseDelay;
    uint16_t mdInterval;

    mdInterval = fatfs_getOperationalParameter(OP_PARAMETER_MD_INTERVAL);

    // Should be off but let's be sure.
    ledFlashDisable();

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
    // HM0360 as main camera
    if (hm0360_md_isHM0360Present()) {
    	XP_LT_GREY;
       	xprintf("Preparing HM0360 for MD:");
    	hm0360_md_prepare(cameraSystemEnabled, mdInterval); // select CONTEXT_B registers (if enabled)

		if ((ledFlashIsActive() && (mdInterval > 0) )) {
			xprintf(" LED flashes.\n");
			hm0360_md_configureStrobe(true);
		}
		else {
			xprintf(" No LED flashes.\n");
			hm0360_md_configureStrobe(false);
		}
    	XP_WHITE;
    }
    else {
    	xprintf("HM0360 missing...\n");
    }
#endif // USE_HM0360

    // TODO - We could retry if we had a timeout?
    if (g_wdt_event) {
    	XP_YELLOW;
    	xprintf(">>> RETRY due to WDT event\n\a");
    	XP_WHITE;
    	timelapseDelay = 1;	// 1 second delay
    }
    else {
    	timelapseDelay = fatfs_getOperationalParameter(OP_PARAMETER_TIMELAPSE_INTERVAL);
    }

	// CLI command might have requested this after firmware update
	if (app_getResetRequest()) {
#if 0
		XP_LT_RED;
		xprintf(">>> Resetting\n\n");
		XP_LT_GREY;	// Grey so the bootloader messages are printed in grey, on exit from DPD

		// does not work
		vTaskDelay(pdMS_TO_TICKS(300));
		// does not return
		NVIC_SystemReset();
#else
		XP_LT_RED;
		xprintf(">>> Reset by watchdog\n\n");
		XP_LT_GREY;	// Grey so the bootloader messages are printed in grey, on exit from DPD

#define WATCH_DOG_TIMEOUT_TH	(100) //ms
		//watch dog start
		WATCHDOG_CFG_T wdg_cfg;
		wdg_cfg.period = WATCH_DOG_TIMEOUT_TH;
		wdg_cfg.ctrl = WATCHDOG_CTRL_CPU;
		wdg_cfg.state = WATCHDOG_STATE_DC;
		wdg_cfg.type = WATCHDOG_RESET; // or WATCHDOG_INT;
		//hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg , WDG_Reset_ISR_CB);
		hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg , NULL);
		xprintf("hx_drv_watchdog_start\n");
#endif
	}

	else if (timelapseDelay > 0) {
        // Enable wakeup on WAKE pin and timer
        sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN | SLEEPMODE_WAKE_SOURCE_RTC,
                             timelapseDelay, false); // Does not return
    }
    else  {
        // If the OP_PARAMETER_TIMELAPSE_INTERVAL setting is 0 then we don't enable a timer wakeup
        sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN, 0, false); // Does not return
    }
}

