/*
 * image_task.c
 *
 *  Created on: 22 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that owns the HM0360 camera. See image_task.h.
 *
 *  A picture is taken with the same calls that ww500_md makes in configure_image_sensor(CAMERA_CONFIG_RUN):
 *  cisdp_dp_init(), hm0360_md_setMode(MODE_SW_NFRAMES_SLEEP, 1 frame) and cisdp_sensor_start(). The data path
 *  reports the frame in a callback; the JPEG is then in a buffer that cisdp_get_jpginfo() locates.
 *
 *  Asking hm0360_md_setMode() for a sleep time of zero (as md does) selects the longest sleep interval (about 2 s)
 *  and disables the motion detection interrupt. The frame is not taken until the sleep has finished, so the
 *  time to the frame can be up to that long. CAPTURE_TIMEOUT_MS allows for it.
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "WE2_core.h"
#include "hx_drv_iic.h"

#include "cisdp_sensor.h"
#include "hm0360_md.h"
#include "hm0360_regs.h"
#include "sensor_dp_lib.h"

#include "printf_x.h"
#include "xprintf.h"

#include "app_msg.h"
#include "barrier.h"
#include "fatfs_task.h"
#include "image_task.h"
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

#define IMAGE_TASK_QUEUE_LEN			5

// How long to wait for the frame after the capture has been started
#define CAPTURE_TIMEOUT_MS				5000

// The JPEG quantisation table: 4 selects the 4x table, anything else the 10x table (the md default)
#define JPEG_RATIO						10

// The number of frames taken by one capture
#define CAPTURE_FRAMES					1

// The resting mode after start-up: mode 2. The HM0360 comment in hm0360_md.c gives about 270 uA for it.
#define DEFAULT_REST_MODE				MODE_SW_NFRAMES_SLEEP

// The pictures in one boot are numbered 0 to MAX_PICTURES_PER_BOOT - 1 (two digits in the file name)
#define MAX_PICTURES_PER_BOOT			100

// The boot count in the file name has this many digits, so it wraps at 10^BOOT_COUNT_DIGITS
#define BOOT_COUNT_MODULUS				100000

// The addresses of the PCA9574 I2C expander (as in ww500_md/pca9574.h). It shares the sensor I2C bus, so it shows
// whether the bus works when the HM0360 does not answer.
#define PCA9574_I2C_ADDRESS_0			0x20
#define PCA9574_I2C_ADDRESS_1			0x21

// The highest mode number the HM0360 has (mode 5 is not defined)
#define HM0360_MAX_MODE					7

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

extern Barrier_t startupBarrier;	// Object that calls a function when all tasks are ready
extern Barrier_t shutdownBarrier;	// Object that calls a function when all tasks are ready to shut down

/********************************************** Local Variables **********************************************/

// The queue that other tasks (and the data path callback) use to send messages to this task
static QueueHandle_t xImageTaskQueue;

// This is the handle of the task
static TaskHandle_t image_task_id;

static volatile IMAGE_TASK_STATE_E imageState = IMAGE_TASK_STATE_UNINIT;

// The mode the sensor is put in when it is not taking a picture
static mode_select_t restMode = DEFAULT_REST_MODE;

// True if the inactivity message arrived while a picture was being taken or written
static bool inactivityPending = false;

// The number of pictures saved in this boot, which is also the number of the next picture
static uint8_t picturesSaved = 0;

// When the capture was started, and when the write was started
static TickType_t captureStartTime;
static TickType_t writeStartTime;

// The HM0360 frame counter when the capture was started (valid if frameCountKnown)
static uint16_t frameCountAtStart;
static bool frameCountKnown = false;

// The time from starting the capture to the frame arriving
static uint32_t frameTimeMs;

// The file operation for the JPEG. Only one picture is in progress at a time.
static fileOperation_t jpegFileOp;
static char jpegFileName[FATFS_TASK_FILENAME_LENGTH];

