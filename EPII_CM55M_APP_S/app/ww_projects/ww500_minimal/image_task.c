/*
 * image_task.c
 *
 *  Created on: 22 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that owns the camera: the HM0360 (USE_HM0360) or the RP3, IMX708 (USE_RP3), one per build. See
 *  image_task.h.
 *
 *  A picture is taken with the same calls that ww500_md makes in configure_image_sensor(): cisdp_dp_init(), then
 *  the sensor is started with cisdp_sensor_start(). The data path reports the frame in a callback; the JPEG is then
 *  in a buffer that cisdp_get_jpginfo() locates. The flow (capture, JPEG, save, shutdown barrier) is shared; the
 *  camera-specific parts are the functions in the USE_HM0360 and USE_RP3 blocks below.
 *
 *  HM0360: before the start, hm0360_md_setMode(MODE_SW_NFRAMES_SLEEP, 1 frame). Asking hm0360_md_setMode() for a
 *  sleep time of zero (as md does) selects the longest sleep interval (about 2 s) and disables the motion detection
 *  interrupt. The first frame does not wait for the sleep interval. CAPTURE_TIMEOUT_MS allows for it anyway. The
 *  sensor keeps its registers through DPD, and between pictures it stays in its resting mode.
 *
 *  RP3: the camera is powered by SENSOR_ENABLE (PB7) only while it is in use, as ww500_md does across DPD, so it
 *  draws nothing between pictures. Each picture powers it up and writes all its registers (cisdp_sensor_init()),
 *  then streams until the first frame arrives, then stops and powers it down. 'cam on' keeps it powered (and
 *  initialised, not streaming) so that its standing current can be measured. There is no auto-exposure and no
 *  white balance (ww500_md's ae.c and img_correct), so the exposure is the fixed one in the register tables and
 *  the picture will be green.
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
#ifdef USE_RP3
#include "pinmux_cfg.h"
#endif // USE_RP3
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

#if defined(USE_HM0360) == defined(USE_RP3)
#error "Build for exactly one camera: USE_HM0360 or USE_RP3 (set by CIS_SUPPORT_INAPP_MODEL in ww500_minimal.mk)"
#endif

#define IMAGE_TASK_QUEUE_LEN			5

// How long to wait for the frame after the capture has been started
#define CAPTURE_TIMEOUT_MS				5000

// The JPEG quantisation table: 4 selects the 4x table, anything else the 10x table (the md default)
#define JPEG_RATIO						10

// The pictures in one boot are numbered 0 to MAX_PICTURES_PER_BOOT - 1 (two digits in the file name)
#define MAX_PICTURES_PER_BOOT			100

// The boot count in the file name has this many digits, so it wraps at 10^BOOT_COUNT_DIGITS
#define BOOT_COUNT_MODULUS				100000

// The addresses of the PCA9574 I2C expander (as in ww500_md/pca9574.h). It shares the sensor I2C bus, so it shows
// whether the bus works when the camera does not answer.
#define PCA9574_I2C_ADDRESS_0			0x20
#define PCA9574_I2C_ADDRESS_1			0x21

// 1 to light the red LED from the start of streaming (cisdp_sensor_start()) until the data path reports the frame (or an
// error), so the capture time can be seen on the PPK2 or a scope (28 Sep 2026). Use 'blink off' first, or the blinky
// task will also drive the LED. The LED current adds to the measured current while it is lit.
#define CAPTURE_TIME_ON_RED_LED			1

#ifdef USE_HM0360
#define CAMERA_NAME						"HM0360"

// The sensor I2C bus speed: after the HM0360 has entered its motion detection mode the I2C master must be at
// 100 kHz (Himax), as in ww500_md
#define SENSOR_I2C_SPEED				DW_IIC_SPEED_STANDARD

// The number of frames taken by one capture
#define CAPTURE_FRAMES					1

// The resting mode after start-up: mode 2. The HM0360 comment in hm0360_md.c gives about 270 uA for it.
#define DEFAULT_REST_MODE				MODE_SW_NFRAMES_SLEEP

// The register context of the resting mode after a cold boot. ww500_md uses CONTEXT_B for motion detection.
#define DEFAULT_REST_CONTEXT			CONTEXT_A

// The motion detection interval after a cold boot, in ms. 0 = off (motion detection interrupt disabled).
#define DEFAULT_MD_INTERVAL_MS			0

// The HM0360 sleep count for one second, as used by calculateSleepTime() in hm0360_md.c
#define MD_SLEEP_COUNT_PER_S			0x8030

// The longest interval the 16-bit sleep count can hold: 0xFFFF * 1000 / MD_SLEEP_COUNT_PER_S
#define MD_MAX_INTERVAL_MS				1997

// 1 to print the HM0360 registers that decide what it does in DPD, just before DPD (see printSensorRegisters()).
// Used while measuring the context B timing (power_investigation.md, parked 26 Sep 2026).
#define PRINT_REGISTERS_BEFORE_DPD		0

// The highest mode number the HM0360 has (mode 5 is not defined)
#define HM0360_MAX_MODE					7
#endif // USE_HM0360

#ifdef USE_RP3
#define CAMERA_NAME						"RP3 (IMX708)"

// The sensor I2C bus speed. The HM0360's 100 kHz limit does not apply to the RP3 build. At 100 kHz the ~255 register
// writes in cisdp_sensor_init() took 111 ms (28 Sep 2026); at 400 kHz they should take about a quarter of that.
// DW_IIC_SPEED_STANDARD (100 kHz) to go back.
#define SENSOR_I2C_SPEED				DW_IIC_SPEED_FAST

// The IMX708 model ID registers (the SMIA standard registers). 0x0708 is expected.
#define IMX708_MODEL_ID_H				0x0016
#define IMX708_MODEL_ID_L				0x0017

// The 'cam' values (see image_task_requestMode()) that power the RP3 down and up
#define RP3_CAM_OFF						0
#define RP3_CAM_ON						1
#endif // USE_RP3

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

// True if the inactivity message arrived while a picture was being taken or written
static bool inactivityPending = false;

// True from cisdp_sensor_start() until the sensor has been stopped (see endCapture())
static bool sensorStreaming = false;

// The number of pictures saved in this boot, which is also the number of the next picture
static uint8_t picturesSaved = 0;

// When the capture was started, and when the write was started
static TickType_t captureStartTime;
static TickType_t writeStartTime;

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

#ifdef USE_HM0360
// The mode the sensor is put in when it is not taking a picture
static mode_select_t restMode = DEFAULT_REST_MODE;

// The register context (CONTEXT_A or CONTEXT_B) of the resting mode
static uint8_t restContext = DEFAULT_REST_CONTEXT;

// The motion detection interval of the resting mode, in ms. 0 = motion detection interrupt off.
static uint16_t mdIntervalMs = DEFAULT_MD_INTERVAL_MS;

// The reason for this wakeup: a motion detection interrupt is only expected after a WAKE pin wake
static WW500_MINIMAL_WAKE_REASON_E bootWakeReason = WW500_MINIMAL_WAKE_REASON_UNKNOWN;

// The HM0360 frame counter when the capture was started (valid if frameCountKnown)
static uint16_t frameCountAtStart;
static bool frameCountKnown = false;
#endif // USE_HM0360

#ifdef USE_RP3
// True while SENSOR_ENABLE is high and the registers have been written
static bool rpPowered = false;

// True after 'cam on': the camera stays powered between pictures, until 'cam off' or DPD
static bool rpHeldOn = false;
#endif // USE_RP3

/**************************************** Local Function Declarations ****************************************/

