
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
#include "c_api_types.h"	// Tensorflow errors
#include "hx_drv_scu.h"

#include "hm0360_md.h"
#include "hm0360_regs.h"


/*************************************** Definitions *******************************************/

// TODO sort out how to allocate priorities
#define image_task_PRIORITY (configMAX_PRIORITIES - 2)

#define IMAGE_TASK_QUEUE_LEN 10

// This is experimental. TODO check it is ok
#define MSGTOMASTERLEN 100

// defaults for PWM output on PB9 for Flash LED brightness
// default 20kHz
#define FLASHLEDFREQ 20000

// enable this to leave the PWM running so we can watch it on the scope.
#define FLASHLEDTEST 1

// Warning: if using 8.3 file names then this applies to directories also

#ifdef USE_HM0360
// Name of file containing extra HM0360 register settings
#define CAMERA_EXTRA_FILE "HM0360EX.BIN"
#elif defined (USE_RP2)
// Name of file containing extra RP2 register settings
#define CAMERA_EXTRA_FILE "RPV2_EX.BIN"
#elif defined (USE_RP3)
// Name of file containing extra RP3 register settings
#define CAMERA_EXTRA_FILE "RPV3_EX.BIN"
#else
// Should not happen. Add something anyway
#define CAMERA_EXTRA_FILE "CAMERA_EX.BIN"
#endif	// USE_HM0360


#define EXIF_MAX_LEN 512	// I have seen 350 used...

// The number of IFD entries in build_exif_segment()
#define IFD0_ENTRY_COUNT 		9
// The number of IFD entries in create_gps_ifd()
#define GPS_IFD_ENTRY_COUNT 	6
#define GPS_IFD_SIZE (2 + GPS_IFD_ENTRY_COUNT * 12 + 4)

// Tag IDs enum
typedef enum {
    TAG_X_RESOLUTION       = 0x011A,
    TAG_Y_RESOLUTION       = 0x011B,
    TAG_RESOLUTION_UNIT    = 0x0128,
    TAG_DATETIME_ORIGINAL  = 0x9003,
    TAG_CREATE_DATE        = 0x9004,
    TAG_MAKE               = 0x010F,
    TAG_MODEL              = 0x0110,
	TAG_GPS_IFD_POINTER    = 0x8825,
    TAG_GPS_LATITUDE_REF   = 0x0001,
    TAG_GPS_LATITUDE       = 0x0002,
    TAG_GPS_LONGITUDE_REF  = 0x0003,
    TAG_GPS_LONGITUDE      = 0x0004,
    TAG_GPS_ALTITUDE_REF   = 0x0005,
    TAG_GPS_ALTITUDE       = 0x0006,
    TAG_NN_DATA        	   = 0xC000   // Neural network output array - arbitrary custom tag ID
} ExifTagID;

// EXIF data types
typedef enum {
    TYPE_BYTE      = 1,
    TYPE_ASCII     = 2,
    TYPE_SHORT     = 3,
    TYPE_LONG      = 4,
    TYPE_RATIONAL  = 5,
	UNDEFINED 		= 7,
    SLONG			= 9,
    SRATIONAL		= 10
} ExifDataType;

// If using the code that has a duplicate buffer then define SECOND_JPEG_BUFSIZE
// Must by a multiple of 32 bytes.
//#define JPEG_BUFSIZE  76800 //640*480/4
#define SECOND_JPEG_BUFSIZE  20000	// jpeg files seem to be about 9k-20k

/*************************************** Local Function Declarations *****************************/