// Strings for each of the states. Values must match IMAGE_TASK_STATE_E in image_task.h
static const char * imageStateString[IMAGE_TASK_NUMSTATES] = {
		"Uninitialised",
		"No camera",
		"Idle",
		"Capturing",
		"Writing",
};

/**************************************** Local Function Declarations ****************************************/

static void vImageTask(void *pvParameters);
static void dataPathCallback(SENSORDPLIB_STATUS_E event);
static bool initCamera(bool coldBoot);
static bool checkExpander(void);
static void applyRestMode(void);
static void startCapture(void);
static void abortCapture(const char *reason);
static const char * dataPathEventName(int32_t event);
static bool readFrameCount(uint16_t *frameCount);
static void handleFrameReady(void);
static void handleFileDone(fileOperation_t *fileOp);
static void handleSetMode(uint8_t mode);
static void finishOperation(void);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Data path callback, called from an interrupt.
 *
 * Tells the task when the frame is ready. Every other event is an error, and is passed on with its number.
 *
 * @param event The data path event.
 */
static void dataPathCallback(SENSORDPLIB_STATUS_E event) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	APP_MSG_T sendMsg;

	sendMsg.msg_data = (uint32_t) event;
	sendMsg.msg_parameter = 0;

	if (event == SENSORDPLIB_STATUS_XDMA_FRAME_READY) {
		sendMsg.msg_event = APP_MSG_IMAGETASK_FRAME_READY;
	}
	else {
		sendMsg.msg_event = APP_MSG_IMAGETASK_FRAME_ERROR;
	}

	xQueueSendFromISR(xImageTaskQueue, &sendMsg, &xHigherPriorityTaskWoken);

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * @brief Describes a data path event.
 *
 * The events are the SENSORDPLIB_STATUS_E values (negative numbers are errors). The commonest are named; the rest are
 * described by their group.
 *
 * @param event The event, as a signed number.
 * @return A pointer to a constant string.
 */
static const char * dataPathEventName(int32_t event) {
	switch (event) {
	case SENSORDPLIB_STATUS_EDM_WDT1_TIMEOUT:
		return "EDM WDT1 timeout: no data at all from the sensor";
	case SENSORDPLIB_STATUS_EDM_WDT2_TIMEOUT:
		return "EDM WDT2 timeout: no frame data from the sensor (no valid VSYNC/HSYNC/PCLK data, or the sensor is not streaming)";
	case SENSORDPLIB_STATUS_EDM_WDT3_TIMEOUT:
		return "EDM WDT3 timeout: the frame data stopped part way";
	case SENSORDPLIB_STATUS_SENSORCTRL_WDT_OUT:
		return "sensor control timeout";
	case SENSORDPLIB_STATUS_ERR_FS_ERR:
	case SENSORDPLIB_STATUS_ERR_HSIZE_ERR:
	case SENSORDPLIB_STATUS_ERR_FE_ERR:
	case SENSORDPLIB_STATUS_ERR_CRC_ERR:
	case SENSORDPLIB_STATUS_ERR_BLANK_ERR:
	case SENSORDPLIB_STATUS_ERR_FS_TOGGLE:
	case SENSORDPLIB_STATUS_ERR_FD_TOGGLE:
	case SENSORDPLIB_STATUS_ERR_FE_TOGGLE:
	case SENSORDPLIB_STATUS_ERR_FS_HVSIZE:
		return "input parser error: the frame from the sensor is malformed";
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL1:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL2:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL3:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL4:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL5:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL6:
	case SENSORDPLIB_STATUS_XDMA_WDMA2_ABNORMAL7:
		return "xDMA WDMA2 (JPEG output) error";
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL1:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL2:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL3:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL4:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL5:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL6:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL7:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL8:
	case SENSORDPLIB_STATUS_XDMA_WDMA3_ABNORMAL9:
		return "xDMA WDMA3 (raw image output) error";
	default:
		if ((event <= SENSORDPLIB_STATUS_VSYNC) && (event >= SENSORDPLIB_STATUS_CONV_DE_MORE)) {
			return "EDM sync or data timing error on the sensor interface";
		}
		return "other data path event";
	}
}