static void vImageTask(void *pvParameters);
static void dataPathCallback(SENSORDPLIB_STATUS_E event);
static const char * dataPathEventName(int32_t event);
static bool checkExpander(void);
static bool initI2cAndExpander(bool *expanderOk);
static bool initCamera(bool coldBoot);
static bool armCapture(void);
static void endCapture(void);
static void prepareSensorForDpd(void);
static void handleSetMode(uint8_t mode);
static void startCapture(void);
static void abortCapture(const char *reason);
static void handleFrameReady(void);
static void handleFileDone(fileOperation_t *fileOp);
static bool readyForChange(void);
static void finishOperation(void);
static void prepareForDpd(void);
#ifdef USE_HM0360
static bool readFrameCount(uint16_t *frameCount);
static void applyRestMode(void);
static void recoverRestSettings(void);
static bool checkMotionInterrupt(const char *when);
static void handleSetContext(uint8_t context);
static void handleSetMdInterval(uint16_t intervalMs);
#if PRINT_REGISTERS_BEFORE_DPD
static void printSensorRegisters(void);
#endif // PRINT_REGISTERS_BEFORE_DPD
#endif // USE_HM0360
#ifdef USE_RP3
static bool rpPowerUp(void);
static void rpPowerDown(void);
#endif // USE_RP3

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

#if CAPTURE_TIME_ON_RED_LED
	ww500_minimal_ledRed(false);	// End of the capture time (a GPIO register write, safe in an interrupt)
#endif // CAPTURE_TIME_ON_RED_LED

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
 * @brief Checks whether the PCA9574 I2C expander answers.
 *
 * The expander is on the same I2C bus as the camera, so an answer proves that the I2C master, its clock, its pins
 * and the pull-ups work. If the expander answers and the camera does not, the fault is at the camera (its
 * supplies, enable, clock or address), not in the bus.
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
 * @brief Initialises the sensor I2C master and checks the bus with the PCA9574 expander.
 *
 * The speed is SENSOR_I2C_SPEED: 100 kHz for the HM0360 (Himax: needed once it is in motion detection mode, as in
 * ww500_md), 400 kHz for the RP3 (28 Sep 2026, to shorten its register writes).
 *
 * @param expanderOk Pointer to receive whether the expander answered.
 * @return true if the I2C master initialised.
 */
