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
 * 	not set:  The LED is turned on by ledFlashEnable() and turned off the the state machine
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

// If uncommented brightness will increment after each image of an image sequence
#define INVESTIGATE_FLASH_BRIGHTNESS
// percentage increment for each image
#define FLASH_BRIGHTNESS_INCREMENT 18

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

#define EXIF_MAX_LEN 512 // I have seen 350 used...
// Limit how many dynamic class entries we will write to EXIF to avoid buffer growth
// TODO - should this not be the same as the maximaum number of classes defined elsewhere? e.g. the max number of labels?
#define EXIF_MAX_DYNAMIC_CLASSES 4
// Limit label length copied into EXIF to keep data small
#define EXIF_MAX_LABEL_LEN 20

// The number of IFD entries in build_exif_segment()
#define IFD0_ENTRY_COUNT 9
// The number of IFD entries in create_gps_ifd()
#define GPS_IFD_ENTRY_COUNT 6
#define GPS_IFD_SIZE (2 + GPS_IFD_ENTRY_COUNT * 12 + 4)

// Buffer for EXIF comment
#define EXIF_COMMENT_LENGTH 256

// Tag IDs enum
typedef enum
{
    TAG_X_RESOLUTION = 0x011A,
    TAG_Y_RESOLUTION = 0x011B,
    TAG_RESOLUTION_UNIT = 0x0128,
    TAG_DATETIME_ORIGINAL = 0x9003,
    TAG_CREATE_DATE = 0x9004,
    TAG_MAKE = 0x010F,
    TAG_MODEL = 0x0110,
    TAG_GPS_IFD_POINTER = 0x8825,
    TAG_GPS_LATITUDE_REF = 0x0001,
    TAG_GPS_LATITUDE = 0x0002,
    TAG_GPS_LONGITUDE_REF = 0x0003,
    TAG_GPS_LONGITUDE = 0x0004,
    TAG_GPS_ALTITUDE_REF = 0x0005,
    TAG_GPS_ALTITUDE = 0x0006,
    TAG_NN_DATA = 0xC000,               // Neural network output array - arbitrary custom tag ID
    TAG_USER_COMMENT = 0x9286,      // Standard EXIF UserComment tag for summary text
    TAG_DEPLOYMENT_ID = 0xF200,		   // Deployment ID (matches ww130_cli convention)
    TAG_WW_CONFIDENCE_BASE = 0xF300	   // Base for confidence tags (0xF300, 0xF301, ...)
} ExifTagID;

// EXIF data types
typedef enum
{
    TYPE_BYTE = 1,
    TYPE_ASCII = 2,
    TYPE_SHORT = 3,
    TYPE_LONG = 4,
    TYPE_RATIONAL = 5,
    UNDEFINED = 7,
    SLONG = 9,
    SRATIONAL = 10
} ExifDataType;

// If using the code that has a duplicate buffer then define SECOND_JPEG_BUFSIZE
// Must by a multiple of 32 bytes.
// #define JPEG_BUFSIZE  76800 //640*480/4
#define SECOND_JPEG_BUFSIZE 20000 // jpeg files seem to be about 9k-20k

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

static void generateImageFileName(uint16_t number);
static void setFileOpFromJpeg(uint32_t jpeg_sz, uint32_t jpeg_addr);

// When final activity from the FatFS Task and IF Task are complete, enter DPD
static void sleepWhenPossible(void);

static void captureSequenceComplete(void);

static void changeEnableState(bool setEnabled);
static void processNNOutput(int8_t * outCategories, uint8_t classCount);

/*************************************** Local EXIF-related Declarations *****************************/

// Insert the EXIF metadata into the jpeg buffer
static uint16_t insertExif(uint32_t jpeg_sz, uint32_t jpeg_addr, int8_t *outCategories, uint8_t categoriesCount);

static void write16_le(uint8_t *ptr, uint16_t val);
static void write32_le(uint8_t *ptr, uint32_t val);
static void write16_be(uint8_t *ptr, uint16_t val);
// static void write32_be(uint8_t *ptr, uint32_t val);
static void addIFD(ExifTagID tagID, uint8_t *entry_ptr, void *tagData);
static uint16_t build_exif_segment(int8_t *outCategories, uint8_t categoriesCount);
static void create_gps_ifd(uint8_t *gps_ifd_start);
static size_t get_gps_ifd_size(void);

/*************************************** External variables *******************************************/

extern QueueHandle_t xFatTaskQueue;
extern QueueHandle_t xIfTaskQueue;

extern Barrier_t shutdownBarrier;

extern SemaphoreHandle_t xSDInitDoneSemaphore;

extern Barrier_t startupBarrier; // Object that calls a function when all tasks are ready

/*************************************** Local variables *******************************************/

#ifdef SECOND_JPEG_BUFSIZE
__attribute__((section(".bss.NoInit"))) uint8_t jpeg_exif_buf[SECOND_JPEG_BUFSIZE] __ALIGNED(32);
#endif // SECOND_JPEG_BUFSIZE

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
uint32_t g_img_data;

static uint16_t g_imageSeqNum; // 0 indicates no SD card

// Semaphore to ensure JPEG buffer is not reused until disk write completes
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
static char imageFileName[IMAGEFILENAMELEN];

// This is the most recently written file name
static char lastImageFileName[IMAGEFILENAMELEN] = "";

// Buffer for messages to be sent via I2C to the BLE processor
static char msgToMaster[MSGTOMASTERLEN];

// Not used
// static bool nnPositive;

// True means we capture images and run NN processing and report results.
// TODO - this is probably redundant. We are probably better to use op_parameter[OP_PARAMETER_CAMERA_ENABLED];
static uint8_t cameraSystemEnabled = 0; // 0 = disabled 1 = enabled