// This is the FreeRTOS task
static void vImageTask(void *pvParameters);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForInit(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForCapturing(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForNNProcessing(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForWaitForTimer(APP_MSG_T img_recv_msg);
static APP_MSG_DEST_T handleEventForSaveState(APP_MSG_T img_recv_msg);

// This is to process an unexpected event
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage);

static bool configure_image_sensor(CAMERA_CONFIG_E operation, uint8_t flashDutyCycle);

// Send unsolicited message to the master
static void sendMsgToMaster(char * str);

static void generateImageDirectory(uint16_t number);
static void generateImageFileName(uint16_t number);
static void setFileOpFromJpeg(uint32_t jpeg_sz, uint32_t jpeg_addr);

// When final activity from the FatFS Task and IF Task are complete, enter DPD
static void sleepWhenPossible(void);
static void sleepNow(void);

// Three function that control the behaviour of the GPIO pin that produces the PWM signal for LED brightness
static void flashLEDPWMInit(void);
static void flashLEDPWMOn(uint8_t duty);
static void flashLEDPWMOff(void);

static void captureSequenceComplete(void);

static void changeEnableState(bool setEnabled);

/*************************************** Local EXIF-related Declarations *****************************/

// Insert the EXIF metadata into the jpeg buffer
static uint16_t insertExif(uint32_t jpeg_sz, uint32_t jpeg_addr, int8_t * outCategories, uint8_t categoriesCount);

static void write16_le(uint8_t *ptr, uint16_t val);
static void write32_le(uint8_t *ptr, uint32_t val);
static void write16_be(uint8_t *ptr, uint16_t val);
//static void write32_be(uint8_t *ptr, uint32_t val);
static void addIFD(ExifTagID tagID, uint8_t *entry_ptr, void *tagData);
static uint16_t build_exif_segment(int8_t * outCategories, uint8_t categoriesCount);
static void create_gps_ifd(uint8_t *gps_ifd_start);
static size_t get_gps_ifd_size(void);

/*************************************** External variables *******************************************/

extern QueueHandle_t xFatTaskQueue;
extern QueueHandle_t     xIfTaskQueue;

extern SemaphoreHandle_t xFatCanSleepSemaphore;
extern SemaphoreHandle_t xIfCanSleepSemaphore;

/*************************************** Local variables *******************************************/

#ifdef SECOND_JPEG_BUFSIZE
__attribute__(( section(".bss.NoInit"))) uint8_t jpeg_exif_buf[SECOND_JPEG_BUFSIZE] __ALIGNED(32);
#endif	// SECOND_JPEG_BUFSIZE

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
static uint32_t g_timer_period;	// Interval between pictures in ms

#ifndef USE_HM0360
// TODO check out this function: sensordplib_set_rtc_start(SENDPLIB_PERIODIC_TIMER_MS);
static TimerHandle_t captureTimer;
#endif	// USE_HM0360

static fileOperation_t fileOp;

// This is a value passed to cisdp_dp_init()
// where the comment is "JPEG Encoding quantization table Selection (4x or 10x)"
uint32_t g_img_data;

static uint16_t g_imageSeqNum; // 0 indicates no SD card

// Strings for each of these states. Values must match APP_IMAGE_TASK_STATE_E in image_task.h
const char *imageTaskStateString[APP_IMAGE_TASK_STATE_NUMSTATES] = {
    "Uninitialised",
    "Init",
    "Capturing",
    "NN Processing",
    "Wait For Timer",
	"Save State",
};

// Strings for expected messages. Values must match messages directed to image Task in app_msg.h
const char *imageTaskEventString[APP_MSG_IMAGETASK_LAST - APP_MSG_IMAGETASK_FIRST] = {
    "Image Event Inactivity",
    "Image Event Start Capture",
    "Image Event Stop Capture",
    "Image Event ReCapture",
    "Image Event Frame Ready",
    "Image Event Done",
    "Image Event Disk Write Complete",
    "Image Event Change Enable",
    "Image Event Error",
};

TickType_t xLastWakeTime;

// There is only one file name for images - this can be declared here - does not need malloc
static char imageFileName[IMAGEFILENAMELEN];

// This is the most recently written file name
static char lastImageFileName[IMAGEFILENAMELEN] = "";

static char msgToMaster[MSGTOMASTERLEN];

static bool nnPositive;

// True means we capture images and run NN processing and report results.
static uint8_t nnSystemEnabled;	// 0 = disabled 1 = enabled

// Support for EXIF
static uint8_t exif_buffer[EXIF_MAX_LEN];

// Global cursor to where non-inline data will be appended
static uint8_t *next_data_ptr;

static uint8_t *tiff_start;

/********************************** Local Functions  *************************************/

#ifndef USE_HM0360
/**
 * This is the local callback that executes when the capture_timer expires
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
	send_msg.msg_event = APP_MSG_IMAGETASK_RECAPTURE;

	// Timer callbacks run in the context of a task, so the non ISR version can be used
	if (xQueueSend(xImageTaskQueue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
		xprintf("send_msg=0x%x fail\r\n", send_msg.msg_event);
	}
}
#endif // USE_HM0360

/**
 * Fabricate a directory
 *
 * Place the name in imageFileName
 *
 * @param number - this forms part of the directory name
 */
static void generateImageDirectory(uint16_t number) {
	// do nothing for now.
	// Perhaps this should be at the start of vFatFsTask()?
}

/**
 * Fabricate a file name
 *
 * Place the name in imageFileName
 *
 * @param number - this forms part of the file name
 */
static void generateImageFileName(uint16_t number) {
#if FF_USE_LFN
    // Create a file name
	// file name: 'image_2025-02-03_1234.jpg' = 25 characters, plus trailing '\0'
	rtc_time time;
	exif_utc_get_rtc_as_time(&time);
    snprintf(imageFileName, IMAGEFILENAMELEN, "image_%d-%02d-%02d_%04d.jpg",
    		time.tm_year, time.tm_mon, time.tm_mday, (uint16_t) frame_num);
#else
    // Must use 8.3 file name: upper case alphanumeric
    if (woken == APP_WAKE_REASON_MD)  {
    	// Motion has woken us
    	snprintf(imageFileName, IMAGEFILENAMELEN, "MD%06d.JPG", (uint16_t) number);
    }
    else {
    	// Must be a time lapse event
    	snprintf(imageFileName, IMAGEFILENAMELEN, "TL%06d.JPG", (uint16_t) number);
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
static void setFileOpFromJpeg(uint32_t jpeg_sz, uint32_t jpeg_addr) {

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

    //dbg_printf(DBG_LESS_INFO, "os_app_dplib_cb event = %d\n", event);

    switch (event) {
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
//    dbg_printf(DBG_LESS_INFO, "Received event 0x%04x from Sensor Datapath. Sending 0x%04x  to Image Task\r\n",
//    		dp_msg.msg_event, dp_msg.msg_data);
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
	uint8_t dutyCycle;

    APP_MSG_T internal_msg;

	event = img_recv_msg.msg_event;
	send_msg.destination = NULL;

	switch (event)  {

	case APP_MSG_IMAGETASK_STARTCAPTURE:
		// MD or timer or the CLI task has asked us to start capturing
		requested_captures = (uint16_t) img_recv_msg.msg_data;
		requested_period = img_recv_msg.msg_parameter;

		// Check parameters are acceptable
		if ((requested_captures < MIN_IMAGE_CAPTURES) || (requested_captures > MAX_IMAGE_CAPTURES) ||
				(requested_period < MIN_IMAGE_INTERVAL) || (requested_period > MAX_IMAGE_INTERVAL) ) {
			xprintf("Invalid parameter values %d or %d\n", requested_captures, requested_period);
		}
		else {
			g_captures_to_take = requested_captures;
			g_timer_period = requested_period;
			XP_LT_GREEN
			xprintf("Images to capture: %d\n", g_captures_to_take);
			xprintf("Interval: %dms\n", g_timer_period);

			xLastWakeTime = xTaskGetTickCount();

	    	// Turn on the PWM that determines the flash intensity
	    	dutyCycle = (uint8_t) fatfs_getOperationalParameter(OP_PARAMETER_LED_FLASH_DUTY);
	    	xprintf("Flash duty cycle %d\%\n", dutyCycle);
			XP_WHITE;

			flashLEDPWMOn(dutyCycle);

			// Now start the image sensor.
			configure_image_sensor(CAMERA_CONFIG_RUN, dutyCycle);

			// The next thing we expect is a frame ready message: APP_MSG_IMAGETASK_FRAME_READY
			image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
		}
		break;

	case APP_MSG_IMAGETASK_CHANGE_ENABLE:
		// We have received an instruction to enable of disable the NN processing system
		setEnabled = (bool) img_recv_msg.msg_data;
		changeEnableState(setEnabled);	// 0 means disabled; 1 means enabled
		if (setEnabled) {
			xprintf("DEBUG: Time to start capturing images!\n");
	    	// Pass the parameters in the ImageTask message queue
	    	internal_msg.msg_data = fatfs_getOperationalParameter(OP_PARAMETER_NUM_PICTURES);
	    	internal_msg.msg_parameter = fatfs_getOperationalParameter(OP_PARAMETER_PICTURE_INTERVAL);
	    	internal_msg.msg_event = APP_MSG_IMAGETASK_STARTCAPTURE;

	    	if (xQueueSend(xImageTaskQueue, (void *)&internal_msg, __QueueSendTicksToWait) != pdTRUE) {
	    		xprintf("Failed to send 0x%x to imageTask\r\n", internal_msg.msg_event);
	    	}
		}
		break;

	case APP_MSG_IMAGETASK_INACTIVITY:
		// Inactivity detected. Prepare to enter DPD
		configure_image_sensor(CAMERA_CONFIG_STOP, 0); // run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360

		if (fatfs_getImageSequenceNumber() > 0) {
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
	    	sleepWhenPossible();	// does not return
		}

		break;

	default:
		flagUnexpectedEvent(img_recv_msg);

	}	// switch

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

	TickType_t startTime;
	TickType_t elapsedTime;
	uint32_t elapsedMs;
	TfLiteStatus ret;
	bool setEnabled;
	uint16_t exif_len;

	// Signed integers
    int8_t outCategories[CATEGORIESCOUNT];

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;
    HM0360_GAIN_T gain;

    switch (event)  {

    case APP_MSG_IMAGETASK_FRAME_READY:
    	// Here when the image sub-ssytem has captured an image.

    	// Turn off the Flash LED PWN signal
    	//flashLEDPWMOff();

    	// frame ready event received from os_app_dplib_cb
        g_cur_jpegenc_frame++;	// The number in this sequence
        g_frames_total++;		// The number since the start of time.

        // run NN processing
        // This gets the input image address and dimensions from:
        // app_get_raw_addr(), app_get_raw_width(), app_get_raw_height()

		startTime = xTaskGetTickCount();

		ret = cv_run(outCategories, CATEGORIESCOUNT);

		elapsedTime = xTaskGetTickCount() - startTime;
		elapsedMs = (elapsedTime * 1000) / configTICK_RATE_HZ;

		if (ret == kTfLiteOk) {
			// OK
        	fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_NN_ANALYSES);

			if (outCategories[1] > 0) {
				XP_LT_GREEN;
				xprintf("PERSON DETECTED!\n");
				// NOTE this only works if CATEGORIESCOUNT == 2
	        	fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_POSITIVE_NN_ANALYSES);
	        	nnPositive = true;

	        	// Send a message to the BLE processor so it can inform the user on the app immediately
				snprintf(msgToMaster, MSGTOMASTERLEN, "NN+");
				sendMsgToMaster(msgToMaster);
			}
			else {
				XP_LT_RED;
				xprintf("No person detected.\n");
				XP_WHITE;
			}
			XP_WHITE;
			xprintf("Score %d/128. NN processing took %dms\n\n", outCategories[1], elapsedMs);
		}
		else {
			// NN error.
			// What do we do here?
			XP_RED;
			xprintf("NN error %d. NN processing took %dms\n\n", ret, elapsedMs);
			XP_WHITE;
		}
#if defined (USE_HM0360) || defined (USE_HM0360_MD)
		// This is a test to see if/how these change with illumination
		hm0360_md_getGainRegs(&gain);
#endif

		XP_LT_GREY;
		xprintf("Gain regs: Int = 0x%04x, Analog = 0x%02x, Digital = 0x%04x, AEMean = 0x%02x, AEConverge = 0x%02x\n",
				gain.integration, gain.analogGain, gain.digitalGain, gain.aeMean, gain.aeConverged);
		XP_WHITE;

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

        cisdp_get_jpginfo(&jpeg_sz, &jpeg_addr);	// Revised size after inserting EIF

		// JPEF buffer exists but this will insert EXIF data and increase jpeg_sz
    	exif_len = insertExif(jpeg_sz, jpeg_addr, outCategories, CATEGORIESCOUNT);

#if 0
    	// Check by printing some of the buffer that includes the EXIF
        //uint16_t bytesToPrint = exif_len + 8;
        uint16_t bytesToPrint = 16;

        XP_LT_GREY;
    	xprintf("JPEG&EXIF buffer (%d bytes) begins:\n", (jpeg_sz + exif_len));
    	for (int i = 0; i < bytesToPrint; i++) {
    	    xprintf("%02X ", jpeg_exif_buf[i]);
    	    if (i%16 == 15) {
    	    	xprintf("\n");
    	    }
    	}
    	xprintf("\n");
        XP_WHITE;
#endif

        // TODO should consider updating the image directory name so it does not get too full (slows things)
        // For now an empty stub here as a reminder. Perhaps this should happen in vFatFsTask()
		// Set the fileOp structure.
        generateImageDirectory(0);

        g_imageSeqNum = fatfs_getImageSequenceNumber();
        generateImageFileName(g_imageSeqNum);

		setFileOpFromJpeg((jpeg_sz + exif_len), (uint32_t) jpeg_exif_buf);

		dbg_printf(DBG_LESS_INFO, "Writing %d bytes (%d + %d) to '%s' from 0x%08x\n",
				fileOp.length, jpeg_sz, exif_len, fileOp.fileName, fileOp.buffer);

		// Save the file name as the most recent image
		snprintf(lastImageFileName, IMAGEFILENAMELEN, "%s", fileOp.fileName);

		send_msg.destination = xFatTaskQueue;
		send_msg.message.msg_event = APP_MSG_FATFSTASK_WRITE_FILE;
		send_msg.message.msg_data = (uint32_t)&fileOp;

		// Also see if it is appropriate to send a "Event" message to the BLE processor
		// TODO this won't work if the preceding 'NN+' message is being sent!
		if ((g_cur_jpegenc_frame == g_captures_to_take) && nnPositive) {

			// TODO DREADFUL HACK!!!

			// This should be replaced with semaphores that delay until preceding messages have been sent!
			// Instead as a quick hack I am adding a delay here in the hope that the preceding "NN+" message
			// will get through

			vTaskDelay(pdMS_TO_TICKS(100));

			// Inform BLE processor
			// For the moment the message body is identical to the "Sleep " message but we might chnage this later
			snprintf(msgToMaster, MSGTOMASTERLEN, "Event ");

			for (uint8_t i=0; i < OP_PARAMETER_NUM_ENTRIES; i++) {
				uint8_t next = strlen(msgToMaster);	// points to the place where we should write the next parameter
				snprintf(&msgToMaster[next], sizeof(msgToMaster), "%d ", fatfs_getOperationalParameter(i));
			}
			sendMsgToMaster(msgToMaster);
		}

		// Wait in this state till the disk write completes - expect APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE
    	image_task_state = APP_IMAGE_TASK_STATE_NN_PROCESSING;

    	break;

	case APP_MSG_IMAGETASK_CHANGE_ENABLE:
		// We have received an instruction to enable or disable the NN processing system
		setEnabled = (bool) img_recv_msg.msg_data;
		changeEnableState(setEnabled);	// 0 means disabled; 1 means enabled
		break;

    case APP_MSG_IMAGETASK_INACTIVITY:
    	// I have seen this, followed soon after by
    	dbg_printf(DBG_LESS_INFO, "Inactive - expect WDT timeout?\n");
    	break;

    case APP_MSG_DPEVENT_EDM_WDT1_TIMEOUT:
    case APP_MSG_DPEVENT_EDM_WDT2_TIMEOUT:
    case APP_MSG_DPEVENT_EDM_WDT3_TIMEOUT:
    case APP_MSG_DPEVENT_SENSORCTRL_WDT_OUT:
    	// Unfortunately I see this. EDM WDT2 Timeout = 0x011c
    	// probably if HM0360 is not receiving I2C commands...
    	dbg_printf(DBG_LESS_INFO, "Received a timeout event. TODO - re-initialise HM0360?\n");

    	// Fault detected. Prepare to enter DPD
		configure_image_sensor(CAMERA_CONFIG_STOP, 0); // run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360

		if (fatfs_getImageSequenceNumber() > 0) {
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
	    	sleepWhenPossible();	// does not return
		};
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

    switch (event) {

    case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
        if ( diskStatus == 0) {
        	fatfs_incrementOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER);
        }
        else {
            dbg_printf(DBG_LESS_INFO, "Image not written. Error: %d\n", diskStatus);
        }

        // This represents the point at which an image has been captured and processed.

		if (g_cur_jpegenc_frame == g_captures_to_take) {
			captureSequenceComplete();
			image_task_state = APP_IMAGE_TASK_STATE_INIT;
		}
#ifdef USE_HM0360
        else {
			// The HM0360 uses an internal timer to determine the time for the next image
        	// Re-start the image sensor.
			configure_image_sensor(CAMERA_CONFIG_CONTINUE, 0);
        	// Expect another frame ready event
        	image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
        }
#else
        else {
        	// Start a timer that delays for the defined interval.
        	// When it expires, switch to CAPTURUNG state and request another image
            if (captureTimer != NULL)  {
                // Change the period and start the timer
            	// The callback issues a APP_MSG_IMAGETASK_RECAPTURE event
                xTimerChangePeriod(captureTimer, pdMS_TO_TICKS(g_timer_period), 0);
                // Expect a APP_MSG_IMAGETASK_RECAPTURE event from the capture_timer
                image_task_state = APP_IMAGE_TASK_STATE_WAIT_FOR_TIMER;
            }
            else {
            	// error
            	flagUnexpectedEvent(img_recv_msg);
                image_task_state = APP_IMAGE_TASK_STATE_INIT;
            }
        }
#endif // USE_HM0360
        break;


	case APP_MSG_IMAGETASK_CHANGE_ENABLE:
		// We have received an instruction to enable of disable the NN processing system
		setEnabled = (bool) img_recv_msg.msg_data;
		changeEnableState(setEnabled);	// 0 means disabled; 1 means enabled
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
 * 		APP_MSG_IMAGETASK_RECAPTURE
 *
 * @param APP_MSG_T img_recv_msg
 * @return APP_MSG_DEST_T send_msg
 */
static APP_MSG_DEST_T handleEventForWaitForTimer(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;

	bool setEnabled;
	uint8_t dutyCycle;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event) {

    case APP_MSG_IMAGETASK_RECAPTURE:
    	// here when the capture_timer expires

    	// Turn on the PWM that determines the flash intensity
    	dutyCycle = (uint8_t) fatfs_getOperationalParameter(OP_PARAMETER_LED_FLASH_DUTY);
    	xprintf("Flash duty cycle %d\% (Timer)\n", dutyCycle);
    	flashLEDPWMOn(dutyCycle);

        sensordplib_retrigger_capture();
        image_task_state = APP_IMAGE_TASK_STATE_CAPTURING;
        break;

	case APP_MSG_IMAGETASK_CHANGE_ENABLE:
		// We have received an instruction to enable of disable the NN processing system
		setEnabled = (bool) img_recv_msg.msg_data;
		changeEnableState(setEnabled);	// 0 means disabled; 1 means enabled
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
static APP_MSG_DEST_T handleEventForSaveState(APP_MSG_T img_recv_msg) {
    APP_MSG_DEST_T send_msg;
    APP_MSG_EVENT_E event;

	bool setEnabled;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    switch (event) {

    case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
    	// Here when the FatFS task has saved state
    	// Wait till the IF Task is also ready, then sleep
    	sleepWhenPossible();	// does not return
        break;

	case APP_MSG_IMAGETASK_CHANGE_ENABLE:
		// We have received an instruction to enable of disable the NN processing system
		setEnabled = (bool) img_recv_msg.msg_data;
		changeEnableState(setEnabled);	// 0 means disabled; 1 means enabled
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
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T img_recv_msg) {
    APP_MSG_EVENT_E event;
    APP_MSG_DEST_T send_msg;

    event = img_recv_msg.msg_event;
    send_msg.destination = NULL;

    XP_LT_RED;
    if ((event >= APP_MSG_IMAGETASK_FIRST) && (event < APP_MSG_IMAGETASK_LAST)) {
        xprintf("IMAGE task unhandled event '%s' in '%s'\r\n", imageTaskEventString[event - APP_MSG_IMAGETASK_FIRST], imageTaskStateString[image_task_state]);
    }
    else  {
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
     		(int) g_captures_to_take, lastImageFileName);
     sendMsgToMaster(msgToMaster);

     // Reset counters
     g_captures_to_take = 0;
     g_cur_jpegenc_frame = 0;
     flashLEDPWMOff();
}

// Three function that control the behaviour of the GPIO pin that produces the PWM signal for LED brightness

/**
 * Initialise a GPIO pin as a PWM output for control of Flash LED brightness.
 *
 * This is PB9, SENSOR_GPIO, routed to the PSU chip that drives the IC that drives the Flash LED.
 *
 * TODO move to pinmux_init()?
 */
static void flashLEDPWMInit(void) {
	// Uncomment this if PB9 is to be used as the green LED.
	// Otherwise it can be PWM for the Flash LED
#ifdef PB9ISLEDGREEN
	XP_LT_RED;
	xprintf("Warning: PB9 in use for LED\n");
#else

	PWM_ERROR_E ret;

	// The output pin of PWM1 is routed to PB9
	ret = hx_drv_scu_set_PB9_pinmux(SCU_PB9_PINMUX_PWM1, 1);

	XP_LT_BLUE;
	if (ret == PWM_NO_ERROR) {
		xprintf("Initialised PB9 for PWM output\n");
		// initializes the PWM1
		ret = hx_drv_pwm_init(PWM1, HW_PWM1_BASEADDR);

		if (ret == PWM_NO_ERROR) {
			xprintf("Initialised flash LED on PB9\n");
		}
		else {
			xprintf("Flash LED initialisation on PB9 fails: %d\n", ret);
		}
	}
	else {
		xprintf("PB9 init for PWM output fails: %d\n", ret);
	}

	XP_WHITE;
#endif //  PB9ISLEDGREEN
}

/**
 * Turns on the PWM signal
 *
 * @param duty - value between 1 and 99 representing duty cycle in percent
 */
static void flashLEDPWMOn(uint8_t duty) {
#ifdef PB9ISLEDGREEN
	//PB9 in use for LED
#else
	pwm_ctrl ctrl;

	if ((duty > 0) && (duty <100)) {
		// PWM1 starts outputting according to the set value.
		// (The high period is 20%, and the low period is 80%)
		ctrl.mode = PWM_MODE_CONTINUOUS;
		ctrl.pol = PWM_POL_NORMAL;
		ctrl.freq = FLASHLEDFREQ;
		ctrl.duty = duty;
		hx_drv_pwm_start(PWM1, &ctrl);
	}
	else {
		xprintf("Invalid PWM duty cycle\n");
		hx_drv_pwm_stop(PWM1);
	}
#endif // PB9ISLEDGREEN
}

/**
 * Turns off the PWM signal.
 *
 */
static void flashLEDPWMOff(void) {

#ifdef PB9ISLEDGREEN
	//PB9 in use for LED
#else
#ifdef FLASHLEDTEST
	// leave it on so we can check on the scope
#else
	hx_drv_pwm_stop(PWM1);
#endif //  FLASHLEDTEST

#endif //  PB9ISLEDGREEN
}


/**
 * Enable or disable the camera system.
 *
 * True means we capture images and run NN processing and report results.
 *
 * @param setEnabled enable if true
 */
static void changeEnableState(bool setEnabled) {

	if (setEnabled) {
		nnSystemEnabled = 1;
		fatfs_setOperationalParameter(OP_PARAMETER_CAMERA_ENABLED, 1);
	}
	else{
		nnSystemEnabled = 0;
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
    bool cameraInitialised;

    g_frames_total = 0;
    g_cur_jpegenc_frame = 0;
    g_captures_to_take = 0;
    g_timer_period = 0;
    g_img_data = 0;
    g_imageSeqNum = 0;	// 0 indicates no SD card

    // True means we capture images and run NN processing and report results.
    nnSystemEnabled = fatfs_getOperationalParameter(OP_PARAMETER_CAMERA_ENABLED);

    nnPositive = false;	// true if the NN analysis detects something

    XP_CYAN;
    // Observing these messages confirms the initialisation sequence
    xprintf("Starting Image Task\n");
    XP_WHITE;

    // Should initialise the camera but not start taking images
#ifdef USE_HM0360
    if (woken == APP_WAKE_REASON_COLD)  {
    	cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_COLD, 0);
    }
    else {
    	cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_WARM, 0);
    }
#else
    // For RP camera the SENSOR_ENABLE signal has been turned off during DPD, so must re-initialise all registers
    cameraInitialised = configure_image_sensor(CAMERA_CONFIG_INIT_COLD, 0);
#endif // USE_HM0360


    if (!cameraInitialised) {
    	xprintf("\nEnter DPD mode because there is no camera!\n\n");
    	vTaskDelay(pdMS_TO_TICKS(1000));	//do we need to pause?

    	sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN, 0, false);	// Does not return
    }

#ifdef USE_HM0360
    // The HM0360 is our main camera
    hm0360_md_init(true, woken == APP_WAKE_REASON_COLD);
#elif defined (USE_HM0360_MD)
    // The HM0360 is not our main camera but we are using it for motion detection.
    // There is some more initialisation required.
    hm0360_md_init(false, woken == APP_WAKE_REASON_COLD);
#endif	// USE_HM0360

#if 0
	// This is a test of reading and printing the MD registers
#if defined (USE_HM0360) || defined (USE_HM0360_MD)

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

	// Experiment: clear after printing MD regs?
	//hm0360_md_clear_interrupt(0xff);		// clear all bits

#endif	// USE_HM0360_MD
#endif // 0

    flashLEDPWMInit();

    // Computer vision init
    if (cv_init(true, true) < 0)  {
    	xprintf("cv init fail\n");
        configASSERT(0);
    }
    else {
        xprintf("Initialised neural network.\n");
    }

    // A value of 0 means no SD card so we can skip all SD card activities
    g_imageSeqNum = fatfs_getImageSequenceNumber();

    // Initial state of the image task (initialized)
    image_task_state = APP_IMAGE_TASK_STATE_INIT;

    // If we woke because of motion detection or timer then let's send ourselves an initial
    // message to take some photos.

    // But only if nnSystemEnabled!

    if ((nnSystemEnabled == 1) &&((woken == APP_WAKE_REASON_MD) || (woken == APP_WAKE_REASON_TIMER))) {
    	// Pass the parameters in the ImageTask message queue
    	internal_msg.msg_data = fatfs_getOperationalParameter(OP_PARAMETER_NUM_PICTURES);
    	internal_msg.msg_parameter = fatfs_getOperationalParameter(OP_PARAMETER_PICTURE_INTERVAL);
    	internal_msg.msg_event = APP_MSG_IMAGETASK_STARTCAPTURE;

    	if (xQueueSend(xImageTaskQueue, (void *)&internal_msg, __QueueSendTicksToWait) != pdTRUE) {
    		xprintf("Failed to send 0x%x to imageTask\r\n", internal_msg.msg_event);
    	}
    }

    // Loop forever, taking events from xImageTaskQueue as they arrive
    for (;;)  {
        // Wait for a message in the queue
        if(xQueueReceive(xImageTaskQueue, &(img_recv_msg), __QueueRecvTicksToWait) == pdTRUE)  {
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

            switch (image_task_state)  {
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

            default:
                send_msg = flagUnexpectedEvent(img_recv_msg);
                break;
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
                if (xQueueSend(target_queue, (void *)&send_msg, __QueueSendTicksToWait) != pdTRUE) {
                    xprintf("IMAGE task sending event 0x%x failed\r\n", send_msg.message.msg_event);
                }
                else {
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
 * @param numFrames - number of frames we ask the HM0360 to take
 * @return true if initialised. false if no working camera
 */
static bool configure_image_sensor(CAMERA_CONFIG_E operation, uint8_t flashDutyCycle) {

	switch (operation) {
	case CAMERA_CONFIG_INIT_COLD:
        if (cisdp_sensor_init(true) < 0) {
            xprintf("\r\nCIS Init fail\r\n");
            return false;
        }
        else {

        	// Initialise extra registers from file
        	cis_file_process(CAMERA_EXTRA_FILE);

            // if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
            //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
            if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
                xprintf("\r\nDATAPATH Init fail\r\n");
                return false;
            }
        }
		break;

	case CAMERA_CONFIG_INIT_WARM:
        if (cisdp_sensor_init(false) < 0) {
            xprintf("\r\nCIS Init fail\r\n");
            return false;
        }
        else {
            // if wdma variable is zero when not init yet, then this step is a must be to retrieve wdma address
            //  Datapath events give callbacks to os_app_dplib_cb() in dp_task
            if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
                xprintf("\r\nDATAPATH Init fail\r\n");
                return false;
            }
        }
		break;

	case CAMERA_CONFIG_RUN:
        if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
            xprintf("\r\nDATAPATH Init fail\r\n");
            return false;
        }
#if defined (USE_HM0360) || defined (USE_HM0360_MD)
    	// Overide HM0360 STROBE setting
    	if (flashDutyCycle == 0) {
    		hm0360_md_configureStrobe(0);
    	}
#endif

#ifdef USE_HM0360
    	hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, 1, g_timer_period);
#endif // USE_HM0360
		cisdp_sensor_start(); // Starts data path sensor control block
		break;

	case CAMERA_CONFIG_CONTINUE:
        if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, os_app_dplib_cb, g_img_data, APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
            xprintf("\r\nDATAPATH Init fail\r\n");
            return false;
        }
		cisdp_sensor_start(); // Starts data path sensor control block
		break;

	case CAMERA_CONFIG_STOP:
        xprintf("Stopping sensor\n");
        cisdp_sensor_stop();	// run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360
		break;

#ifdef USE_HM0360
	case CAMERA_CONFIG_MD:
		// Now we can ask the HM0360 to get ready for DPD
		hm0360_md_enableMD(); // select CONTEXT_B registers
		// Do some configuration of MD parameters
		//hm0360_x_set_threshold(10);
		break;
#endif // USEHM0360

	default:
		// should not happen
		return false;
		break;
	}

    return true;
}

/**
 * Send an unsolicited message to the MKL62BA.
 *
 * This is experimental...
 */
static void sendMsgToMaster(char * str) {
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

	xprintf("Waiting for IF task to finish.\n");
	xSemaphoreTake(xIfCanSleepSemaphore, portMAX_DELAY);

	sleepNow();
}

/**
 * Enter DPD
 *
 * This is a separate routine from sleepWhenPossible since it is called from 2 places
 */
static void sleepNow(void) {
	uint32_t timelapseDelay;

#ifdef USE_HM0360
	if (nnSystemEnabled) {
		xprintf("Preparing HM0360 for MD\n");
		configure_image_sensor(CAMERA_CONFIG_MD, 0);
	}
	else {
		// camera system is not enabled.
		// I think something else has already set its mode to SLEEP
	}
#endif	// USE_HM0360


#ifdef USE_HM0360_MD
	xprintf("Preparing HM0360 for MD\n");
	hm0360_md_prepare();	 // select CONTEXT_B registers
#endif	// USE_HM0360_MD

	xprintf("\nEnter DPD mode!\n\n");

	timelapseDelay = fatfs_getOperationalParameter(OP_PARAMETER_TIMELAPSE_INTERVAL);

	if (timelapseDelay > 0) {
		// Enable wakeup on WAKE pin and timer
		sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN | SLEEPMODE_WAKE_SOURCE_RTC,
				timelapseDelay, false);	// Does not return
	}
	else {
		// If the OP_PARAMETER_TIMELAPSE_INTERVAL setting is 0 then we don't enable a timer wakeup
		sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN, 0, false);	// Does not return
	}
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
		int8_t * outCategories, uint8_t categoriesCount) {
    uint16_t exif_len = 0;
    uint8_t * jpeg_buf;

    jpeg_buf = (uint8_t *) jpeg_addr;

	// Sanity check: must start with FFD8 and then FFE0
	if (jpeg_buf[0] != 0xFF || jpeg_buf[1] != 0xD8 || jpeg_buf[2] != 0xFF || jpeg_buf[3] != 0xE0) {
	    // Handle error: unexpected JPEG structure
		return 0;
	}

	// Build EXIF segment - placed in exif_buffer[] and size in exif_len
	exif_len = build_exif_segment(outCategories, categoriesCount);

	// Check for enough space (depends on your memory layout)
	if (jpeg_sz + exif_len > SECOND_JPEG_BUFSIZE) {
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
	jpeg_exif_buf[0] = 0xff;	// Add the JPEG marker 0xffd8
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
	//memmove((uint8_t *) jpeg_addr + 2 + exif_len, (uint8_t *) jpeg_addr + 2, jpeg_sz - 2);

	// Insert EXIF after SOI (at jpeg_buf[2])
	//memcpy((uint8_t *) jpeg_addr + 2, exif_buffer, exif_len);
	memcpy(&jpeg_buf[2], exif_buffer, exif_len);

#endif // SECOND_JPEG_BUFSIZE

	return exif_len;
}

// Helper to write 2- and 4-byte little endian values
static void write16_le(uint8_t *ptr, uint16_t val) {
    ptr[0] = val & 0xFF;
    ptr[1] = val >> 8;
}

static void write32_le(uint8_t *ptr, uint32_t val) {
    ptr[0] = val & 0xFF;
    ptr[1] = (val >> 8) & 0xFF;
    ptr[2] = (val >> 16) & 0xFF;
    ptr[3] = (val >> 24) & 0xFF;
}

// Helper to write 2- and 4-byte big endian values
static void write16_be(uint8_t *ptr, uint16_t val) {
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
static void addIFD(ExifTagID tagID, uint8_t *entry_ptr, void *tagData) {
	switch (tagID) {
	case TAG_X_RESOLUTION:
	case TAG_Y_RESOLUTION:{
		uint32_t *rational = (uint32_t *)tagData;
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_RATIONAL);
		write32_le(entry_ptr + 4, 1);
		write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
		write32_le(next_data_ptr, rational[0]); next_data_ptr += 4;
		write32_le(next_data_ptr, rational[1]); next_data_ptr += 4;
		break;
	}
	case TAG_RESOLUTION_UNIT: {
		uint16_t value = *(uint16_t *)tagData;
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_SHORT);
		write32_le(entry_ptr + 4, 1);
		write16_le(entry_ptr + 8, value);
		write16_le(entry_ptr +10, 0);
		break;
	}
	case TAG_DATETIME_ORIGINAL:
	case TAG_CREATE_DATE:
	case TAG_MAKE:
	case TAG_MODEL:
	case TAG_GPS_LATITUDE_REF:
	case TAG_GPS_LONGITUDE_REF: {
		char *ascii = (char *)tagData;
		uint32_t length = strlen(ascii) + 1;  // include null terminator
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_ASCII);
		write32_le(entry_ptr + 4, length);

		if (length <= 4) {
			// Data will fit into the next 4 bytes
		    memset(entry_ptr + 8, 0, 4);
		    memcpy(entry_ptr + 8, ascii, length);
		}
		else {
			// Data needs to go elsewhere and we write the pointer to it here
		    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
		    memcpy(next_data_ptr, ascii, length);
		    next_data_ptr += length;
		}
		break;
	}
	case TAG_GPS_LATITUDE:
	case TAG_GPS_LONGITUDE: {
		uint32_t *dms = (uint32_t *)tagData; // 3 pairs of (numerator, denominator)
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_RATIONAL);
		write32_le(entry_ptr + 4, 3);
		write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
		for (int i = 0; i < 3; ++i) {
			write32_le(next_data_ptr, dms[i * 2]);     next_data_ptr += 4;
			write32_le(next_data_ptr, dms[i * 2 + 1]); next_data_ptr += 4;
		}
		break;
	}
	case TAG_GPS_ALTITUDE: {
		uint32_t *rational = (uint32_t *)tagData;
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_RATIONAL);
		write32_le(entry_ptr + 4, 1);
		write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
		write32_le(next_data_ptr, rational[0]); next_data_ptr += 4;
		write32_le(next_data_ptr, rational[1]); next_data_ptr += 4;
		break;
	}
	case TAG_GPS_ALTITUDE_REF: {
		uint8_t value = *(uint8_t *)tagData;
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_BYTE);
		write32_le(entry_ptr + 4, 1);
		memset(entry_ptr + 8, 0, 4);
		entry_ptr[8] = value;
		break;
	}
	case TAG_NN_DATA: {
		// For NN output we use a byte array, with the first entry being the number of bytes that follow
		uint8_t *bytes = (uint8_t *)tagData;
		uint32_t length = bytes[0]; // First byte is the length of the following data
		write16_le(entry_ptr, tagID);
		write16_le(entry_ptr + 2, TYPE_BYTE);
		write32_le(entry_ptr + 4, length);
		write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));

		if (length <= 4) {
			// Data will fit into the next 4 bytes
		    memset(entry_ptr + 8, 0, 4);	// pre-fill with zeros
		    memcpy(entry_ptr + 8, &bytes[1], length);
		}
		else {
			// Data needs to go elsewhere and we write the pointer to it here
		    write32_le(entry_ptr + 8, (uint32_t)(next_data_ptr - tiff_start));
		    memcpy(next_data_ptr, &bytes[1], length);
		    next_data_ptr += length;
		}
		break;
	}
    case TAG_GPS_IFD_POINTER: {
        uint32_t offset = *(uint32_t *)tagData;
        write16_le(entry_ptr, tagID);
        write16_le(entry_ptr + 2, TYPE_LONG);
        write32_le(entry_ptr + 4, 1);
        write32_le(entry_ptr + 8, offset);
        break;
    }
    }	// switch
}