static bool initI2cAndExpander(bool *expanderOk) {
	if (hx_drv_i2cm_init(USE_DW_IIC_1, HX_I2C_HOST_MST_1_BASE, SENSOR_I2C_SPEED) != IIC_ERR_OK) {
		XP_RED;
		xprintf("Image: the sensor I2C master did not initialise\n");
		XP_WHITE;
		return false;
	}
	xprintf("Image: sensor I2C at %s\n", (SENSOR_I2C_SPEED == DW_IIC_SPEED_STANDARD) ? "100 kHz" : "400 kHz");

	*expanderOk = checkExpander();
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
		prepareForDpd();
	}
}

/**
 * @brief Gets the camera ready for DPD, then reports to the shutdown barrier.
 */
static void prepareForDpd(void) {
	if (imageState != IMAGE_TASK_STATE_NO_CAMERA) {
		prepareSensorForDpd();
	}
	barrier_ready(&shutdownBarrier);
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
	// Whether the JPEG can be saved (an SD card, mounted, with a valid boot count) is checked once the frame
	// is in hand (see handleFrameReady()), not here: this lets the sensor be tested (and its current measured)
	// with no SD card or no FatFS code (WW500_MINIMAL_NO_FATFS) fitted, at the cost of the frame being taken
	// and then discarded when it cannot be saved.
	if (picturesSaved >= MAX_PICTURES_PER_BOOT) {
		xprintf("Image: %d pictures have been saved in this boot: the limit\n", MAX_PICTURES_PER_BOOT);
		return;
	}

	if (!armCapture()) {
		return;
	}

	// The frame time is measured from here, as before the RP3 was added: it does not include the RP3 power-up
	captureStartTime = xTaskGetTickCount();
	imageState = IMAGE_TASK_STATE_CAPTURING;
	sensorStreaming = true;
	xprintf("Image: capturing...\n");

#if CAPTURE_TIME_ON_RED_LED
	ww500_minimal_ledRed(true);		// Start of the capture time: ends in dataPathCallback()
#endif // CAPTURE_TIME_ON_RED_LED
	cisdp_sensor_start();
}

/**
 * @brief Stops a picture that has failed, stops the sensor if it is still running and goes back to idle.
 *
 * @param reason Text for the console.
 */