// Support for EXIF
static uint8_t exif_buffer[EXIF_MAX_LEN];

// Global cursor to where non-inline data will be appended
static uint8_t *next_data_ptr;

static uint8_t *tiff_start;

// Measure duration between events
TickType_t startTime;

/********************************** Local Functions  *************************************/

#ifdef INVESTIGATE_FLASH_BRIGHTNESS
static uint8_t changingBrightness = 1; // set to 1% at warm boot.

/**
 * Experimental feature that increments flash brightness for each successive image.
 *
 * Change after each APP_MSG_IMAGETASK_FRAME_READY event
 */
static void incrementBrightness(void) {

	ledFlashBrightness(changingBrightness);

	// increment the brightness for the next image
	changingBrightness += FLASH_BRIGHTNESS_INCREMENT;
	if (changingBrightness >= 100) {
		changingBrightness = 100;
	}
}
#endif // INVESTIGATE_FLASH_BRIGHTNESS


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

/**
 * Fabricate a file name
 *
 * Place the name in imageFileName
 *
 * @param number - this forms part of the file name
 */
static void generateImageFileName(uint16_t number)
{
#if FF_USE_LFN
    // Create a file name
    // file name: 'image_1234_2025-02-03.jpg' = 25 characters, plus trailing '\0'
    rtc_time time;
    exif_utc_get_rtc_as_time(&time);
    snprintf(imageFileName, IMAGEFILENAMELEN, "image_%04d_%d-%02d-%02d.jpg",
             (uint16_t)frame_num, time.tm_year, time.tm_mon, time.tm_mday);
#else
    // Must use 8.3 file name: upper case alphanumeric
    if (woken == APP_WAKE_REASON_MD)
    {
        // Motion has woken us
        snprintf(imageFileName, IMAGEFILENAMELEN, "MD%06d.JPG", (uint16_t)number);
    }
    else
    {
        // Must be a time lapse event
        snprintf(imageFileName, IMAGEFILENAMELEN, "TL%06d.JPG", (uint16_t)number);
    }

#endif // FF_USE_LFN
}

/**
 * Sets the fileOp pointers for the data retrieved from cisdp_get_jpginfo()
 *
 * This includes setting the pointer to the jpeg buffer and setting a file name
 *
 * Parameters: uint32_t - jpeg_sz, jpeg_addr, frame_num
 */