/**
 * @brief Reads the HM0360 frame counter, which counts the frames the sensor has output.
 *
 * @param frameCount Pointer to receive the count.
 * @return true if the registers could be read.
 */
static bool readFrameCount(uint16_t *frameCount) {
	uint8_t high = 0;
	uint8_t low = 0;

	if ((hx_drv_cis_get_reg(FRAME_COUNT_H, &high) != HX_CIS_NO_ERROR) ||
			(hx_drv_cis_get_reg(FRAME_COUNT_L, &low) != HX_CIS_NO_ERROR)) {
		return false;
	}

	*frameCount = (uint16_t) ((high << 8) | low);
	return true;
}

/**
 * @brief Puts the sensor in its resting mode.
 *
 * A sleep time of zero is what md uses to mean "no motion detection": the longest sleep interval, and the
 * motion detection interrupt disabled.
 */
static void applyRestMode(void) {
	HX_CIS_ERROR_E ret;

	ret = hm0360_md_setMode(CONTEXT_A, restMode, CAPTURE_FRAMES, 0);

	if (ret != HX_CIS_NO_ERROR) {
		XP_RED;
		xprintf("Image: could not set HM0360 mode %d, error %d\n", (int) restMode, (int) ret);
		XP_WHITE;
	}
}

/**
 * @brief Checks whether the PCA9574 I2C expander answers.
 *
 * The expander is on the same I2C bus as the HM0360, so an answer proves that the I2C master, its clock, its pins
 * and the pull-ups work. If the expander answers and the HM0360 does not, the fault is at the HM0360 (its
 * supplies, XSLEEP, clock select or address), not in the bus.
 *
 * @return true if the expander answered.
 */
static bool checkExpander(void) {
	static const uint8_t addresses[] = { PCA9574_I2C_ADDRESS_0, PCA9574_I2C_ADDRESS_1 };
	uint8_t i;

	for (i = 0; i < (sizeof(addresses) / sizeof(addresses[0])); i++) {
		if (hm0360_md_isSensorPresent(addresses[i])) {
			XP_LT_GREEN;
			xprintf("Image: PCA9574 I2C expander answers at 0x%02x, so the sensor I2C bus works\n", addresses[i]);
			XP_WHITE;
			return true;
		}
	}

	XP_YELLOW;
	xprintf("Image: no PCA9574 I2C expander at 0x%02x or 0x%02x. Not fitted, or the sensor I2C bus is not working\n",
			PCA9574_I2C_ADDRESS_0, PCA9574_I2C_ADDRESS_1);
	XP_WHITE;
	return false;
}

/**
 * @brief Initialises the camera and the data path.
 *
 * Checks that the HM0360 is there, and (after a cold boot) writes the long register table. After a warm boot
 * the sensor should still hold its registers, so the mode it was in when DPD was entered is printed first: it
 * is what shows that the sensor kept its state.
 *
 * @param coldBoot true after a cold boot, when the registers must be written.
 * @return true if the camera is ready for use.
 */