static void abortCapture(const char *reason) {
#ifdef USE_HM0360
	uint16_t frameCountNow;
#endif // USE_HM0360

#if CAPTURE_TIME_ON_RED_LED
	ww500_minimal_ledRed(false);	// In case the frame never came (timeout), so dataPathCallback() did not switch it off
#endif // CAPTURE_TIME_ON_RED_LED

	XP_RED;
	xprintf("Image: capture failed: %s (after %d ms)\n", reason, (int) ww500_minimal_getElapsedMs(captureStartTime));
	XP_WHITE;

#ifdef USE_HM0360
	// Did the sensor output a frame? If its counter did not move it never streamed (mode, power or reset problem);
	// if it did move the frame left the sensor but did not reach the data path (the parallel data connections).
	if (sensorStreaming && frameCountKnown && readFrameCount(&frameCountNow)) {
		xprintf("Image: the HM0360 frame counter was %u at the start and is %u now: the sensor %s\n",
				(unsigned) frameCountAtStart, (unsigned) frameCountNow,
				(frameCountNow != frameCountAtStart) ? "did output frames, so check the data, PCLK, VSYNC and HSYNC connections" :
						"did not output any frame");
	}
#endif // USE_HM0360

	endCapture();
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

	// The sensor is stopped (and put back in its resting state) before the write begins. The JPEG stays in memory.
	endCapture();

	if ((jpegLength == 0) || (jpegAddress == 0)) {
		abortCapture("the JPEG is empty");
		return;
	}

	// The sensor has been tested regardless of the SD card - only the save needs one. Checked here, not
	// before the capture, so a frame can still be taken (and its size and timing seen) with no card or no
	// FatFS code (WW500_MINIMAL_NO_FATFS) fitted.
	if (!fatfs_task_mounted()) {
		XP_YELLOW;
		xprintf("Image: frame after %d ms, JPEG is %u bytes, but there is no SD card, so it was not saved\n",
				(int) frameTimeMs, (unsigned) jpegLength);
		XP_WHITE;
		finishOperation();
		return;
	}
	if (!fatfs_task_bootCountValid()) {
		XP_YELLOW;
		xprintf("Image: frame after %d ms, JPEG is %u bytes, but the boot count was not updated, so it was not saved\n",
				(int) frameTimeMs, (unsigned) jpegLength);
		XP_WHITE;
		finishOperation();
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
 * @brief Checks that the sensor settings can be changed now: there is a camera and no picture is in progress.
 *
 * @return true if the task is idle.
 */
static bool readyForChange(void) {
	if (imageState == IMAGE_TASK_STATE_NO_CAMERA) {
		xprintf("Image: no camera\n");
		return false;
	}
	if (imageState != IMAGE_TASK_STATE_IDLE) {
		xprintf("Image: busy (%s)\n", imageStateString[imageState]);
		return false;
	}
	return true;
}

#ifdef USE_HM0360
/******************************************** HM0360 camera functions ****************************************/

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
 * @brief Puts the sensor in its resting mode, with its resting context and motion detection interval.
 *
 * An interval of zero is what md uses to mean "no motion detection": the longest sleep interval, and the
 * motion detection interrupt disabled. hm0360_md_setMode() enables the interrupt (MD_CTRL1) for a non-zero
 * interval, but while the processor is awake it is disabled again here, as md does at a warm boot: the sensor
 * keeps taking its motion detection frames, and prepareSensorForDpd() enables the interrupt just before DPD.
 */
static void applyRestMode(void) {
	HX_CIS_ERROR_E ret;

	hm0360_md_clearInterrupt(0xff);

	ret = hm0360_md_setMode(restContext, restMode, CAPTURE_FRAMES, mdIntervalMs);

	if (ret != HX_CIS_NO_ERROR) {
		XP_RED;
		xprintf("Image: could not set HM0360 mode %d, error %d\n", (int) restMode, (int) ret);
		XP_WHITE;
	}

	if (mdIntervalMs > 0) {
		hm0360_md_disableInterrupt();
	}
}

/**
 * @brief After a warm boot, reads the resting settings back from the HM0360, which kept them through DPD.
 *
 * The HX6538 RAM is not kept through DPD, but the sensor's supply is, so the sensor is the record of the mode,
 * context and motion detection interval that were set before DPD. Nothing is written here. The interval is
 * worked back from the sleep count (to within 1 ms); it counts as off if the motion detection interrupt is
 * disabled, as it is only enabled (by prepareSensorForDpd()) when the interval is non-zero.
 */
static void recoverRestSettings(void) {
	mode_select_t modeFound;
	uint8_t context = 0;
	uint8_t mdCtrl1 = 0;
	uint8_t countHigh = 0;
	uint8_t countLow = 0;
	uint32_t sleepCount;

	if ((hm0360_md_getMode(&modeFound) != HX_CIS_NO_ERROR) ||
			(hx_drv_cis_get_reg(PMU_CFG_3, &context) != HX_CIS_NO_ERROR) ||
			(hx_drv_cis_get_reg(MD_CTRL1, &mdCtrl1) != HX_CIS_NO_ERROR) ||
			(hx_drv_cis_get_reg(PMU_CFG_8, &countHigh) != HX_CIS_NO_ERROR) ||
			(hx_drv_cis_get_reg(PMU_CFG_9, &countLow) != HX_CIS_NO_ERROR)) {
		XP_YELLOW;
		xprintf("Image: could not read the HM0360 settings, so the defaults are used\n");
		XP_WHITE;
		return;
	}

	restMode = modeFound;
	restContext = context & CONTEXT_B;

	if (mdCtrl1 == 0) {
		mdIntervalMs = 0;
	}
	else {
		sleepCount = ((uint32_t) countHigh << 8) | countLow;
		mdIntervalMs = (uint16_t) ((sleepCount * 1000 + (MD_SLEEP_COUNT_PER_S / 2)) / MD_SLEEP_COUNT_PER_S);
	}

	xprintf("Image: HM0360 kept mode %d, context %c, motion detection %s (sleep count 0x%02x%02x, MD_CTRL1 0x%02x)\n",
			(int) restMode, (restContext == CONTEXT_B) ? 'B' : 'A', (mdIntervalMs > 0) ? "on" : "off",
			(unsigned) countHigh, (unsigned) countLow, (unsigned) mdCtrl1);
	if (mdIntervalMs > 0) {
		xprintf("Image: motion detection interval %d ms\n", (int) mdIntervalMs);
	}
}

/**
 * @brief Reports and clears the HM0360 motion detection interrupt.
 *
 * While the interrupt is raised the HM0360 holds its INT output, and so the WAKE pin (PA0), high: DPD would end
 * at once. INT_INDIC is read first, then the motion blocks (MD_ROI_OUT), then every interrupt bit is cleared.
 *
 * @param when Text for the console, saying when the check was made.
 * @return true if the motion detection interrupt was raised.
 */
static bool checkMotionInterrupt(const char *when) {
	uint8_t intIndic = 0;
	uint8_t roiOut[ROIOUTENTRIES];
	uint16_t blocks;
	bool motion;

	if (hm0360_md_getInterruptStatus(&intIndic) != HX_CIS_NO_ERROR) {
		xprintf("Image: could not read INT_INDIC %s\n", when);
		return false;
	}

	motion = ((intIndic & MD_INT) != 0);

	if (motion) {
		blocks = hm0360_md_getMDOutput(roiOut, ROIOUTENTRIES);
		XP_LT_GREEN;
		xprintf("Image: motion detected %s: INT_INDIC 0x%02x, motion in %d blocks\n", when, (unsigned) intIndic,
				(int) blocks);
		XP_WHITE;
	}
	else {
		xprintf("Image: no motion interrupt %s: INT_INDIC 0x%02x\n", when, (unsigned) intIndic);
	}

	hm0360_md_clearInterrupt(0xff);

	return motion;
}

/**
 * @brief Initialises the HM0360 and the data path.
 *
 * Checks that the HM0360 is there, and (after a cold boot) writes the long register table. After a warm boot
 * the sensor should still hold its registers, so before anything is written the motion detection interrupt is
 * reported (and cleared), and the resting mode, context and motion detection interval are read back from the
 * sensor. They are then applied again, so the settings made by the CLI survive DPD.
 *
 * @param coldBoot true after a cold boot, when the registers must be written.
 * @return true if the camera is ready for use.
 */
static bool initCamera(bool coldBoot) {
	bool expanderOk;
	bool motion;
	TickType_t startTime = xTaskGetTickCount();

	if (!initI2cAndExpander(&expanderOk)) {
		return false;
	}

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
		// Before anything is written: what woke us, and what the sensor kept through DPD
		hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);
		motion = checkMotionInterrupt("at wake");
		if (bootWakeReason == WW500_MINIMAL_WAKE_REASON_WAKE_PIN) {
			XP_YELLOW;
			xprintf("Image: the WAKE pin woke the processor: %s\n",
					motion ? "motion detection by the HM0360" : "not the HM0360, so something else on PA0");
			XP_WHITE;
		}
		recoverRestSettings();
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
	xprintf("Image: HM0360 ready in mode %d, context %c, MD interval %d ms (%s init, %d ms)\n", (int) restMode,
			(restContext == CONTEXT_B) ? 'B' : 'A', (int) mdIntervalMs, coldBoot ? "cold" : "warm",
			(int) ww500_minimal_getElapsedMs(startTime));
	XP_WHITE;

	return true;
}

/**
 * @brief Gets the HM0360 ready to stream one picture: the data path, then mode 2 for one frame in context A.
 *
 * As md does before each picture: the data path is set up again, then the sensor mode is set.
 *
 * @return true if the capture can be started.
 */
static bool armCapture(void) {
	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dataPathCallback, JPEG_RATIO,
			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		XP_RED;
		xprintf("Image: data path initialisation failed\n");
		XP_WHITE;
		return false;
	}

	if (hm0360_md_setMode(CONTEXT_A, MODE_SW_NFRAMES_SLEEP, CAPTURE_FRAMES, 0) != HX_CIS_NO_ERROR) {
		XP_RED;
		xprintf("Image: could not start the HM0360\n");
		XP_WHITE;
		return false;
	}

	frameCountKnown = readFrameCount(&frameCountAtStart);

	return true;
}