static void setFileOpFromJpeg(uint32_t jpeg_sz, uint32_t jpeg_addr)
{

    fileOp.fileName = imageFileName;
    fileOp.buffer = (uint8_t *)jpeg_addr;
    fileOp.length = jpeg_sz;
    fileOp.senderQueue = xImageTaskQueue;
    fileOp.closeWhenDone = true;
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
            XP_WHITE;

#ifdef INVESTIGATE_FLASH_BRIGHTNESS
            incrementBrightness();
#endif // INVESTIGATE_FLASH_BRIGHTNESS

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
    	// run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360
        configure_image_sensor(CAMERA_CONFIG_STOP);

        if (fatfs_getImageSequenceNumber() > 0)  {
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
    uint32_t jpeg_addr;
    uint32_t jpeg_sz;

    TfLiteStatus ret;
    bool setEnabled;
    uint16_t exif_len;
    bool skip_nn = false;

    // Signed integers
    // Can we use a pointer to the output tensor instead?
    uint8_t classCount;
    int8_t outCategories[MAX_CLASSES];

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;
#if defined(USE_HM0360) || defined(USE_HM0360_MD)
    HM0360_GAIN_T gain;
#endif

    switch (event) {

    case APP_MSG_IMAGETASK_FRAME_READY:
        // Here when the image sub-system has captured an image.

#ifdef INVESTIGATE_FLASH_BRIGHTNESS
        incrementBrightness();
#endif // INVESTIGATE_FLASH_BRIGHTNESS

    	ledFlashDisable(); // finished with the LED flash. Turn it off.

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
        // By deferring the clearing of the interrupt till here we can measure the latency of interrupt to image captured.
        // This writes to register 0x2065 - we could put this into the big config file?
        hm0360_md_clearInterrupt(0xff); // clear all bits
#endif

        // frame ready event received from os_app_dplib_cb
        g_cur_jpegenc_frame++; // The number in this sequence
        g_frames_total++;      // The number since the start of time.

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

        snprintf(msgToMaster, MSGTOMASTERLEN, "Gain regs:\n  Integration time = %d lines\n  Analog gain = %d\n  Digital gain = %d\n  AE Mean = %d\n  AEConverged?: %c",
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
#endif

#if 0
		// This is a test of reading and printing the MD registers

		uint8_t roiOut[ROIOUTENTRIES];
		hm0360_md_getMDOutput(roiOut, ROIOUTENTRIES);

		XP_LT_GREY;
		xprintf("Motion detected???:\n");
        for (uint8_t i=0; i < ROIOUTENTRIES; i++) {
        	xprintf("%02x ", roiOut[i]);
        	if ((i % 8) == 7) {
        		// space after 8 bytes
        		xprintf("\n");
        	}
        }
		XP_WHITE;
#endif // 0

        // Proceed to write the jpeg file, even if there is no SD card
        // since the fatfs_task will handle that.

        // Take semaphore FIRST before modifying jpeg_exif_buf
        // This prevents jpeg_exif_buf from being overwritten before previous write completes
        xSemaphoreTake(xJpegBufferSemaphore, portMAX_DELAY);

        cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr); // Gets JPEG buffer from hardware encoder
        // Clearing cache between each capture
        SCB_InvalidateDCache_by_Addr((void *)jpeg_addr, jpeg_sz);

        // JPEF buffer exists but this will insert EXIF data and increase jpeg_sz
        exif_len = insertExif(jpeg_sz, jpeg_addr, outCategories, classCount);

        // Determine which buffer to use based on whether EXIF insertion succeeded
        uint32_t buffer_to_write;
        uint32_t size_to_write;

        if (exif_len > 0)  {
            // EXIF insertion succeeded - use combined buffer
            buffer_to_write = (uint32_t)jpeg_exif_buf;
            size_to_write = jpeg_sz + exif_len;

            SCB_CleanDCache_by_Addr((void *)jpeg_exif_buf, size_to_write);
        }
        else {
            // EXIF insertion failed - use original JPEG buffer
            xprintf("EXIF insertion failed, using original JPEG buffer\n");
            buffer_to_write = jpeg_addr;
            size_to_write = jpeg_sz;

            // Flush the original JPEG buffer cache
            SCB_CleanDCache_by_Addr((void *)jpeg_addr, size_to_write);
        }

#if 0
    	// Check by printing some of the buffer that includes the EXIF
        //uint16_t bytesToPrint = exif_len + 8;
        uint16_t bytesToPrint = 16;

        XP_LT_GREY;
    	xprintf("JPEG&EXIF buffer (%d bytes) begins:\n", size_to_write);
    	for (int i = 0; i < bytesToPrint; i++) {
    	    xprintf("%02X ", ((uint8_t*)buffer_to_write)[i]);
    	    if (i%16 == 15) {
    	    	xprintf("\n");
    	    }
    	}
    	xprintf("\n");
        XP_WHITE;
#endif

        g_imageSeqNum = fatfs_getImageSequenceNumber();
        generateImageFileName(g_imageSeqNum);

        setFileOpFromJpeg(size_to_write, buffer_to_write);

        dbg_printf(DBG_LESS_INFO, "Writing %d bytes (%d + %d) to '%s' from 0x%08x\n",
                   size_to_write, jpeg_sz, exif_len, fileOp.fileName, buffer_to_write);

        // Save the file name as the most recent image
        snprintf(lastImageFileName, IMAGEFILENAMELEN, "%s", fileOp.fileName);

        send_msg.destination = xFatTaskQueue;
        send_msg.message.msg_event = APP_MSG_FATFSTASK_WRITE_FILE;
        send_msg.message.msg_data = (uint32_t)&fileOp;

        //		// Also see if it is appropriate to send a "Event" message to the BLE processor
        //		// TODO this won't work if the preceding 'NN+' message is being sent!
        //		if ((g_cur_jpegenc_frame == g_captures_to_take) && nnPositive) {
        //
        //			// TODO DREADFUL HACK!!!
        //
        //			// This should be replaced with semaphores that delay until preceding messages have been sent!
        //			// Instead as a quick hack I am adding a delay here in the hope that the preceding "NN+" message
        //			// will get through
        //
        //			vTaskDelay(pdMS_TO_TICKS(100));
        //
        //			// Inform BLE processor
        //			// For the moment the message body is identical to the "Sleep " message but we might change this later
        //			snprintf(msgToMaster, MSGTOMASTERLEN, "Event ");
        //
        //			for (uint8_t i=0; i < OP_PARAMETER_NUM_ENTRIES; i++) {
        //				uint8_t next = strlen(msgToMaster);	// points to the place where we should write the next parameter
        //				snprintf(&msgToMaster[next], sizeof(msgToMaster), "%d ", fatfs_getOperationalParameter(i));
        //			}
        //			sendMsgToMaster(msgToMaster);
        //		}

        // Wait in this state till the disk write completes - expect APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE
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

        if (fatfs_getImageSequenceNumber() > 0)  {
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
        	captureSequenceComplete();
        	// Stop the image sensor.
        	configure_image_sensor(CAMERA_CONFIG_STOP);
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
    	ledFlashEnable(fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
#endif //  STROBE_CONTROLS_FLASH
#else
    	ledFlashEnable(fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
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

        if (fatfs_getImageSequenceNumber() > 0)  {
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
static void captureSequenceComplete(void) {
    // Current captures sequence completed
    XP_GREEN;
    xprintf("Current captures completed: %d\n", g_captures_to_take);
    xprintf("Total frames captured since last reset: %d\n", g_frames_total);
    XP_WHITE;

    // Inform BLE processor
    snprintf(msgToMaster, MSGTOMASTERLEN, "Captured %d images. Last is %s",
             (int)g_captures_to_take, lastImageFileName);
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
    g_img_data = 0;
    g_imageSeqNum = 0; // 0 indicates no SD card
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
    		hm0360_md_configureStrobe(0);
    	}
    	else {
    		xprintf("HM0360 missing...\n");
    	}
    }

#endif // USE_HM0360_MD
#endif // USE_HM0360

#if 0
	// This is a test of reading and printing the MD registers
#if defined(USE_HM0360) || defined(USE_HM0360_MD)

	uint8_t roiOut[ROIOUTENTRIES];

	// This is a test to see if we can read MD regs
	hm0360_md_getMDOutput(roiOut, ROIOUTENTRIES);

	XP_LT_GREY;
	xprintf("Motion detected at init?:\n");
    for (uint8_t i=0; i < ROIOUTENTRIES; i++) {
    	xprintf("%02x ", roiOut[i]);
    	if ((i % 8) == 7) {
    		// space after 8 bytes
    		xprintf("\n");
    	}
    }
	XP_WHITE;

#endif // USE_HM0360_MD
#endif // 0

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

    // A value of 0 means no SD card so we can skip all SD card activities
    g_imageSeqNum = fatfs_getImageSequenceNumber();

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
        	xprintf("\r\nCIS Init fail\r\n");
        	processedOK = false;
        }
        else  {
        	// Initialise extra registers from file
        	cis_file_process(CAMERA_EXTRA_FILE);

        	// if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
        	//  Datapath events give callbacks to os_app_dplib_cb() in dp_task
        	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
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
            // if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
            //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
            if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0)  {
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
    	else if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
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

    		if ((fatfs_getOperationalParameter(OP_PARAMETER_LED_BRIGHTNESS_PERCENT) > 0)
    				&& (fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED) != 0) )  {
    			hm0360_md_configureStrobe(HM0360_SENSOR_STROBE_MODE);
    		}
    		// else no strobe
#else
    		ledFlashEnable(fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
#endif //  STROBE_CONTROLS_FLASH
#else
    		// turn on the LED for the RP camera
    		ledFlashEnable(fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
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
    	else if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
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
    		ledFlashEnable(fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION));
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
	FlashLeds_t ledInUse;

	brightnessPercent = (uint8_t)fatfs_getOperationalParameter(OP_PARAMETER_LED_BRIGHTNESS_PERCENT);
	ledFlashBrightness(brightnessPercent);

	// Select the LED
	ledInUse = fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED);
	ledFlashSelectLED(ledInUse);

	ledFlashDisable(); // This writes the control bits to the PCA9574
}

/**
 * Send an unsolicited message to the MKL62BA.
 *
 */
static void sendMsgToMaster(char *str) {
    APP_MSG_T send_msg;

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



/*************************************** Local EXIF-related Definitions *****************************/

// TODO here are some notes that need to be placed in the right place

/**
 * Insert EXIF metadata into the buffer that contains the rest of the JPEG data
 *
 * The objective is to find the appropriate place in the jpeg file to insert the EXIF,
 * then insert the EXIF there, having shifted the second part of the buffer to make space
 *
 * EXIF data is probably:
 * 	- UTC time, obtained with exif_utc_get_rtc_as_exif_string()
 * 	- GPS location, obtained with exif_gps_generate_byte_array()
 * 	- Neural network output, passed as parameters to this function
 * 	- Other EXIF fields we have decided on
 *
 * NOTE: the jpeg_enc_addr pointer is to an array of 32-bit words.
 * NOTE: It is not clear to me whether the JPEG buffer is big enough to allow insertion of the data.
 * There is a definition in cispd_densor:
 * 		#define JPEG_BUFSIZE  76800 //640*480/4
 * It is not clear from a qucik look at the code whether there is more than one jpeg image using this buffer.
 * In any event, it is important not to overrun that buffer.
 *
 * It would be safer to make a new buffer e.g. with malloc (or the FreeRTOS version) and copy the jpeg
 * data into that. But maybe that is not necessary. Perhaps easiest initially to try to insert the EXIF
 * in place, and only use a different approach if that fails.
 *
 * There is another approach entirely, which is not to insert the EXIT data, but to write the JPEG file
 * in 3 chunks:
 * 		1 The JPEG data up to the insertion point
 * 		2 The EXIF data
 * 		3 The JPEG data after the insertion point
 *
 * A different approach again would be write the JPEG file to SD card without inserting the EXIF,
 * then write the EXIF data to a separate file (same file name but with the extension .exif)
 * then post-process the two files off-line.
 *
 * Regardless of the approach, a number of functions will be required, something like this:
 *
 * unit32_t * exifInsertionPoint((uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr);
 * void createExit(uint32_t exifBuffer, uint16_t exitBufferSize,  uint8_t * outCategories, uint16_t categoriesCount);
 *
 * IMPORTANT to figure out whether we are manipulating 8, 16 or 32 bit arrays.
 * Uncertainty here could be causing the trouble Tobyn has reported.
 *
 * See here for an investigation into exiftool:
 * 	https://chatgpt.com/share/68040174-01fc-8005-a30f-23fc363b98ec
 *
 * @param jpeg_enc_filesize - size of the jpeg buffer. Return the size of the expanded buffer, including the EXIF
 * @param jpeg_enc_addr - pointer to the buffer containing the jpeg data
 * @param outCategories - an array of integers, one for each of the neural network output categories
 * @param categoriesCount - size of that array
 */
// Copy of what is in cisdp_sensor

static uint16_t insertExif(uint32_t jpeg_sz, uint32_t jpeg_addr,
                           int8_t *outCategories, uint8_t categoriesCount)
{
    uint16_t exif_len = 0;
    uint8_t *jpeg_buf;

    jpeg_buf = (uint8_t *)jpeg_addr;

    // Sanity check: must start with FFD8 and then FFE0
    if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8 || jpeg_buf[2] != 0xFF || jpeg_buf[3] != 0xE0)
    {
        // Handle error: unexpected JPEG structure
        return 0;
    }

    // Build EXIF segment - placed in exif_buffer[] and size in exif_len
    exif_len = build_exif_segment(outCategories, categoriesCount);

    // Check for enough space (depends on your memory layout)
    if (jpeg_sz + exif_len > SECOND_JPEG_BUFSIZE)
    {
        // TODO Handle error: buffer too small
        return 0;
    }

#if 0
	// Check by printing EXIF buffer
	xprintf("Added %d bytes of EXIF to %d bytes of jpeg\n", exif_len, jpeg_sz);

	// Print the buffer - useful for debugging
	for (int i = 0; i < exif_len; i++) {
	    xprintf("%02X ", exif_buffer[i]);
	    if (i%16 == 15) {
	    	xprintf("\n");
	    }
	}
	xprintf("\n");
#endif

#ifdef SECOND_JPEG_BUFSIZE
    jpeg_exif_buf[0] = 0xff; // Add the JPEG marker 0xffd8
    jpeg_exif_buf[1] = 0xd8;

    // Now copy in the EXIF
    memcpy(&jpeg_exif_buf[2], exif_buffer, exif_len);

    // Finally copy in the original JPEG data (excluding 0xffd8
    memcpy(&jpeg_exif_buf[exif_len + 2], &jpeg_buf[2], jpeg_sz - 2);

#if 0
	// Check by printing some of jpeg_exif_buf buffer
	xprintf("Modified jpeg buffer:\n");
	for (int i = 0; i < exif_len + 8; i++) {
	    xprintf("%02X ", jpeg_exif_buf[i]);
	    if (i%16 == 15) {
	    	xprintf("\n");
	    }
	}
	xprintf("\n");
#endif

#else
    // earlier code - try this again
    // Shift data (move APP0 and onward forward to make room for EXIF)
    memmove(&jpeg_buf[2] + exif_len, &jpeg_buf[2], jpeg_sz - 2);
    // memmove((uint8_t *) jpeg_addr + 2 + exif_len, (uint8_t *) jpeg_addr + 2, jpeg_sz - 2);

    // Insert EXIF after SOI (at jpeg_buf[2])
    // memcpy((uint8_t *) jpeg_addr + 2, exif_buffer, exif_len);
    memcpy(&jpeg_buf[2], exif_buffer, exif_len);

#endif // SECOND_JPEG_BUFSIZE

    return exif_len;
}

// Helper to write 2- and 4-byte little endian values
static void write16_le(uint8_t *ptr, uint16_t val)
{
    ptr[0] = val & 0xFF;
    ptr[1] = val >> 8;
}

static void write32_le(uint8_t *ptr, uint32_t val)
{
    ptr[0] = val & 0xFF;
    ptr[1] = (val >> 8) & 0xFF;
    ptr[2] = (val >> 16) & 0xFF;
    ptr[3] = (val >> 24) & 0xFF;
}

// Helper to write 2- and 4-byte big endian values
static void write16_be(uint8_t *ptr, uint16_t val)
{
    ptr[1] = val & 0xFF;
    ptr[0] = val >> 8;
}

/*
static void write32_be(uint8_t *ptr, uint32_t val) {
    ptr[3] = val & 0xFF;
    ptr[2] = (val >> 8) & 0xFF;
    ptr[1] = (val >> 16) & 0xFF;
    ptr[0] = (val >> 24) & 0xFF;
}
*/

// Add an IFD entry
static void addIFD(ExifTagID tagID, uint8_t *entry_ptr, void *tagData)
{
    switch (tagID)
    {
    case TAG_X_RESOLUTION:
    case TAG_Y_RESOLUTION:
    {
        uint32_t *rational = (uint32_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        write32_le(next_data_ptr, rational[0]);
        next_data_ptr += 4;
        write32_le(next_data_ptr, rational[1]);
        next_data_ptr += 4;
        break;
    }
    case TAG_RESOLUTION_UNIT:
    {
        uint16_t value = *(uint16_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_SHORT);
        write32_le(entry_ptr + 4, 1);
        write16_le(entry_ptr + 8, value);
        write16_le(entry_ptr + 10, 0);
        break;
    }
    case TAG_DATETIME_ORIGINAL:
    case TAG_CREATE_DATE:
    case TAG_MAKE:
    case TAG_MODEL:
    case TAG_USER_COMMENT:
    case TAG_DEPLOYMENT_ID:
    case TAG_GPS_LATITUDE_REF:
    case TAG_GPS_LONGITUDE_REF:
    {
        char *ascii = (char *)tagData;
        uint32_t length = strlen(ascii) + 1; // include null terminator
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_ASCII);
        write32_le(entry_ptr + 4, length);

        if (length <= 4)
        {
            // Data will fit into the next 4 bytes
            memset(entry_ptr + 8, 0, 4);
            memcpy(entry_ptr + 8, ascii, length);
        }
        else
        {
            // Data needs to go elsewhere and we write the pointer to it here
            write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
            memcpy(next_data_ptr, ascii, length);
            next_data_ptr += length;
        }
        break;
    }
    case TAG_GPS_LATITUDE:
    case TAG_GPS_LONGITUDE:
    {
        uint32_t *dms = (uint32_t *)tagData; // 3 pairs of (numerator, denominator)
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 3);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        for (int i = 0; i < 3; ++i)
        {
            write32_le(next_data_ptr, dms[i * 2]);
            next_data_ptr += 4;
            write32_le(next_data_ptr, dms[i * 2 + 1]);
            next_data_ptr += 4;
        }
        break;
    }
    case TAG_GPS_ALTITUDE:
    {
        uint32_t *rational = (uint32_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_RATIONAL);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
        write32_le(next_data_ptr, rational[0]);
        next_data_ptr += 4;
        write32_le(next_data_ptr, rational[1]);
        next_data_ptr += 4;
        break;
    }
    case TAG_GPS_ALTITUDE_REF:
    {
        uint8_t value = *(uint8_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_BYTE);
        write32_le(entry_ptr + 4, 1);
        memset(entry_ptr + 8, 0, 4);
        entry_ptr[8] = value;
        break;
    }
    case TAG_NN_DATA:
    {
        // For NN output we use a byte array, with the first entry being the number of bytes that follow
        uint8_t *bytes = (uint8_t *)tagData;
        uint32_t length = bytes[0]; // First byte is the length of the following data
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_BYTE);
        write32_le(entry_ptr + 4, length);
        write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));

        if (length <= 4)
        {
            // Data will fit into the next 4 bytes
            memset(entry_ptr + 8, 0, 4); // pre-fill with zeros
            memcpy(entry_ptr + 8, &bytes[1], length);
        }
        else
        {
            // Data needs to go elsewhere and we write the pointer to it here
            write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
            memcpy(next_data_ptr, &bytes[1], length);
            next_data_ptr += length;
        }
        break;
    }
    case TAG_GPS_IFD_POINTER:
    {
        uint32_t offset = *(uint32_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_LONG);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, offset);
        break;
    }
    default:
    {
        // Handle dynamic confidence tags (private tag range 0xF300-0xF3FF)
        // Even tags (0xF300, 0xF302, ...) are SHORT (confidence percentage)
        // Odd tags (0xF301, 0xF303, ...) are ASCII (labels)
        if (tagID >= TAG_WW_CONFIDENCE_BASE && tagID < (TAG_WW_CONFIDENCE_BASE + MAX_CLASSES * 2))
        {
            if ((tagID - TAG_WW_CONFIDENCE_BASE) % 2 == 0)
            {
                // Even offset: confidence value (SHORT)
                uint16_t value = *(uint16_t *)tagData;
                write16_le(entry_ptr, tagID);
                write16_le(entry_ptr + 2, TYPE_SHORT);
                write32_le(entry_ptr + 4, 1);
                write16_le(entry_ptr + 8, value);
                write16_le(entry_ptr + 10, 0);
            }
            else
            {
                // Odd offset: label string (ASCII)
                char *ascii = (char *)tagData;
                uint32_t length = strlen(ascii) + 1; // include null terminator
                write16_le(entry_ptr, tagID);
                write16_le(entry_ptr + 2, TYPE_ASCII);
                write32_le(entry_ptr + 4, length);

                if (length <= 4)
                {
                    // Data will fit into the next 4 bytes
                    memset(entry_ptr + 8, 0, 4);
                    memcpy(entry_ptr + 8, ascii, length);
                }
                else
                {
                    // Data needs to go elsewhere and we write the pointer to it here
                    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
                    memcpy(next_data_ptr, ascii, length);
                    next_data_ptr += length;
                }
            }
        }
        // If tag is not in expected range, do nothing (could add error handling)
        break;
    }
    } // switch
}