/**
 * Get the size of the GPS IFD -
 *
 * If the altitude is present it is 78 bytes
 */
static size_t get_gps_ifd_size(void) {
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
static void create_gps_ifd(uint8_t *gps_ifd_start) {
    uint8_t *p = gps_ifd_start;

    write16_le(p, GPS_IFD_ENTRY_COUNT); p += 2;

    uint8_t *ifd = p;
    p += GPS_IFD_ENTRY_COUNT * 12;
    write32_le(p, 0); p += 4;	// write terminating 4 x 0 (indictes the end of the IFDs

    char latRef[] = "N";
    char lonRef[] = "E";
    uint32_t lat[6] = { 37, 1, 48, 1, 3000, 100 }; // 37°48'30.00"
    uint32_t lon[6] = { 122, 1, 25, 1, 1500, 100 }; // 122°25'15.00"
    uint8_t altRef = 0;                          // 0 = above sea level
    uint32_t alt[2] = { 5000, 100 };             // 50.00 meters

    // Now write 6 IFDs
    addIFD(TAG_GPS_LATITUDE_REF,   ifd + 0*12, latRef);
    addIFD(TAG_GPS_LATITUDE,       ifd + 1*12, lat);
    addIFD(TAG_GPS_LONGITUDE_REF,  ifd + 2*12, lonRef);
    addIFD(TAG_GPS_LONGITUDE,      ifd + 3*12, lon);
    addIFD(TAG_GPS_ALTITUDE_REF,   ifd + 4*12, &altRef);
    addIFD(TAG_GPS_ALTITUDE,       ifd + 5*12, alt);
}


/**
 * Builds a valid EXIF data structure, including APP1 tag
 *
 * This particular example has hard-coded tags to add, including the X&Y resilutions.
 *
 */
static uint16_t build_exif_segment(int8_t * outCategories, uint8_t categoriesCount) {
    char timestamp[EXIFSTRINGLENGTH] = {0}; //22:20:36 2025:07:06 = 19 characters plus trailing \0
    uint16_t exif_len;

    uint8_t nnData[CATEGORIESCOUNT + 2];

    // Insert the NN data
    if (categoriesCount > CATEGORIESCOUNT) {
    	// error
    	nnData[0] = 1;
    	nnData[1] = 0;
    }
    else {
    	// First byte is the number of bytes following
    	nnData[0] = categoriesCount + 1;
    	// Second byte is the number of categories
    	nnData[1] = categoriesCount;
    	// Subsequent bytes are the NN outputs
    	for (uint8_t i=0; i < categoriesCount; i++) {
    		nnData[i + 2] = outCategories[i];
    	}
    }

    // Prepare the timestamp
    exif_utc_get_rtc_as_exif_string(timestamp, sizeof(timestamp));

    // Prepare the resolution entries
    uint32_t xres_rational[2] = { app_get_raw_width(), 1 };
    uint32_t yres_rational[2] = { app_get_raw_height(), 1 };
    uint16_t res_unit = 2;		// TODO check this

    uint8_t *p = exif_buffer;

    // Add APP1 marker
    *p++ = 0xFF;
    *p++ = 0xE1;

    // Write placeholder for segment length
    uint8_t *len_ptr = p;
    p += 2;

    // "Exif\0\0"
    memcpy(p, "Exif\0\0", 6); p += 6;

    // TIFF header: Intel (II), magic 0x002A, IFD0 offset = 8
    tiff_start = p;
    memcpy(p, "II", 2); p += 2;
    write16_le(p, 0x002A);  p += 2;
    write32_le(p, 0x00000008); p += 4;

    // The number of IFD0 entries goes here
    write16_le(p, IFD0_ENTRY_COUNT); p += 2;

    // Keep a note of this location, which is where the IFD entries start
    uint8_t *ifd_start = p;

    p += IFD0_ENTRY_COUNT * 12;  // Skip past the IFD entries

    // Next IFD offset (0)
    // This is writing the offset to the next IFD (Image File Directory) after IFD0.
    // EXIF's TIFF format can chain multiple IFDs.
    // After the IFD0 entry list, there's a 4-byte field indicating the offset (from the TIFF header start) of the next IFD (like IFD1 or the EXIF SubIFD).
    // If there is no next IFD, it must be 0.
    write32_le(p, 0); p += 4;

    // Set pointer to first data location (after IFD)
    next_data_ptr = p;

    //xprintf("DEBUG: next_data_ptr was 0x%08x\n", next_data_ptr);

    // Reserve space for the GPS IFD block before writing tag data
    uint8_t *gps_ifd_start = next_data_ptr;
    uint32_t gps_ifd_offset = (uint32_t)(gps_ifd_start - tiff_start);
    next_data_ptr += get_gps_ifd_size();	// add 78 bytes.

    //xprintf("DEBUG: next_data_ptr is now 0x%08x\n", next_data_ptr);

    // Add IFD entries - these must match IFD0_ENTRY_COUNT
    uint8_t entry = 0;
    addIFD(TAG_MAKE, 			  ifd_start + (entry++ * 12), "Wildlife.ai");
    addIFD(TAG_MODEL, 			  ifd_start + (entry++ * 12), "WW500");
    addIFD(TAG_RESOLUTION_UNIT,   ifd_start + (entry++ * 12), &res_unit);
    addIFD(TAG_X_RESOLUTION,      ifd_start + (entry++ * 12), xres_rational);
    addIFD(TAG_Y_RESOLUTION,      ifd_start + (entry++ * 12), yres_rational);
    addIFD(TAG_DATETIME_ORIGINAL, ifd_start + (entry++ * 12), timestamp);
    addIFD(TAG_CREATE_DATE, 	  ifd_start + (entry++ * 12), timestamp);
    addIFD(TAG_NN_DATA, 	  	  ifd_start + (entry++ * 12), nnData);	// Neural network output
    // GPS:
    addIFD(TAG_GPS_IFD_POINTER, 	ifd_start + (entry++ * 12), &gps_ifd_offset);

    // Now write the GPS IFD structure
    create_gps_ifd(gps_ifd_start);

    // Fill in length field (BE!)
    uint16_t len = (next_data_ptr - exif_buffer) - 2; // exclude 0xFFE1 marker
    write16_be(len_ptr, len);

    exif_len = next_data_ptr - exif_buffer;

    return exif_len;
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
    if (xImageTaskQueue == 0) {
        xprintf("Failed to create xImageTaskQueue\n");
        configASSERT(0); // TODO add debug messages?
    }
#ifdef USE_HM0360
    //xprintf("DEBUG: Using HM0360 so no need for captureTimer\n");
#else
    captureTimer = xTimerCreate("CaptureTimer",
            pdMS_TO_TICKS(1000),    // initial dummy period
            pdFALSE,                // one-shot timer
            NULL,                   // timer ID (optional)
			capture_timer_callback);

    if (captureTimer == NULL) {
        xprintf("Failed to create captureTimer\n");
        configASSERT(0); // TODO add debug messages?
    }
#endif // USE_HM0360

    if (xTaskCreate(vImageTask, /*(const char *)*/ "IMAGE",
                    3 * configMINIMAL_STACK_SIZE,
                    NULL, priority,
                    &image_task_id) != pdPASS) {
        xprintf("Failed to create vImageTask\n");
        configASSERT(0); // TODO add debug messages?
    }

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
const char * image_getStateString(void)
{
    return *&imageTaskStateString[image_task_state];
}

/**
 * Returns the name of the most recently written file as a string
 */
const char * image_getLastImageFile(void) {
    return lastImageFileName;
}

// Called by CLI only.
// Temporary until I can make this work through the state machine
// Call back when inactivity is sensed and we want to enter DPD
// TODO this should really be done using the state machine events!
void image_hackInactive(void) {

	XP_LT_GREEN;
	xprintf("Inactive - in image_hackInactive() Remove this!\n");
	XP_WHITE;

	configure_image_sensor(CAMERA_CONFIG_STOP, 0); // run some sensordplib_stop functions then run HM0360_stream_off commands to the HM0360

	sleepNow();
}

bool image_nnDetected(void) {
	return nnPositive;
}


/**
 * Returns whether the camera system is enabled.
 *
 * This is the "getter".
 * There is no explicit "setter" - this is done by sending messages to the image task queue.
 *
 */
bool image_getEnabled(void) {
	if (nnSystemEnabled == 0) {
		return false;
	}
	else {
		return true;
	}
}