static bool initCamera(bool coldBoot) {
	mode_select_t modeFound;
	bool expanderOk;
	TickType_t startTime = xTaskGetTickCount();

	// After the HM0360 has entered its motion detection mode the I2C master must be at 100 kHz (Himax).
	if (hx_drv_i2cm_init(USE_DW_IIC_1, HX_I2C_HOST_MST_1_BASE, DW_IIC_SPEED_STANDARD) != IIC_ERR_OK) {
		XP_RED;
		xprintf("Image: the sensor I2C master did not initialise\n");
		XP_WHITE;
		return false;
	}

	expanderOk = checkExpander();

	if (!hm0360_md_isSensorPresent(HM0360_SENSOR_I2CID)) {
		XP_YELLOW;
		xprintf("No HM0360 at I2C address 0x%02x. Carrying on without it\n", HM0360_SENSOR_I2CID);
		if (expanderOk) {
			xprintf("The bus works (the expander answered), so check the HM0360 supplies, XSLEEP and clock select\n");
		}
		XP_WHITE;
		return false;
	}

	hm0360_md_setIsMainCamera(true);

	if (!coldBoot) {
		if (hm0360_md_getMode(&modeFound) == HX_CIS_NO_ERROR) {
			xprintf("Image: HM0360 was in mode %d when the boot began\n", (int) modeFound);
		}
	}

	if (cisdp_sensor_init(coldBoot) != 0) {
		XP_RED;
		xprintf("Image: HM0360 initialisation failed\n");
		XP_WHITE;
		return false;
	}

	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dataPathCallback, JPEG_RATIO,
			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		XP_RED;
		xprintf("Image: data path initialisation failed\n");
		XP_WHITE;
		return false;
	}

	applyRestMode();

	XP_LT_GREEN;
	xprintf("Image: HM0360 ready in mode %d (%s init, %d ms)\n", (int) restMode, coldBoot ? "cold" : "warm",
			(int) ww500_minimal_getElapsedMs(startTime));
	XP_WHITE;

	return true;
}

/**
 * @brief Called when a picture, or an attempt at one, has ended: goes back to idle and deals with any inactivity
 * message that arrived meanwhile.
 */
static void finishOperation(void) {
	imageState = IMAGE_TASK_STATE_IDLE;

	if (inactivityPending) {
		inactivityPending = false;
		barrier_ready(&shutdownBarrier);
	}
}

/**
 * @brief Starts taking a picture, if that is possible.
 */
static void startCapture(void) {
	if (imageState == IMAGE_TASK_STATE_NO_CAMERA) {
		xprintf("Image: no camera\n");
		return;
	}
	if (imageState != IMAGE_TASK_STATE_IDLE) {
		xprintf("Image: busy (%s)\n", imageStateString[imageState]);
		return;
	}
	if (!fatfs_task_mounted()) {
		xprintf("Image: no SD card, so no picture\n");
		return;
	}
	if (!fatfs_task_bootCountValid()) {
		xprintf("Image: the boot count was not updated, so no picture\n");
		return;
	}
	if (picturesSaved >= MAX_PICTURES_PER_BOOT) {
		xprintf("Image: %d pictures have been saved in this boot: the limit\n", MAX_PICTURES_PER_BOOT);
		return;
	}

	// As md does before each picture: the data path is set up again, then the sensor starts, then the data path
	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dataPathCallback, JPEG_RATIO,
			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		XP_RED;
		xprintf("Image: data path initialisation failed\n");
		XP_WHITE;
		return;
	}

	if (hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, CAPTURE_FRAMES, 0) != HX_CIS_NO_ERROR) {
		XP_RED;
		xprintf("Image: could not start the HM0360\n");
		XP_WHITE;
		return;
	}

	frameCountKnown = readFrameCount(&frameCountAtStart);

	captureStartTime = xTaskGetTickCount();
	imageState = IMAGE_TASK_STATE_CAPTURING;
	xprintf("Image: capturing...\n");

	cisdp_sensor_start();
}

/**
 * @brief Stops a picture that has failed, puts the sensor back in its resting mode and goes back to idle.
 *
 * @param reason Text for the console.
 */
static void abortCapture(const char *reason) {
	uint16_t frameCountNow;

	XP_RED;
	xprintf("Image: capture failed: %s (after %d ms)\n", reason, (int) ww500_minimal_getElapsedMs(captureStartTime));
	XP_WHITE;

	// Did the sensor output a frame? If its counter did not move it never streamed (mode, power or reset problem);
	// if it did move the frame left the sensor but did not reach the data path (the parallel data connections).
	if (frameCountKnown && readFrameCount(&frameCountNow)) {
		xprintf("Image: the HM0360 frame counter was %u at the start and is %u now: the sensor %s\n",
				(unsigned) frameCountAtStart, (unsigned) frameCountNow,
				(frameCountNow != frameCountAtStart) ? "did output frames, so check the data, PCLK, VSYNC and HSYNC connections" :
						"did not output any frame");
	}

	cisdp_sensor_stop();	// Stops the data path, and puts the HM0360 in MODE_SLEEP
	applyRestMode();
	finishOperation();
}