/**
 * Get the size of the GPS IFD -
 *
 * If the altitude is present it is 78 bytes
 */
static size_t get_gps_ifd_size(void)
{
    return GPS_IFD_SIZE;
}

// Create a GPS IFD block
/**
 * Create a GPS IFD block.
 *
 * TODO - this code hard-codes some GPS data - use exif_gps.c!
 * TODO - check buffer overflow
 *
 * @param gps_ifd_start - pointer to where the buffer should be
 */
static void create_gps_ifd(uint8_t *gps_ifd_start)
{
    uint8_t *p = gps_ifd_start;

    write16_le(p, GPS_IFD_ENTRY_COUNT);
    p += 2;

    uint8_t *ifd = p;
    p += GPS_IFD_ENTRY_COUNT * 12;
    write32_le(p, 0);
    p += 4; // write terminating 4 x 0 (indictes the end of the IFDs

    char latRef[] = "N";
    char lonRef[] = "E";
    uint32_t lat[6] = {37, 1, 48, 1, 3000, 100};  // 37°48'30.00"
    uint32_t lon[6] = {122, 1, 25, 1, 1500, 100}; // 122°25'15.00"
    uint8_t altRef = 0;                           // 0 = above sea level
    uint32_t alt[2] = {5000, 100};                // 50.00 meters

    // Now write 6 IFDs
    addIFD(TAG_GPS_LATITUDE_REF, ifd + 0 * 12, latRef);
    addIFD(TAG_GPS_LATITUDE, ifd + 1 * 12, lat);
    addIFD(TAG_GPS_LONGITUDE_REF, ifd + 2 * 12, lonRef);
    addIFD(TAG_GPS_LONGITUDE, ifd + 3 * 12, lon);
    addIFD(TAG_GPS_ALTITUDE_REF, ifd + 4 * 12, &altRef);
    addIFD(TAG_GPS_ALTITUDE, ifd + 5 * 12, alt);
}