/**
 * @brief Stops the data path and puts the HM0360 back in its resting mode. Does nothing if it is not streaming.
 */
static void endCapture(void) {
	if (!sensorStreaming) {
		return;
	}
	sensorStreaming = false;

	cisdp_sensor_stop();	// Stops the data path, and puts the HM0360 in MODE_SLEEP
	applyRestMode();
}

/**
 * @brief Gets the HM0360 ready for DPD.
 *
 * The sensor is already in its resting mode, which it keeps through DPD (its supply stays on). If motion
 * detection is on, the interrupt bits are cleared (a raised interrupt would hold the WAKE pin high and end DPD at
 * once) and then the interrupt is enabled, as md's hm0360_md_prepare() does.
 */
static void prepareSensorForDpd(void) {
	if (mdIntervalMs > 0) {
		hm0360_md_clearInterrupt(0xff);
		hm0360_md_enableInterrupt();
	}
	xprintf("Image: HM0360 left in mode %d, context %c, MD interval %d ms for DPD\n", (int) restMode,
			(restContext == CONTEXT_B) ? 'B' : 'A', (int) mdIntervalMs);
#if PRINT_REGISTERS_BEFORE_DPD
	printSensorRegisters();
#endif // PRINT_REGISTERS_BEFORE_DPD
}