/**
 * @brief The frame has arrived: finds the JPEG and asks the FatFS task to write it.
 */
static void handleFrameReady(void) {
	uint32_t jpegLength = 0;
	uint32_t jpegAddress = 0;

	if (imageState != IMAGE_TASK_STATE_CAPTURING) {
		xprintf("Image: a frame arrived when none was expected\n");
		return;
	}

	frameTimeMs = ww500_minimal_getElapsedMs(captureStartTime);

	cisdp_get_jpginfo(&jpegLength, &jpegAddress);

	// The JPEG was written by DMA, so the data cache must not be trusted
	SCB_InvalidateDCache_by_Addr((void *) jpegAddress, jpegLength);

	// The data path is stopped, and the sensor put back in its resting mode, before the write begins
	cisdp_sensor_stop();
	applyRestMode();

	if ((jpegLength == 0) || (jpegAddress == 0)) {
		abortCapture("the JPEG is empty");
		return;
	}

	// Bnnnnnnn.JPG : 5 digits of boot count then 2 digits of picture number
	snprintf(jpegFileName, sizeof(jpegFileName), "B%05lu%02u.JPG",
			(unsigned long) (fatfs_task_getBootCount() % BOOT_COUNT_MODULUS), (unsigned) picturesSaved);

	jpegFileOp.fileName = jpegFileName;
	jpegFileOp.buffer = (uint8_t *) jpegAddress;
	jpegFileOp.length = jpegLength;
	jpegFileOp.doneEvent = APP_MSG_IMAGETASK_FILE_DONE;
	jpegFileOp.senderQueue = xImageTaskQueue;

	xprintf("Image: frame after %d ms, JPEG is %u bytes. Writing '%s'\n", (int) frameTimeMs,
			(unsigned) jpegLength, jpegFileName);

	writeStartTime = xTaskGetTickCount();
	imageState = IMAGE_TASK_STATE_WRITING;

	if (!fatfs_task_sendFileOp(APP_MSG_FATFSTASK_WRITE_FILE, &jpegFileOp)) {
		abortCapture("the FatFS task did not accept the file");
	}
}

/**
 * @brief The FatFS task has finished writing the JPEG: reports the result.
 *
 * @param fileOp The file operation, which is jpegFileOp.
 */
static void handleFileDone(fileOperation_t *fileOp) {
	if (fileOp->res == FR_OK) {
		picturesSaved++;
		XP_LT_GREEN;
		xprintf("Image: saved '%s', %u bytes (frame %d ms, write %d ms)\n", fileOp->fileName,
				(unsigned) fileOp->length, (int) frameTimeMs, (int) ww500_minimal_getElapsedMs(writeStartTime));
		XP_WHITE;
	}
	else {
		XP_RED;
		xprintf("Image: writing '%s' failed: FatFS error %d\n", fileOp->fileName, (int) fileOp->res);
		XP_WHITE;
	}

	finishOperation();
}

/**
 * @brief Sets the resting mode, or prints the mode.
 *
 * @param mode The HM0360 mode (0 to 7, but not 5), or IMAGE_TASK_MODE_REPORT.
 */