/**
 * Builds a valid EXIF data structure, including APP1 tag
 *
 * This particular example has hard-coded tags to add, including the X&Y resilutions.
 *
 */
static uint16_t build_exif_segment(int8_t *outCategories, uint8_t categoriesCount) {
    char timestamp[EXIFSTRINGLENGTH] = {0}; // 22:20:36 2025:07:06 = 19 characters plus trailing \0
    uint16_t exif_len;
    uint8_t nnData[MAX_CLASSES + 2];

    // IFD count: base entries (9) + UserComment (1 if has confidence data) + DeploymentID (1 if has deployment ID)
    uint16_t dynamic_ifd_count = IFD0_ENTRY_COUNT;	// 9

    // Insert the NN data (raw scores for backwards compatibility)
    if (categoriesCount > MAX_CLASSES)  {
        // error
        nnData[0] = 1;
        nnData[1] = 0;
    }
    else  {
        // First byte is the number of bytes following
        nnData[0] = categoriesCount + 1;
        // Second byte is the number of categories
        nnData[1] = categoriesCount;
        // Subsequent bytes are the NN outputs
        for (uint8_t i = 0; i < categoriesCount; i++)  {
            nnData[i + 2] = outCategories[i];
        }
    }

#ifdef ENABLE_EXIF_CONFIDENCE

    // Get confidence data from CV module (optional)
    // Use static variables to avoid consuming ~164 bytes of stack space.
    // This prevents stack overflow during memory-intensive operations like manifest unzip.
    // TODO resolve the above cleanly
    static ClassConfidenceData confidence_data;

    static char user_comment[256]; // Buffer for UserComment string

    bool has_confidence_data;

    memset(&confidence_data, 0, sizeof(confidence_data));
    memset(user_comment, 0, sizeof(user_comment));

    cv_get_confidence_data(&confidence_data);
    has_confidence_data = ((confidence_data.class_count > 0) && (confidence_data.class_count <= MAX_CLASSES));

    // DEBUG message to check the difference between int and uint8_t
    //xprintf("EXIF: sizeof(confidence_data) = %d\n", sizeof(confidence_data));

    // Build UserComment string with model results
    if (has_confidence_data)  {
        char *ptr = user_comment;
        int remaining = sizeof(user_comment) - 1;

        //xprintf("EXIF: Building UserComment with %d classes\n", confidence_data.class_count);

        for (uint8_t i = 0; i < confidence_data.class_count && i < EXIF_MAX_DYNAMIC_CLASSES; i++)  {
            const char *label = confidence_data.labels[i] ? confidence_data.labels[i] : "Unknown";
            int conf = confidence_data.confidence_percent[i];

            // Format: "Class: label (conf%); "
            int written = snprintf(ptr, remaining, "%s: %d%%; ", label, conf);

            if (written > 0 && written < remaining) {
                ptr += written;
                remaining -= written;
            }
            else  {
                break; // Buffer full
            }
        }

        if (user_comment[0] != '\0')  {
            dynamic_ifd_count++; // Add one entry for UserComment
        }
    }
#endif //ENABLE_EXIF_CONFIDENCE

	// Get deployment ID from operational parameters
	char deployment_id[UUIDLENGTH];
	fatfs_getDeploymentId(deployment_id, sizeof(deployment_id));
	
	// Only include in EXIF if not all zeros (i.e., a deployment is active)
	bool has_deployment_id = (strcmp(deployment_id, DEPLOYMENT_ID_ZERO_UUID) != 0);


	if (has_deployment_id) {
		dynamic_ifd_count += 1; // Add one entry for DeploymentID
	}

    // Prepare the timestamp
    exif_utc_get_rtc_as_exif_string(timestamp, sizeof(timestamp));

    // Prepare the resolution entries
    uint32_t xres_rational[2] = {app_get_raw_width(), 1};
    uint32_t yres_rational[2] = {app_get_raw_height(), 1};
    uint16_t res_unit = 2; // TODO check this

    uint8_t *p = exif_buffer;

    // Add APP1 marker
    *p++ = 0xFF;
    *p++ = 0xE1;

    // Write placeholder for segment length
    uint8_t *len_ptr = p;
    p += 2;

    // "Exif\0\0"
    memcpy(p, "Exif\0\0", 6);
    p += 6;

    // TIFF header: Intel (II), magic 0x002A, IFD0 offset = 8
    tiff_start = p;
    memcpy(p, "II", 2);
    p += 2;
    write16_le(p, 0x002A);
    p += 2;
    write32_le(p, 0x00000008);
    p += 4;

    // The number of IFD0 entries goes here (dynamic based on class count)
    write16_le(p, dynamic_ifd_count);
    p += 2;

    // Keep a note of this location, which is where the IFD entries start
    uint8_t *ifd_start = p;

    p += dynamic_ifd_count * 12; // Skip past the IFD entries

    // Next IFD offset (0)
    // This is writing the offset to the next IFD (Image File Directory) after IFD0.
    // EXIF's TIFF format can chain multiple IFDs.
    // After the IFD0 entry list, there's a 4-byte field indicating the offset (from the TIFF header start) of the next IFD (like IFD1 or the EXIF SubIFD).
    // If there is no next IFD, it must be 0.
    write32_le(p, 0);
    p += 4;

    // Set pointer to first data location (after IFD)
    next_data_ptr = p;

    // Reserve space for the GPS IFD block before writing tag data
    uint8_t *gps_ifd_start = next_data_ptr;
    uint32_t gps_ifd_offset = (uint32_t)(gps_ifd_start - tiff_start);
    // Ensure we don't exceed buffer when reserving GPS IFD
    size_t gps_size = get_gps_ifd_size();
    if ((size_t)(next_data_ptr - exif_buffer) + gps_size > EXIF_MAX_LEN) {
        // If GPS won't fit, reduce gps_size to 0 (skip GPS)
        gps_size = 0;
    }
    next_data_ptr += gps_size; // reserve

    // Add IFD entries - base entries (9) + dynamic class entries
    uint8_t entry = 0;
    addIFD(TAG_MAKE, ifd_start + (entry++ * 12), "Wildlife.ai");
    addIFD(TAG_MODEL, ifd_start + (entry++ * 12), "WW500");
    addIFD(TAG_RESOLUTION_UNIT, ifd_start + (entry++ * 12), &res_unit);
    addIFD(TAG_X_RESOLUTION, ifd_start + (entry++ * 12), xres_rational);
    addIFD(TAG_Y_RESOLUTION, ifd_start + (entry++ * 12), yres_rational);
    addIFD(TAG_DATETIME_ORIGINAL, ifd_start + (entry++ * 12), timestamp);
    addIFD(TAG_CREATE_DATE, ifd_start + (entry++ * 12), timestamp);
    addIFD(TAG_NN_DATA, ifd_start + (entry++ * 12), nnData); // Neural network output (raw, for backwards compatibility)

#ifdef ENABLE_EXIF_CONFIDENCE
    // Add UserComment with model results if available
    // TODO is it best to use TAG_USER_COMMENT or make a new tag?
    if (has_confidence_data && user_comment[0] != '\0')  {
        xprintf("EXIF: Adding UserComment (%d classes): '%s'\n", confidence_data.class_count, user_comment);
        addIFD(TAG_USER_COMMENT, ifd_start + (entry++ * 12), user_comment);
    }
#else
    // Create an IFD containing the NN output tensor plus class labels
    // TODO is it best to use TAG_USER_COMMENT or make a new tag?

    char user_comment[EXIF_COMMENT_LENGTH];
	user_comment[0] = '\0';
	size_t offset = 0;

    if (cv_modelLoaded())  {

    	for (uint8_t i = 0; i < categoriesCount; i++) {
    	    int written = snprintf(
    	        user_comment + offset,
    	        EXIF_COMMENT_LENGTH - offset,
    	        "%s: %d; ",
				cv_getLabel(i),
    	        outCategories[i]
    	    );

    	    if (written < 0) {
    	        // encoding error
    	        break;
    	    }

    	    if ((size_t)written >= EXIF_COMMENT_LENGTH - offset) {
    	        // buffer full, string truncated
    	        offset = EXIF_COMMENT_LENGTH - 1;
    	        break;
    	    }

    	    offset += written;
    	}

        xprintf("EXIF: Adding UserComment (%d classes): '%s'\n", categoriesCount, user_comment);
        addIFD(TAG_USER_COMMENT, ifd_start + (entry++ * 12), user_comment);

        dynamic_ifd_count++; // Add one entry for UserComment
    }

#endif // ENABLE_EXIF_CONFIDENCE

	// Add deployment ID if present (not all zeros)
	if (has_deployment_id) {
		xprintf("EXIF: Adding DeploymentID: %s\n", deployment_id);
		addIFD(TAG_DEPLOYMENT_ID, ifd_start + (entry++ * 12), deployment_id);
	}

    // GPS:
    addIFD(TAG_GPS_IFD_POINTER, ifd_start + (entry++ * 12), &gps_ifd_offset);

    // Now write the GPS IFD structure if we reserved space
    if (gps_size > 0) {
        create_gps_ifd(gps_ifd_start);
    }

    // Fill in length field (BE!)
    // Protect against exceeding EXIF_MAX_LEN
    if ((size_t)(next_data_ptr - exif_buffer) > EXIF_MAX_LEN) {
        next_data_ptr = exif_buffer + EXIF_MAX_LEN;
    }
    uint16_t len = (next_data_ptr - exif_buffer) - 2; // exclude 0xFFE1 marker
    write16_be(len_ptr, len);

    exif_len = next_data_ptr - exif_buffer;

    return exif_len;
}

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
    uint8_t brightnessPercent;
    uint16_t mdInterval;
    FlashLeds_t ledInUse;

    brightnessPercent = (uint8_t)fatfs_getOperationalParameter(OP_PARAMETER_LED_BRIGHTNESS_PERCENT);
    ledInUse = fatfs_getOperationalParameter(OP_PARAMETER_FLASH_LED);
    mdInterval = fatfs_getOperationalParameter(OP_PARAMETER_MD_INTERVAL);

    // Should be off but let's be sure.
    ledFlashDisable();