#if PRINT_REGISTERS_BEFORE_DPD
/**
 * @brief Prints the HM0360 registers that decide what the sensor does in DPD, as read back from the sensor.
 *
 * Shows the state that is really in the sensor, not what this task believes it set: the mode, the context
 * selection, the motion detection and PMU (frame count, sleep count, pre-metering) settings, and the frame and
 * line lengths of the three blocks of per-context registers in the .i table (context A at 0x3500, context B at
 * 0x355A, and a third block at 0x35B4 whose purpose is not yet known).
 */
static void printSensorRegisters(void) {
	static const struct {
		uint16_t address;
		const char *name;
	} registers[] = {
		{ MODE_SELECT, "MODE_SELECT" },
		{ PMU_CFG_3, "PMU_CFG_3 (context)" },
		{ PMU_CFG_5, "PMU_CFG_5 (pre-meter)" },
		{ PMU_CFG_7, "PMU_CFG_7 (frames)" },
		{ PMU_CFG_8, "PMU_CFG_8 (sleep H)" },
		{ PMU_CFG_9, "PMU_CFG_9 (sleep L)" },
		{ MD_CTRL, "MD_CTRL" },
		{ MD_CTRL1, "MD_CTRL1" },
		{ INTEGRATION_H, "INTEGRATION_H" },
		{ INTEGRATION_L, "INTEGRATION_L" },
		{ FRAME_LEN_LINES_H, "FRAME_LEN_LINES_H" },
		{ FRAME_LEN_LINES_L, "FRAME_LEN_LINES_L" },
		{ LINE_LEN_PCK_H, "LINE_LEN_PCK_H" },
		{ LINE_LEN_PCK_L, "LINE_LEN_PCK_L" },
		{ 0x3503, "A frame length H" },
		{ 0x3504, "A frame length L" },
		{ 0x3505, "A line length H" },
		{ 0x3506, "A line length L" },
		{ 0x355D, "B frame length H" },
		{ 0x355E, "B frame length L" },
		{ 0x355F, "B line length H" },
		{ 0x3560, "B line length L" },
		{ 0x3561, "B H_SUB" },
		{ 0x3562, "B V_SUB" },
		{ 0x356A, "B output enable" },
		{ 0x35B7, "35B4 block frame length H" },
		{ 0x35B8, "35B4 block frame length L" },
		{ 0x35B9, "35B4 block line length H" },
		{ 0x35BA, "35B4 block line length L" },
		{ STROBE_CFG, "STROBE_CFG" },
	};
	uint8_t value;
	uint8_t i;

	xprintf("Image: HM0360 registers before DPD:\n");
	for (i = 0; i < (sizeof(registers) / sizeof(registers[0])); i++) {
		if (hx_drv_cis_get_reg(registers[i].address, &value) == HX_CIS_NO_ERROR) {
			xprintf("  0x%04x = 0x%02x  %s\n", (unsigned) registers[i].address, (unsigned) value, registers[i].name);
		}
		else {
			xprintf("  0x%04x could not be read  %s\n", (unsigned) registers[i].address, registers[i].name);
		}
	}
}
#endif // PRINT_REGISTERS_BEFORE_DPD

/**
 * @brief Sets the resting mode, or prints the mode.
 *
 * @param mode The HM0360 mode (0 to 7, but not 5), or IMAGE_TASK_MODE_REPORT.
 */