static void handleSetMode(uint8_t mode) {
	mode_select_t modeNow;
	uint8_t idHigh = 0;
	uint8_t idLow = 0;
	uint16_t frameCountNow;

	if (imageState == IMAGE_TASK_STATE_NO_CAMERA) {
		xprintf("Image: no camera\n");
		return;
	}
	if (imageState != IMAGE_TASK_STATE_IDLE) {
		xprintf("Image: busy (%s)\n", imageStateString[imageState]);
		return;
	}

	if (mode == IMAGE_TASK_MODE_REPORT) {
		if (hm0360_md_getMode(&modeNow) == HX_CIS_NO_ERROR) {
			xprintf("Image: HM0360 is in mode %d, the resting mode is %d, %d picture(s) saved in this boot\n",
					(int) modeNow, (int) restMode, (int) picturesSaved);

			// The model ID (0x0360 expected) shows that register reads are sound. The frame counter shows whether the
			// sensor is streaming: run 'cam' twice, a few seconds apart, in mode 1 (continuous) and see if it moves.
			// It is 0xFFFF until the sensor has output a frame.
			if (hx_drv_cis_get_reg(MODEL_ID_H, &idHigh) == HX_CIS_NO_ERROR &&
					hx_drv_cis_get_reg(MODEL_ID_L, &idLow) == HX_CIS_NO_ERROR) {
				xprintf("Image: HM0360 model ID 0x%02x%02x", (unsigned) idHigh, (unsigned) idLow);
			}
			if (readFrameCount(&frameCountNow)) {
				xprintf(", frame counter %u\n", (unsigned) frameCountNow);
			}
			else {
				xprintf(", frame counter could not be read\n");
			}
		}
		else {
			xprintf("Image: could not read the HM0360 mode\n");
		}
	}
	else if ((mode > HM0360_MAX_MODE) || (mode == MODE_RFU)) {
		xprintf("Image: mode %d is not one of the HM0360 modes (0-4, 6, 7)\n", (int) mode);
	}
	else {
		restMode = (mode_select_t) mode;
		applyRestMode();
		xprintf("Image: HM0360 set to mode %d\n", (int) restMode);
	}
}

/**
 * @brief The FreeRTOS task.
 *
 * Initialises the camera, then reports that it is ready and serves requests.
 *
 * @param pvParameters The reason for the wakeup, as a WW500_MINIMAL_WAKE_REASON_E.
 */
static void vImageTask(void *pvParameters) {
	APP_MSG_T rxMessage;
	TickType_t waitTicks;
	TickType_t elapsedTicks;
	// Any boot that is not clearly a wakeup from DPD gets the full register table
	bool coldBoot = ((WW500_MINIMAL_WAKE_REASON_E) (uint32_t) pvParameters != WW500_MINIMAL_WAKE_REASON_WAKE_PIN)
			&& ((WW500_MINIMAL_WAKE_REASON_E) (uint32_t) pvParameters != WW500_MINIMAL_WAKE_REASON_TIMER);

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting Image Task\n");
	XP_WHITE;

	if (initCamera(coldBoot)) {
		imageState = IMAGE_TASK_STATE_IDLE;
	}
	else {
		imageState = IMAGE_TASK_STATE_NO_CAMERA;
	}

	barrier_ready(&startupBarrier);		// Call a function when every task reaches this point

	for (;;) {
		// While a picture is being taken the wait for a message is limited, so that a frame that never comes is noticed
		waitTicks = portMAX_DELAY;
		if (imageState == IMAGE_TASK_STATE_CAPTURING) {
			elapsedTicks = xTaskGetTickCount() - captureStartTime;
			waitTicks = (elapsedTicks < pdMS_TO_TICKS(CAPTURE_TIMEOUT_MS)) ? (pdMS_TO_TICKS(CAPTURE_TIMEOUT_MS) - elapsedTicks) : 0;
		}

		if (xQueueReceive(xImageTaskQueue, &rxMessage, waitTicks) == pdTRUE) {

			switch (rxMessage.msg_event) {

			case APP_MSG_IMAGETASK_CAPTURE:
				startCapture();
				break;

			case APP_MSG_IMAGETASK_FRAME_READY:
				handleFrameReady();
				break;

			case APP_MSG_IMAGETASK_FRAME_ERROR:
				if (imageState == IMAGE_TASK_STATE_CAPTURING) {
					xprintf("Image: data path event %d: %s\n", (int) (int32_t) rxMessage.msg_data,
							dataPathEventName((int32_t) rxMessage.msg_data));
					abortCapture("data path error");
				}
				break;

			case APP_MSG_IMAGETASK_FILE_DONE:
				handleFileDone((fileOperation_t *) rxMessage.msg_data);
				break;

			case APP_MSG_IMAGETASK_SET_MODE:
				handleSetMode((uint8_t) rxMessage.msg_data);
				break;

			case APP_MSG_IMAGETASK_REINIT:
				if (imageState == IMAGE_TASK_STATE_IDLE || imageState == IMAGE_TASK_STATE_NO_CAMERA) {
					imageState = initCamera(true) ? IMAGE_TASK_STATE_IDLE : IMAGE_TASK_STATE_NO_CAMERA;
				}
				else {
					xprintf("Image: busy (%s)\n", imageStateString[imageState]);
				}
				break;

			case APP_MSG_IMAGETASK_INACTIVITY:
				if ((imageState == IMAGE_TASK_STATE_CAPTURING) || (imageState == IMAGE_TASK_STATE_WRITING)) {
					// Wait for the picture to finish. finishOperation() reports to the barrier.
					inactivityPending = true;
				}
				else {
					// The sensor is already in its resting mode, which it keeps through DPD (its supply stays on)
					xprintf("Image: HM0360 left in mode %d for DPD\n", (int) restMode);
					barrier_ready(&shutdownBarrier);
				}
				break;

			default:
				xprintf("Image Task: unexpected event 0x%04x\n", rxMessage.msg_event);
				break;
			}
		}
		else if (imageState == IMAGE_TASK_STATE_CAPTURING) {
			abortCapture("timed out waiting for the frame");
		}
	}
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 *
 * @param priority   The FreeRTOS priority for the task.
 * @param wakeReason The reason for this wakeup: after a cold boot the HM0360 registers are written.
 * @return The handle of the task.
 */
TaskHandle_t image_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason) {
	if (priority < 0) {
		priority = 0;
	}

	xImageTaskQueue = xQueueCreate(IMAGE_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xImageTaskQueue == 0) {
		xprintf("Failed to create xImageTaskQueue\n");
		configASSERT(0);
	}

	if (xTaskCreate(vImageTask, (const char *)"Image",
			configMINIMAL_STACK_SIZE * 6,
			(void *) (uint32_t) wakeReason, priority,
			&image_task_id) != pdPASS) {
		xprintf("Failed to create vImageTask\n");
		configASSERT(0);
	}

	return image_task_id;
}