#if defined(USE_HM0360) || defined(USE_HM0360_MD)
    // HM0360 as main camera
    if (hm0360_md_isHM0360Present()) {
    	XP_LT_GREY;
       	xprintf("Preparing HM0360 for MD\n");
    	hm0360_md_prepare(cameraSystemEnabled, mdInterval); // select CONTEXT_B registers (if enabled)

    	// Consider turning on the LED flashes, controlled by the HM0360 STROBE output
    	if ((brightnessPercent == 0) || (ledInUse == 0) || (mdInterval == 0))  {
    		// No STROBE pulses because brightness=0 or neither LED selected or MD is disabled
    		xprintf("   No LED flashes.\n");
    		hm0360_md_configureStrobe(0);
    	}
    	else {
    		// Configure STROBE pulses
    		xprintf("   LED flashes (Strobe mode 0x%02x)\n", HM0360_SENSOR_STROBE_MODE);
    		hm0360_md_configureStrobe(HM0360_SENSOR_STROBE_MODE);
    	}
    	XP_WHITE;
    }
    else {
    	xprintf("HM0360 missing...\n");
    }
#endif // USE_HM0360

// Now merged (above)
//#ifdef USE_HM0360_MD
//    // HM0360 for motion detection only.
//    // If the camera system is disabled then ensure MD is off and flash LED is off
//    if (hm0360_md_isHM0360Present()) {
//    	hm0360_md_prepare(enabled, mdInterval); // select CONTEXT_B registers (if enabled)
//
//    	// Consider turning on the LED flashes, controlled by the HM0360 STROBE output
//    	if (!enabled) {
//    		// No STROBE pulses
//    		xprintf("Camera disabled - no LED flashes\n");
//    		hm0360_md_configureStrobe(0);
//    	}
//    	else if ((brightnessPercent == 0) || (ledInUse == 0))  {
//    		// No STROBE pulses
//    		xprintf("Preparing HM0360 for MD - no LED flashes\n");
//    		hm0360_md_configureStrobe(0);
//    	}
//    	else   {
//            // Configure STROBE pulses - NOTE: normal mode is 0x0b = 'dynamic 2'
//            xprintf("Preparing HM0360 for MD - with LED flashes 0x%02x\n", fatfs_getOperationalParameter(OP_PARAMETER_STROBE_MODE));
//            hm0360_md_configureStrobe(HM0360_SENSOR_STROBE_MODE);
//    	}
//    }
//    else {
//    	// HM0360 is missing
//    	xprintf("HM0360 missing...\n");
//    }
//
//#endif // USE_HM0360_MD

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

    if (timelapseDelay > 0) {
        // Enable wakeup on WAKE pin and timer
        sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN | SLEEPMODE_WAKE_SOURCE_RTC,
                             timelapseDelay, false); // Does not return
    }
    else  {
        // If the OP_PARAMETER_TIMELAPSE_INTERVAL setting is 0 then we don't enable a timer wakeup
        sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN, 0, false); // Does not return
    }
}