static void handleSetMode(uint8_t mode) {
	mode_select_t modeNow;
	uint8_t idHigh = 0;
	uint8_t idLow = 0;
	uint8_t intIndic = 0;
	uint16_t frameCountNow;

	if (!readyForChange()) {
		return;
	}

	if (mode == IMAGE_TASK_MODE_REPORT) {
		if (hm0360_md_getMode(&modeNow) == HX_CIS_NO_ERROR) {
			xprintf("Image: HM0360 is in mode %d, the resting mode is %d, %d picture(s) saved in this boot\n",
					(int) modeNow, (int) restMode, (int) picturesSaved);
			xprintf("Image: resting context %c, motion detection interval %d ms (%s)",
					(restContext == CONTEXT_B) ? 'B' : 'A', (int) mdIntervalMs, (mdIntervalMs > 0) ? "on" : "off");
			// Reading INT_INDIC does not clear it: that is done at a warm boot and before DPD
			if (hm0360_md_getInterruptStatus(&intIndic) == HX_CIS_NO_ERROR) {
				xprintf(", INT_INDIC 0x%02x%s\n", (unsigned) intIndic, ((intIndic & MD_INT) != 0) ? " (motion)" : "");
			}
			else {
				xprintf(", INT_INDIC could not be read\n");
			}

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
 * @brief Sets the register context of the resting mode, and applies it.
 *
 * @param context CONTEXT_A or CONTEXT_B.
 */
static void handleSetContext(uint8_t context) {
	if (!readyForChange()) {
		return;
	}

	restContext = (context == CONTEXT_B) ? CONTEXT_B : CONTEXT_A;
	applyRestMode();
	xprintf("Image: HM0360 resting context set to %c\n", (restContext == CONTEXT_B) ? 'B' : 'A');
}

/**
 * @brief Sets the motion detection interval of the resting mode, and applies it.
 *
 * The interval is the sleep between frames in mode 2 (and mode 6). A non-zero interval enables the motion
 * detection interrupt; zero disables it and selects the longest sleep.
 *
 * @param intervalMs The interval in ms, 0 = off. Values above MD_MAX_INTERVAL_MS give the longest sleep.
 */
static void handleSetMdInterval(uint16_t intervalMs) {
	if (!readyForChange()) {
		return;
	}

	if (intervalMs > MD_MAX_INTERVAL_MS) {
		xprintf("Image: %d ms is longer than the sensor can sleep, so %d ms is used\n", (int) intervalMs,
				MD_MAX_INTERVAL_MS);
		intervalMs = MD_MAX_INTERVAL_MS;
	}

	mdIntervalMs = intervalMs;
	applyRestMode();
	xprintf("Image: motion detection interval set to %d ms (%s)\n", (int) mdIntervalMs,
			(mdIntervalMs > 0) ? "on" : "off");

	if ((mdIntervalMs > 0) && (restMode != MODE_SW_NFRAMES_SLEEP)) {
		xprintf("Image: note the resting mode is %d; motion detection is designed for mode 2\n", (int) restMode);
	}
}
#endif // USE_HM0360

#ifdef USE_RP3
/********************************************* RP3 camera functions ******************************************/

/**
 * @brief Powers the RP3 up and writes all its registers, as ww500_md does at CAMERA_CONFIG_INIT_COLD.
 *
 * cisdp_sensor_init() waits CIS_POWERUP_DELAY after SENSOR_ENABLE goes high, before the first register write.
 *
 * @return true if the camera is powered and initialised. On failure it is powered down again.
 */
static bool rpPowerUp(void) {
	TickType_t startTime = xTaskGetTickCount();

	pinmux_cfg_rpSensorEnable(true);

	if (cisdp_sensor_init(true) != 0) {
		XP_RED;
		xprintf("Image: RP3 initialisation failed\n");
		XP_WHITE;
		pinmux_cfg_rpSensorEnable(false);
		return false;
	}

	rpPowered = true;
	xprintf("Image: RP3 powered and initialised in %d ms\n", (int) ww500_minimal_getElapsedMs(startTime));
	return true;
}

/**
 * @brief Powers the RP3 down. It loses its registers.
 */
static void rpPowerDown(void) {
	pinmux_cfg_rpSensorEnable(false);
	rpPowered = false;
}

/**
 * @brief Checks that the RP3 is there, then powers it down until it is needed.
 *
 * The camera is powered, given CIS_POWERUP_DELAY to start, and read at its I2C address, as ww500_md's
 * checkForCameras() does. Its registers are not written here: that is done for each picture (or by 'cam on'),
 * because it loses them while SENSOR_ENABLE is low. Cold and warm boots are the same, as SENSOR_ENABLE is low in DPD.
 *
 * @param coldBoot Not used: the RP3 is initialised for every picture.
 * @return true if the camera answered.
 */
static bool initCamera(bool coldBoot) {
	bool expanderOk;
	bool present;
	uint8_t idHigh = 0;
	uint8_t idLow = 0;

	(void) coldBoot;

	rpHeldOn = false;

	if (!initI2cAndExpander(&expanderOk)) {
		return false;
	}

	pinmux_cfg_rpSensorEnable(true);
	vTaskDelay(pdMS_TO_TICKS(CIS_POWERUP_DELAY));

	present = hm0360_md_isSensorPresent(CIS_I2C_ID);

	if (present) {
		// The model ID (0x0708 expected) shows that register reads are sound
		hx_drv_cis_set_slaveID(CIS_I2C_ID);
		if ((hx_drv_cis_get_reg(IMX708_MODEL_ID_H, &idHigh) == HX_CIS_NO_ERROR) &&
				(hx_drv_cis_get_reg(IMX708_MODEL_ID_L, &idLow) == HX_CIS_NO_ERROR)) {
			xprintf("Image: RP3 model ID 0x%02x%02x\n", (unsigned) idHigh, (unsigned) idLow);
		}
	}

	rpPowerDown();

	if (!present) {
		XP_YELLOW;
		xprintf("No RP3 (IMX708) at I2C address 0x%02x. Carrying on without it\n", CIS_I2C_ID);
		if (expanderOk) {
			xprintf("The bus works (the expander answered), so check the camera connector and SENSOR_ENABLE (PB7)\n");
		}
		XP_WHITE;
		return false;
	}

	XP_LT_GREEN;
	xprintf("Image: RP3 found at 0x%02x, and powered down until it is needed\n", CIS_I2C_ID);
	XP_WHITE;

	return true;
}

/**
 * @brief Gets the RP3 ready to stream: powers it up and writes its registers (unless 'cam on' has done so), then
 * sets up the data path.
 *
 * @return true if the capture can be started.
 */
static bool armCapture(void) {
	if (!rpPowered && !rpPowerUp()) {
		return false;
	}

	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG, dataPathCallback, JPEG_RATIO,
			APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		XP_RED;
		xprintf("Image: data path initialisation failed\n");
		XP_WHITE;
		if (!rpHeldOn) {
			rpPowerDown();
		}
		return false;
	}

	return true;
}

/**
 * @brief Stops the RP3 streaming (and the MIPI receiver), then powers it down unless 'cam on' holds it on. Does
 * nothing if it is not streaming.
 */
static void endCapture(void) {
	if (!sensorStreaming) {
		return;
	}
	sensorStreaming = false;

	cisdp_sensor_stop();

	if (!rpHeldOn) {
		rpPowerDown();
	}
}

/**
 * @brief Gets the RP3 ready for DPD: powers it down if 'cam on' left it powered.
 */
static void prepareSensorForDpd(void) {
	if (rpPowered) {
		rpPowerDown();
	}
	rpHeldOn = false;
	xprintf("Image: RP3 powered down for DPD\n");
}

/**
 * @brief Powers the RP3 up or down between pictures, or prints its state.
 *
 * @param mode RP3_CAM_ON, RP3_CAM_OFF or IMAGE_TASK_MODE_REPORT.
 */
static void handleSetMode(uint8_t mode) {
	if (!readyForChange()) {
		return;
	}

	if (mode == IMAGE_TASK_MODE_REPORT) {
		xprintf("Image: RP3 is %s%s, %d picture(s) saved in this boot\n", rpPowered ? "powered" : "powered down",
				rpHeldOn ? " (held on by 'cam on')" : "", (int) picturesSaved);
	}
	else if (mode == RP3_CAM_ON) {
		rpHeldOn = true;
		if (!rpPowered && !rpPowerUp()) {
			rpHeldOn = false;
		}
	}
	else if (mode == RP3_CAM_OFF) {
		rpHeldOn = false;
		if (rpPowered) {
			rpPowerDown();
		}
		xprintf("Image: RP3 powered down\n");
	}
	else {
		xprintf("Image: expected on or off\n");
	}
}
#endif // USE_RP3

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

#ifdef USE_HM0360
	bootWakeReason = (WW500_MINIMAL_WAKE_REASON_E) (uint32_t) pvParameters;
#endif // USE_HM0360

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting Image Task (expecting : %s camera)\n", CAMERA_NAME);
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
					prepareForDpd();
				}
				break;

#ifdef USE_HM0360
			case APP_MSG_IMAGETASK_SET_CONTEXT:
				handleSetContext((uint8_t) rxMessage.msg_data);
				break;

			case APP_MSG_IMAGETASK_SET_MD_INTERVAL:
				handleSetMdInterval((uint16_t) rxMessage.msg_data);
				break;
#endif // USE_HM0360

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
 * @brief Asks the task to change the camera's resting state, or to print it.
 *
 * HM0360: the resting mode (0 to 7, but not 5). RP3: 1 = powered ('cam on'), 0 = powered down ('cam off').
 *
 * @param mode The mode or state, or IMAGE_TASK_MODE_REPORT to print it.
 * @return true if the request was queued.
 */
bool image_task_requestMode(uint8_t mode) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_SET_MODE;
	sendMsg.msg_data = mode;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

#ifdef USE_HM0360
/**
 * @brief Asks the task to set the register context of the resting mode.
 *
 * @param context CONTEXT_A or CONTEXT_B.
 * @return true if the request was queued.
 */
bool image_task_requestContext(uint8_t context) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_SET_CONTEXT;
	sendMsg.msg_data = context;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Asks the task to set the motion detection interval of the resting mode.
 *
 * @param intervalMs The interval in ms, 0 = motion detection off.
 * @return true if the request was queued.
 */
bool image_task_requestMdInterval(uint16_t intervalMs) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_IMAGETASK_SET_MD_INTERVAL;
	sendMsg.msg_data = intervalMs;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xImageTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}
#endif // USE_HM0360

/**
 * @brief Asks the task to initialise the camera again, as after a cold boot.
 *
 * HM0360: writes the register table again. RP3: checks again that it answers.
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