/**
 * @brief Returns the internal state as a number.
 *
 * @return The state, an IMAGE_TASK_STATE_E value.
 */
uint16_t image_task_getState(void) {
	return imageState;
}

/**
 * @brief Returns the internal state as a string.
 *
 * @return A pointer to a constant string.
 */
const char * image_task_getStateString(void) {
	return imageStateString[imageState];
}

/**
 * @brief Returns true while a picture is being taken or written.
 *
 * @return true if the task is capturing or writing.
 */
bool image_task_isBusy(void) {
	return (imageState == IMAGE_TASK_STATE_CAPTURING) || (imageState == IMAGE_TASK_STATE_WRITING);
}

/**
 * @brief Asks the task to take a picture and save it. The result is printed when it is known.
 *
 * @return true if the request was queued.
 */
bool image_task_requestCapture(void) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_CAPTURE;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Asks the task to set the resting mode of the HM0360, or to print the mode.
 *
 * @param mode The HM0360 mode (0 to 7, but not 5), or IMAGE_TASK_MODE_REPORT to print it.
 * @return true if the request was queued.
 */
bool image_task_requestMode(uint8_t mode) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_SET_MODE;
	sendMsg.msg_data = mode;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Asks the task to write the HM0360 register table again, as after a cold boot.
 *
 * @return true if the request was queued.
 */
bool image_task_requestReinit(void) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_REINIT;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Tells the task that all tasks are inactive, so it should finish what it is doing and get ready for DPD.
 *
 * Called from the FreeRTOS idle hook (see inactivity.c) so it must not block: the message is dropped if the
 * queue is full.
 */
void image_task_notifyInactivity(void) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_INACTIVITY;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	xQueueSend(xImageTaskQueue, (void *) &sendMsg, 0);
}
