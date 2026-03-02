/*
 * fatfs_task.c
 *
 *  Created on: 19 Aug 2024
 *      Author: CGP
 *
 * My initial approach is to use the fatfs routines which are used in ww130_test and elsewhere,
 * then encapsulate these in a FreeRTOS task.
 *
 * It looks like the Himax code uses the "ChaN" code, which is present in the middleware/fatfs folder.
 * And within that code the mmc_spi drivers are used.
 *
 * The other approach is to use the FreeRTOS+FAT here:
 * 		https://github.com/FreeRTOS/Lab-Project-FreeRTOS-FAT
 *
 * According to Copilot:
 *
 * FreeRTOS+FAT builds upon the FatFs library by ChaN.
 * It adapts the FatFs code to work within the FreeRTOS environment, adding thread safety
 * and integration with FreeRTOS tasks.
 * FreeRTOS+FAT includes additional features specific to FreeRTOS, such as support for mutexes
 * and semaphores.
 * Essentially, FreeRTOS+FAT extends the capabilities of FatFs to make it suitable for
 * multitasking systems like FreeRTOS.
 *
 * You can also ask Copilot: "What are the advantages of using FreeRTOS+FAT over FatFs?"
 *
 * It looks like I spent time in 2022 getting FreeRTOS+FAT working on the MAX78000 - see here:
 * https://forums.freertos.org/t/freertos-fat-example-required-for-sd-card-using-spi-interface/15503/15
 *
 * Notes on 8.3 file names
 * -----------------------
 * Apparently much faster. Do this by setting FF_USE_LFN to 0 in ffconf.h
 * See this ChatGPT discussion: https://chatgpt.com/share/6861bbf0-f8e0-8005-af0a-3f42d0fcb775
 *
 * Notes on SD cards > 32G
 * -------------------------
 * These are probaly supporting exFAT. The above ChatGPT conversation suggested I install fat32format.exe
 * from here: http://ridgecrop.co.uk/index.htm?fat32format.htm
 * This worked for me - formatted 64G cards as FAT32
 *
 */

/*************************************** Includes *******************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "WE2_device.h"
#include "WE2_debug.h"
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

#include "fatfs_task.h"
#include "image_task.h"
#include "app_msg.h"
#include "CLI-commands.h"
#include "ff.h"
#include "CLI-FATFS-commands.h"
#include "time_handling.h"

// TODO I am not using the public functions in this. Can we move the important bits of this to here?
#include "spi_fatfs.h"
#include "hx_drv_rtc.h"
#include "exif_utc.h"
#include "ww500_md.h"
#include "inactivity.h"
#include "directory_manager.h"

#include "barrier.h"
#include "selfTest.h"
#include "cvapp.h"

// TODO this is for the default project id and version - move elsewhere?
#include "common_config.h"

/*************************************** Definitions *******************************************/

// TODO sort out how to allocate priorities
#define fatfs_task_PRIORITY (configMAX_PRIORITIES - 3)

#define FATFS_TASK_QUEUE_LEN 10

#define DRV ""

// Length of lines in configuration.txt
#define MAXCOMMENTLENGTH 80
// Max number of comment lines in configuration.txt
#define MAXNUMCOMMENTS OP_PARAMETER_NUM_ENTRIES + 5

/*************************************** Local Function Declarations *****************************/

// This is the FreeRTOS task
static void vFatFsTask(void *pvParameters);

static FRESULT fatFsInit(void);

// These are separate event handlers, one for each of the possible state machine state
static APP_MSG_DEST_T handleEventForUninit(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForIdle(APP_MSG_T rxMessage);
static APP_MSG_DEST_T handleEventForBusy(APP_MSG_T rxMessage);

// This is to process an unexpected event
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage);

static FRESULT fileRead(fileOperation_t *fileOp);
static FRESULT fileWrite(fileOperation_t *fileOp);

// Warning: list_dir() is in spi_fatfs.c - how to declare it and reuse it?
FRESULT list_dir(const char *path);

static FRESULT load_configuration(const char *filename, directoryManager_t *dirManager);
FRESULT save_configuration(const char *filename, directoryManager_t *dirManager);

// ZIP and label handling functions (moved from cvapp.cpp)
static int8_t load_labels_from_sd(const char *path, char labels[][MAX_LABEL_LEN], uint8_t *label_count, uint8_t max_labels, uint8_t max_label_len);

#ifdef UNZIPMANIFEST
static int fatfs_unzip_manifest_zip(void);
int fatfs_unzip_manifest(void);
#endif //  UNZIPMANIFEST

/*************************************** External Function Declaraions *******************************************/

extern FRESULT init_directories(directoryManager_t *dirManager);
extern FRESULT add_capture_folder(directoryManager_t *dirManager);

/*************************************** External variables *******************************************/

extern directoryManager_t dirManager;
extern QueueHandle_t xIfTaskQueue;
extern QueueHandle_t xImageTaskQueue;

extern Barrier_t startupBarrier; // Object that calls a function when all tasks are ready
extern Barrier_t shutdownBarrier;  // Object that calls a function when all tasks are ready to shut down

/*************************************** Local variables *******************************************/

SemaphoreHandle_t xSDInitDoneSemaphore;

static APP_WAKE_REASON_E woken;

// This is the handle of the task
TaskHandle_t fatFs_task_id;
QueueHandle_t xFatTaskQueue;

// These are the handles for the input queues of Task2. So we can send it messages
// extern QueueHandle_t     xFatTaskQueue;

volatile APP_FATFS_STATE_E fatFs_task_state = APP_FATFS_STATE_UNINIT;

static FATFS fs; /* Filesystem object */

static bool mounted;

static TickType_t xStartTime;

// Strings for each of these states. Values must match APP_TASK1_STATE_E in task1.h
const char *fatFsTaskStateString[APP_FATFS_STATE_NUMSTATES] = {
	"Uninitialised",
	"Idle",
	"Busy"
};

// Strings for expected messages. Values must match messages directed to fatfs Task in app_msg.h
const char *fatFsTaskEventString[APP_MSG_FATFSTASK_LAST - APP_MSG_FATFSTASK_WRITE_FILE] = {
	"Write file",
	"Read file",
	"File op done",
	"Save State",
};

// Number of pictures to take after motion detect wake
uint32_t numPicturesToGrab = NUMPICTURESTOGRAB;

// Interval between pictures (for now seconds, but let's change this to ms later
uint32_t pictureInterval = PICTUREINTERVAL;

// Values to read from the CONFIG.TXT file
uint16_t op_parameter[OP_PARAMETER_NUM_ENTRIES] = {
	1,				   // 0 Image file number (0 indicates no SD card)
	0,				   // 1 # times the NN model has run
	0,				   // 2 # times the NN model says "yes"
	0,				   // 3 # of AI processor cold boots
	0,				   // 4 # of AI processor warm boots
	NUMPICTURESTOGRAB, // 5 Num pics when triggered
	PICTUREINTERVAL,   // 6 Pic interval when triggered (ms)
	TIMELAPSEINTERVAL, // 7 Interval (s) (0 inhibits)
	INACTIVITYTIMEOUT, // 8 Delay before DPD (ms)
	FLASHLEDDUTY,	   // 9 in percent (0 inhibits)
	1,				   // 10 0 = disabled, 1 = enabled
	DPDINTERVAL,	   // 11 Interval (ms) between frames in MD mode (0 inhibits)
	FLASHDURATION,	   // 12 Duration (ms) that LED Flash is on
	0,				   // 13 LED bit mask: vis=1, IR=2, none=0
	PROJECT_ID,		   // 14 OP_PARAMETER_MODEL_PROJECT
	PROJECT_VER,	   // 15 OP_PARAMETER_MODEL_VERSION
	MODEL_THRESHOLD,   // 16 OP_PARAMETER_MODEL_THRESHOLD default
	0, 0, 0,	   		// 17-19 Reserved for future use
	0, 0, 0, 0, 0, 0, 0, 0  // 20-27 Deployment ID chunks
};

/********************************** Private Function definitions  *************************************/

/** Another task asks us to write a file for them
 *
 */
static FRESULT fileWrite(fileOperation_t *fileOp) {
	FIL fdst;	 // File object
	FRESULT res; // FatFs function common result code
	UINT bw;	 // Bytes written

	// TODO omit this soon as it might not handle long files or binary files
	// xprintf("DEBUG: writing %d bytes to '%s' from address 0x%08x. Contents:\n%s\n",
	//		fileOp->length, fileOp->fileName, fileOp->buffer, fileOp->buffer );

	res = f_open(&fdst, fileOp->fileName, FA_WRITE | FA_CREATE_ALWAYS);
	if (res) {
		xprintf("Fail opening file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		return res;
	}

	res = f_write(&fdst, fileOp->buffer, fileOp->length, &bw);
	if (res) {
		xprintf("Fail writing to file %s\n", fileOp->fileName);
		fileOp->length = bw;
		fileOp->res = res;
		return res;
	}

	// TODO experimental:leave file open so it can be appended? TODO need to make stuff static?
	if (fileOp->closeWhenDone) {
		res = f_close(&fdst);

		if (res) {
			xprintf("Fail closing file %s\n", fileOp->fileName);
			fileOp->length = bw;
			fileOp->res = res;
			return res;
		}
	}

	if (bw != (fileOp->length)) {
		xprintf("Error. Wrote %d bytes rather than %d\n", bw, fileOp->length);
		res = FR_DISK_ERR; // TODO find a better error code? Disk full?
	} else {
		xprintf("Wrote %d bytes\n", bw);
		res = FR_OK;
	}
	XP_GREEN
	xprintf("Wrote file to SD %s\n", fileOp->fileName);
	XP_WHITE;
	fileOp->res = res;
	return res;
}

/** Image writing function, will primarily be called from the image task
 *
 * Called when APP_MSG_FATFSTASK_WRITE_FILE message arrives in fatfs task queue
 * 		parameters: fileOperation_t fileOp
 * 		returns: FRESULT res
 */
static FRESULT fileWriteImage(fileOperation_t *fileOp, directoryManager_t *dirManager) {
	FRESULT res;
	rtc_time time;

	// Move to fastfs_write_image()
	//	res = f_chdir(dirManager->current_capture_dir);
	//	if (res != FR_OK) {
	//		return res;
	//	}

	// fastfs_write_image() expects filename is a uint8_t array
	// TODO resolve this warning! "warning: passing argument 1 of 'fastfs_write_image' makes integer from pointer without a cast"
	res = fastfs_write_image((uint32_t)(fileOp->buffer), fileOp->length, (uint8_t *)fileOp->fileName, dirManager);

	if (res != FR_OK) {
		xprintf("Error writing file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;

		return res;
	}

	XP_GREEN
	xprintf("Wrote %d byte image to SD: %s ", fileOp->length, fileOp->fileName);
	XP_WHITE;

	exif_utc_get_rtc_as_time(&time);

	xprintf("at %d:%d:%d %d/%d/%d\n",
			time.tm_hour, time.tm_min, time.tm_sec,
			time.tm_mday, time.tm_mon, time.tm_year);

	return res;
}

/** Another task asks us to read a file for them
 *
 */
static FRESULT fileRead(fileOperation_t *fileOp) {
	FIL fsrc;	 // File object
	FRESULT res; // FatFs function common result code
	UINT br;	 // Bytes read

	//	xprintf("DEBUG: reading file %s to buffer at address 0x%08x (%d bytes)\n",
	//			fileOp->fileName, fileOp->buffer, fileOp->length);

	res = f_open(&fsrc, fileOp->fileName, FA_READ);
	if (res) {
		xprintf("Fail opening file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		return res;
	}

	// Read a chunk of data from the source file
	res = f_read(&fsrc, fileOp->buffer, fileOp->length, &br);

	// TODO experimental: leave file open so it can be appended? TODO need to make stuff static?
	if (fileOp->closeWhenDone) {
		res = f_close(&fsrc);

		if (res) {
			xprintf("Fail closing file %s\n", fileOp->fileName);
			fileOp->length = 0;
			fileOp->res = res;
			return res;
		}
	}

	xprintf("Read %d bytes\n", br);
	fileOp->length = br;
	fileOp->res = res;
	return res;
}

/**
 * Implements state machine when in APP_FATFS_STATE_UNINIT
 *
 * If disk operation requests happen they are
 *
 */
static APP_MSG_DEST_T handleEventForUninit(APP_MSG_T rxMessage) {
	APP_MSG_EVENT_E event;
	FRESULT res;
	fileOperation_t *fileOp;
	static APP_MSG_DEST_T sendMsg;

	sendMsg.destination = NULL;
	res = FR_OK;

	event = rxMessage.msg_event;

	fileOp = (fileOperation_t *)rxMessage.msg_data;

	switch (event) {

	case APP_MSG_FATFSTASK_WRITE_FILE:
		// someone wants a file written. Send back an error message

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		} else if (sendMsg.destination == xImageTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_WRITE_COMPLETE;
		else {
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_WRITE_COMPLETE;
		}
		break;

	case APP_MSG_FATFSTASK_READ_FILE:
		// someone wants a file read. Send back an error message

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_READ_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_READ_COMPLETE;
		else {
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_READ_COMPLETE;
		}

		break;

	case APP_MSG_FATFSTASK_SAVE_STATE:
		// Save the state of the imageSequenceNumber
		// This is the last thing we will do before sleeping.

		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;

		// Signal to the caller that it may enter DPD.
		sendMsg.destination = xImageTaskQueue; // fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		sendMsg.message.msg_data = (uint32_t)res;

		break;

	case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
		break;

	default:
		// Here for events that are not expected in this state.
		flagUnexpectedEvent(rxMessage);
		break;
	}
	return sendMsg;
}

/**
 * Implements state machine when in APP_FATFS_STATE_IDLE
 *
 */
static APP_MSG_DEST_T handleEventForIdle(APP_MSG_T rxMessage) {
	APP_MSG_EVENT_E event;
	FRESULT res;
	fileOperation_t *fileOp;
	static APP_MSG_DEST_T sendMsg;

	sendMsg.destination = NULL;
	res = FR_OK;

	event = rxMessage.msg_event;

	fileOp = (fileOperation_t *)rxMessage.msg_data;

	switch (event) {

	case APP_MSG_FATFSTASK_WRITE_FILE:
		// someone wants a file written. Structure including file name a buffer is passed in data
		fatFs_task_state = APP_FATFS_STATE_BUSY;
		xStartTime = xTaskGetTickCount();

		if (fileOp->senderQueue == xImageTaskQueue) {
			// writes image
			res = fileWriteImage(fileOp, &dirManager);
		} else {
			// writes file
			res = fileWrite(fileOp);
		}

		xprintf("File write took %dms\n", app_getElapsedMs(xStartTime));

		fatFs_task_state = APP_FATFS_STATE_IDLE;

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		} else if (sendMsg.destination == xImageTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		} else {
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_WRITE_COMPLETE;
		}
		break;

	case APP_MSG_FATFSTASK_READ_FILE:
		// someone wants a file read
		fatFs_task_state = APP_FATFS_STATE_BUSY;
		xStartTime = xTaskGetTickCount();
		res = fileRead(fileOp);

		xprintf("Elapsed time (fileRead) %dms. Result code %d\n", (xTaskGetTickCount() - xStartTime) * portTICK_PERIOD_MS, res);

		fatFs_task_state = APP_FATFS_STATE_IDLE;

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_READ_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_READ_COMPLETE;
		else {
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_READ_COMPLETE;
		}

		break;

	case APP_MSG_FATFSTASK_SAVE_STATE:
		// Save the state of the imageSequenceNumber
		// This is the last thing we will do before sleeping.
		if (fatfs_getOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER) > 0) {
			res = save_configuration(STATE_FILE, &dirManager);
			f_unmount(DRV);

			if (res) {
				xprintf("Error %d saving state\n", res);
			} else {
				xprintf("Saved state to SD card. Image sequence number = %d\n",
						fatfs_getImageSequenceNumber());
			}
		}

		// Signal to the caller that it may enter DPD.
		sendMsg.destination = xImageTaskQueue; // fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		sendMsg.message.msg_data = (uint32_t)res;

		break;

	case APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE:
		break;

	default:
		// Here for events that are not expected in this state.
		flagUnexpectedEvent(rxMessage);
		break;
	}
	return sendMsg;
}

/**
 * Implements state machine when in APP_FATFS_STATE_BUSY
 *
 */
static APP_MSG_DEST_T handleEventForBusy(APP_MSG_T rxMessage) {
	APP_MSG_EVENT_E event;
	// uint32_t data;
	APP_MSG_DEST_T sendMsg;
	sendMsg.destination = NULL;

	event = rxMessage.msg_event;
	// data = rxMessage.msg_data;

	switch (event) {

	case APP_MSG_FATFSTASK_DONE:
		// someone wants a file written
		fatFs_task_state = APP_FATFS_STATE_IDLE;
		break;

	default:
		// Here for events that are not expected in this state.
		flagUnexpectedEvent(rxMessage);
		break;
	}

	// If non-null then our task sends another message to another task
	return sendMsg;
}

/**
 * For state machine: Print a red message to see if there are unhandled events we should manage
 */
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage) {
	APP_MSG_EVENT_E event;
	APP_MSG_DEST_T sendMsg;
	sendMsg.destination = NULL;

	event = rxMessage.msg_event;

	XP_LT_RED;
	if ((event >= APP_MSG_IFTASK_FIRST) && (event < APP_MSG_IFTASK_LAST)) {
		xprintf("FatFS Task unhandled event '%s' in '%s'\r\n", fatFsTaskEventString[event - APP_MSG_IFTASK_FIRST], fatFsTaskStateString[fatFs_task_state]);
	} else {
		xprintf("FatFS Task unhandled event 0x%04x in '%s'\r\n", event, fatFsTaskStateString[fatFs_task_state]);
	}
	XP_WHITE;

	// If non-null then our task sends another message to another task
	return sendMsg;
}

/**
 * Initialise FatFS system
 *
 * Earlier versions (e.g. in spi_fatfs.c) initialise the PB2-5 pins used for the SD card interface,
 * but this largely replicated code in spi_m_pinmux_cfg() (called from pinmux_init() via app_main())
 * and I think it is cleaner if the pin setup is done there, in one place.
 *
 * It looks like the low-level drivers are done in mmc_we2_spi.c, selected by these lines in the makefile:
 * 	MID_SEL = fatfs
 * 	FATFS_PORT_LIST = mmc_spi
 *
 * Note that although the hardware includes a card detect signal, card detect is always true:
 * 	#define MMC_CD()    1
 * And the lower-level SPI operations use ARM_DRIVER_SPI in Driver_SPI.h
 *
 * It is not clear to me what links there is between the hardware and the chip's SPI block, and the ARM driver...
 * Though there are calls to SSPI_CS_GPIO_Pinmux(), SSPI_CS_GPIO_Output_Level() and SSPI_CS_GPIO_Dir()
 * which are obviously used to control the /CS pin.
 */
static FRESULT fatFsInit(void) {
	FRESULT res;

	XP_CYAN;
	xprintf("Mounting FatFS on SD card ");
	XP_WHITE;

	// This is probably blocking...
	res = f_mount(&fs, DRV, 1);

	if (res) {
		XP_RED;
		xprintf("Failed error = %d\r\n", res);
		XP_WHITE;
		mounted = false;
	} else {
		xprintf("OK\n");
		mounted = true;
	}
	return res;
}

/**
 * Loads configuration information from a file.
 *
 * The file comprises several lines each with two integers.
 * The first integer is an index into the configuration[] array.
 * The second integer is the value to place into the array.
 *
 * Default values for configuration[] are set in the task initialisation.
 *
 * @param file name
 * @return error code
 */
static FRESULT load_configuration(const char *filename, directoryManager_t *dirManager) {
	FRESULT res;
	char line[64];
	char *token;
	uint8_t index;
	uint16_t value;

    if (!fatfs_mounted()) {
        xprintf("SD card not mounted.\n");
    	return FR_NO_FILESYSTEM;
    }

	if (!dirManager->configOpen) {
		res = f_chdir(dirManager->current_config_dir);
		if (res != FR_OK) {
			xprintf("Failed to change to: %s\n", dirManager->current_config_dir);
			return res;
		}
    	xprintf("Loading configuration: ");
    	fatfs_printCwd();	// will print CWD

		// Open the file
		res = f_open(&dirManager->configFile, filename, FA_READ);
		if (res != FR_OK) {
			xprintf("Failed to open config file: %d\n", res);
			dirManager->configRes = res;
			return dirManager->configRes;
		}
		dirManager->configOpen = true;

		// Read lines from the file
		while (f_gets(line, sizeof(line), &dirManager->configFile)) {
			// Remove trailing newline if present
			char *newline = strchr(line, '\n');
			if (newline) {
				*newline = '\0';
			}

			// Skip comments which start with #
			if (line[0] == '#') {
				continue;
			}

			// The first call to strtok() should have a pointer to the string which
			// should be split, while any following calls should use NULL as an
			// argument. Each time the function is called a pointer to a different
			// token is returned until there are no more tokens.
			// At that point each function call returns NULL.
			token = strtok(line, " ");
			if (token == NULL) {
				continue;
			}

			index = (uint8_t)atoi(token);

			token = strtok(NULL, " ");
			if (token == NULL) {
				continue;
			}

			value = (uint16_t)atoi(token);

			// Set array value if index is in range
			if (index >= 0 && index < OP_PARAMETER_NUM_ENTRIES) {
				op_parameter[index] = value;
				// debug only:
				// xprintf("   op_parameter[%d] = %d\n", index, value);
			}
		}
	}

	// Close file
	res = f_close(&dirManager->configFile);
	if (res != FR_OK) {
		xprintf("Failed to close config file: %d\n", res);
	} else {
		dirManager->configOpen = false;
	}
	dirManager->configRes = res;

	return res;
}

/**
 * Saves the current configuration to a file.
 *
 * The file comprises several lines each with two integers.
 * The first integer is an index into the configuration[] array.
 * The second integer is the value to place into the array.
 *
 * Default values for configuration[] are set in the task initialisation.
 *
 * @param filename name of the file to save to
 * @param dirManager pointer to the directoryManager_t structure
 * @return error code
 */
FRESULT save_configuration(const char *filename, directoryManager_t *dirManager) {
	FRESULT res;
	UINT bytesWritten;
	char line[MAXCOMMENTLENGTH];
	char comment_lines[MAXNUMCOMMENTS][MAXCOMMENTLENGTH];
	uint16_t comment_count = 0;

    if (!fatfs_mounted()) {
        xprintf("SD card not mounted.\n");
    	return FR_NO_FILESYSTEM;
    }

	if (!dirManager->configOpen) {
		res = f_chdir(dirManager->current_config_dir);
		if (res != FR_OK) {
			return res;
		}

    	//xprintf("Saving configuration: ");
    	//fatfs_printCwd();	// will print CWD

		// --- First Pass: Try to read existing comment lines ---
		res = f_open(&dirManager->configFile, filename, FA_READ);
		if (res == FR_OK) {
			dirManager->configOpen = true;
			while (f_gets(line, sizeof(line), &dirManager->configFile)) {
				if (line[0] == '#') {
					if (comment_count < MAXNUMCOMMENTS) {
						strncpy(comment_lines[comment_count], line, MAXCOMMENTLENGTH);
						comment_lines[comment_count][MAXCOMMENTLENGTH - 1] = '\0';
						comment_count++;
					} else {
						break;
					}
				}
			}
			res = f_close(&dirManager->configFile);
			if (res != FR_OK) {
				xprintf("Failed to close config file: %d\n", res);
			} else {
				dirManager->configOpen = false;
			}
		} else if (res != FR_NO_FILE) {
			dirManager->configRes = res;
			return res; // Error reading file (not just "file not found")
		}

		// --- Second Pass: Open for write ---
		res = f_open(&dirManager->configFile, filename, FA_WRITE | FA_CREATE_ALWAYS);
		if (res != FR_OK) {
			xprintf("Failed to open file for writing: %d\n", res);
			dirManager->configRes = res;
			return res;
		}
		dirManager->configOpen = true;

		// Write comment lines
		for (uint16_t i = 0; i < comment_count; i++) {
			f_write(&dirManager->configFile, comment_lines[i], strlen(comment_lines[i]), &bytesWritten);
		}

		// Write parameters
		for (uint8_t i = 0; i < OP_PARAMETER_NUM_ENTRIES; i++) {
			snprintf(line, sizeof(line), "%d %d\n", i, op_parameter[i]);
			f_write(&dirManager->configFile, line, strlen(line), &bytesWritten);
		}

		// Close file and restore original directory
		res = f_close(&dirManager->configFile);
		if (res != FR_OK) {
			xprintf("Failed to close config file: %d\n", res);
		} else {
			dirManager->configOpen = false;
		}
		dirManager->configRes = res;
	}

	return dirManager->configRes;
}

/**
 * Load labels from SD card text file, one per line
 *
 * @param path Path to the labels file
 * @param labels 2D array to store labels
 * @param label_count Pointer to store number of labels loaded
 * @param max_labels Maximum number of labels supported
 * @param max_label_len Maximum length of each label
 * @return 0 on success, -1 on failure
 */
static int8_t load_labels_from_sd(const char *path, char labels[][MAX_LABEL_LEN], uint8_t *label_count, uint8_t max_labels, uint8_t max_label_len) {
	FIL f;
	FRESULT res;
	char line[96];
	UINT br = 0;
	int pos = 0;
	bool labels_loaded;

	*label_count = 0;

	res = f_open(&f, path, FA_READ);

	if (res != FR_OK) {
		xprintf("Labels open failed: %s (err %d)\n", path, res);
		return -1;
	}

	for (;;) {
		char c;
		res = f_read(&f, &c, 1, &br);
		if (res != FR_OK || br == 0) {
			// EOF – flush pending line
			if (pos > 0 && *label_count < max_labels) {
				line[pos] = '\0';
				strncpy(labels[*label_count], line, max_label_len - 1);
				labels[*label_count][max_label_len - 1] = '\0';
				(*label_count)++;
			}
			break;
		}

		if (c == '\r') {
			continue;
		}

		if (c == '\n') {
			if (pos > 0 && *label_count < max_labels) {
				line[pos] = '\0';
				strncpy(labels[*label_count], line, max_label_len - 1);
				labels[*label_count][max_label_len - 1] = '\0';
				(*label_count)++;
			}
			pos = 0;
		}
		else if (pos < (int)sizeof(line) - 1) {
			line[pos++] = c;
		}
	}

	f_close(&f);

	labels_loaded = (*label_count > 0);

	return labels_loaded ? 0: -1;
}

#ifdef UNZIPMANIFEST
/**
 * Minimal unzipper: extract manifest/labels.txt and manifest/MOD0000X.tfl
 * Only supports method 0 (STORE, no compression).
 *
 * @return 0 on success (both files present or at least model), -1 on failure.
 */
static int fatfs_unzip_manifest_zip(void) {
	// Canonical manifest locations (8.3 + uppercase), but be tolerant to zip filename case variants.
	const char *zip_candidates[] = {
		"/MANIFEST.ZIP",
		"/MANIFEST.zip",
		"/Manifest.zip",
		"/manifest.zip",
		"/Manifest.ZIP",
		"/manifest.ZIP",
	};
	const char *zip_path = NULL;
	const char *out_dir = "/MANIFEST";
	FIL zf;
	FRESULT res = FR_NO_FILE;
	for (unsigned i = 0; i < (sizeof(zip_candidates) / sizeof(zip_candidates[0])); i++) {
		res = f_open(&zf, zip_candidates[i], FA_READ);
		if (res == FR_OK) {
			zip_path = zip_candidates[i];
			break;
		}
	}
	if (res != FR_OK) {
		xprintf("No manifest zip present (err %d)\n", res);
		return -1;
	}

	DWORD fsize = f_size(&zf);
	xprintf("manifest zip '%s' size=%lu bytes\n", zip_path ? zip_path : "<unknown>", (unsigned long)fsize);

	// Don't create /MANIFEST up-front. If unzip fails early (bad/unsupported zip),
	// creating the directory here masks the failure and causes confusing behavior.
	bool outdir_created = false;

	if (f_lseek(&zf, 0) != FR_OK) {
		f_close(&zf);
		return -1;
	}

	int models_extracted = 0;
	bool labels_extracted = false;
	bool config_extracted = false;

	// First pass: Extract all files
	while (f_tell(&zf) < fsize) {
		// Note: keep parsing simple; we don't currently need the local file offset.
		uint8_t lfh[30];
		UINT br = 0;

		if (f_read(&zf, lfh, sizeof(lfh), &br) != FR_OK || br != sizeof(lfh)) {
			break;
		}

		if (!(lfh[0] == 0x50 && lfh[1] == 0x4b && lfh[2] == 0x03 && lfh[3] == 0x04)) {
			xprintf("ZIP parse stopped: local header signature mismatch at offset %lu\n", (unsigned long)(f_tell(&zf) - sizeof(lfh)));
			break;
		}

		uint16_t method = lfh[8] | (lfh[9] << 8);
		uint32_t csize = lfh[18] | (lfh[19] << 8) | (lfh[20] << 16) | (lfh[21] << 24);
		uint16_t fnlen = lfh[26] | (lfh[27] << 8);
		uint16_t xlen = lfh[28] | (lfh[29] << 8);

		char name[128];
		if (fnlen >= sizeof(name))
			fnlen = sizeof(name) - 1;
		if (f_read(&zf, name, fnlen, &br) != FR_OK || br != fnlen)
			break;
		name[fnlen] = '\0';

		if (xlen > 0) {
			if (f_lseek(&zf, f_tell(&zf) + xlen) != FR_OK)
				break;
		}

		if (method != 0) {
			if (f_lseek(&zf, f_tell(&zf) + csize) != FR_OK)
				break;
			continue;
		}

		// ZIP entries may use either '/' or '\\' as separators.
		const char *slash_fwd = strrchr(name, '/');
		const char *slash_bak = strrchr(name, '\\');
		const char *slash = slash_fwd;
		if (slash_bak && (!slash_fwd || slash_bak > slash_fwd)) {
			slash = slash_bak;
		}
		const char *base = slash ? slash + 1 : name;
		// Skip directory entries (e.g. "MANIFEST/" or names ending with a separator).
		if (base[0] == '\0') {
			// No file to extract; continue to next entry.
			continue;
		}
		// Defensive: ignore empty/odd names
		if (base[0] == '\0' || base[0] == '.') {
			if (f_lseek(&zf, f_tell(&zf) + csize) != FR_OK)
				break;
			continue;
		}

		// Minimal debug to understand real entry names on device.
		xprintf("ZIP entry: '%s' (base '%s', size %lu)\n", name, base, (unsigned long)csize);

		// If this is the config entry, always extract to canonical name.
		bool is_config_entry = ((strcasecmp(base, "config.txt") == 0) ||
								(strcasecmp(base, "config") == 0) ||
								(strcasecmp(base, STATE_FILE) == 0));

		char outpath[64];
		if (is_config_entry) {
			snprintf(outpath, sizeof(outpath), "%s/%s", out_dir, STATE_FILE);
		} else {
			snprintf(outpath, sizeof(outpath), "%s/%s", out_dir, base);
		}

		xprintf("Extracting '%s' to '%s'\n", name, outpath);

		if (!outdir_created) {
			FRESULT mk = f_mkdir(out_dir);
			if (mk != FR_OK && mk != FR_EXIST) {
				xprintf("Failed to create output dir '%s' (%d)\n", out_dir, mk);
				break;
			}
			outdir_created = true;
		}

		FIL out;
		FRESULT open_res = f_open(&out, outpath, FA_WRITE | FA_CREATE_ALWAYS);
		if (open_res != FR_OK) {
			xprintf("Failed to open '%s' for writing (err %d)\n", outpath, open_res);
			if (f_lseek(&zf, f_tell(&zf) + csize) != FR_OK)
				break;
			continue;
		}

		uint8_t buf[256];
		UINT togo = csize;
		bool copy_ok = true;
		while (togo > 0) {
			UINT chunk = (togo > sizeof(buf)) ? sizeof(buf) : togo;
			UINT rr = 0, bw = 0;
			if (f_read(&zf, buf, chunk, &rr) != FR_OK || rr != chunk ||
				f_write(&out, buf, chunk, &bw) != FR_OK || bw != chunk) {
				copy_ok = false;
				break;
			}
			togo -= chunk;
		}
		f_close(&out);

		if (!copy_ok) {
			if (f_lseek(&zf, f_tell(&zf) + togo) != FR_OK)
				break;
		} else {
			// After successful extraction, check what file it was.
			if (is_config_entry) {
				xprintf("  Found and extracted CONFIG.TXT\n");
				config_extracted = true;
			}

			if (strcasecmp(base, "labels.txt") == 0) {
				xprintf("  Found and extracted labels.txt\n");
				labels_extracted = true;
			}

			// Parse and update model info for .tfl files
			int pid = 0, ver = 0;
			const char *extension = strrchr(base, '.');
			if (extension && (strcasecmp(extension, ".tfl") == 0)) {
				if (sscanf(base, "%4dV%d", &pid, &ver) == 2) {
					xprintf("  Parsed: ProjectID=%d, Version=%d\n", pid, ver);
					cv_set_model_info(pid, ver);
					models_extracted++;
				} else {
					xprintf("  Parse FAILED for '%s'\n", base);
				}
			}
		}
	}

	// Summary + success criteria aligned to required scenarios:
	// Scenario 1 (no model): CONFIG.TXT present => success
	// Scenario 2 (with model): model + labels typically present => success
	if (!config_extracted) {
		xprintf("Warning: CONFIG.TXT was not found in the zip archive.\n");
	}
	if (!labels_extracted) {
		xprintf("Warning: labels.txt was not found in the zip archive.\n");
	}

	xprintf("Manifest unzip summary: config=%s, labels=%s, models=%d\n",
			config_extracted ? "yes" : "no",
			labels_extracted ? "yes" : "no",
			models_extracted);

	f_close(&zf);
	return (config_extracted || (models_extracted > 0)) ? 0 : -1;
}
#endif // UNZIPMANIFEST

/********************************** FreeRTOS Task  *************************************/

/**
 * FreeRTOS task responsible for handling interactions with the FatFS
 *
 * This is called when the scheduler starts.
 * Various entities have already be set up by fatfs_createTask()
 *
 * After some one-off act	ivities it waits for events to arrive in its xFatTaskQueue
 */
static void vFatFsTask(void *pvParameters) {
	APP_MSG_T rxMessage;
	APP_MSG_DEST_T txMessage;
	QueueHandle_t targetQueue;
	FRESULT res;
	uint16_t inactivityPeriod;
	APP_MSG_T sendMsg;

	APP_FATFS_STATE_E old_state;
	const char *eventString;
	APP_MSG_EVENT_E event;
	uint32_t rxData;
	bool enabled;

	TickType_t startTime;
	TickType_t elapsedTime;
	uint32_t elapsedMs;

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting FatFS Task\n");
	XP_WHITE;

	// One-off initialisation here...
	startTime = xTaskGetTickCount();

	// TODO - experiment - do I need settling time for 3V3_WE?
	vTaskDelay(pdMS_TO_TICKS(10));
	res = fatFsInit();

	if (res == FR_OK) {
		fatFs_task_state = APP_FATFS_STATE_IDLE;
		// Only if the file system is working should we add CLI commands for FATFS
		cli_fatfs_init();

		res = dir_mgr_init_directories(&dirManager);
		if (res == FR_OK) {
			xprintf("SD card initialised. ");
			fatfs_printCwd();	// for debug purposes

			// Load all the saved configuration values, including the image sequence number
			res = load_configuration(STATE_FILE, &dirManager);
			if (res == FR_OK) {
				// File exists and op_parameter[] has been initialised
				enabled = op_parameter[OP_PARAMETER_CAMERA_ENABLED];
				xprintf("'%s' found. (Next image #%d), camera %senabled. Flash brightness %d\%\r\n",
						STATE_FILE,
						fatfs_getImageSequenceNumber(),
						(enabled == 1) ? "" : "not ",
						op_parameter[OP_PARAMETER_LED_BRIGHTNESS_PERCENT]);
			} else {
				fatfs_setOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER, 1);
				xprintf("'%s' NOT found. (Next image #1)\r\n", STATE_FILE);
			}
		} else {
			// TODO what? Is this an error we must deal with?
		}
	} else {
		// Failure.
		xprintf("SD card initialisation failed (reason %d)\r\n", res);
		selfTest_setErrorBits(1 << SELF_TEST_AI_NO_SD_CARD);
	}

	elapsedTime = xTaskGetTickCount() - startTime;
	elapsedMs = (elapsedTime * 1000) / configTICK_RATE_HZ;

	xprintf("FatFs setup took %dms\n", elapsedMs);

	// Start a timer that detects inactivity in every task, exceeding op_parameter[OP_PARAMETER_INTERVAL_BEFORE_DPD]
	if (woken == APP_WAKE_REASON_COLD) {
		// Short timeout after cold boot.
		inactivityPeriod = INACTIVITYTIMEOUTCB;
		fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_COLD_BOOTS);
	}
	else {
		inactivityPeriod = op_parameter[OP_PARAMETER_INTERVAL_BEFORE_DPD];
		fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_WARM_BOOTS);
	}

	xprintf("Inactivity period set at %dms\n", inactivityPeriod);
	inactivity_init(inactivityPeriod, app_onInactivityDetection);

	// Now the Operation Parameters are loaded, send a message to the BLE processor
	sendMsg.msg_event = APP_MSG_IFTASK_AWAKE;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	// But wait a short time if it is a cold boot, so the BLE processor is initialised and ready
	if (woken == APP_WAKE_REASON_COLD) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	if (xQueueSend(xIfTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE) {
		xprintf("sendMsg=0x%x fail\r\n", sendMsg.msg_event);
	}

	// The semaphore lets the Image Task proceed
	// xprintf("DEBUG: giving semaphore so Image Task can proceed\n");
	xSemaphoreGive(xSDInitDoneSemaphore);

	barrier_ready(&startupBarrier); // Call a function when every task reaches this point

	// The task loops forever here, waiting for messages to arrive in its input queue
	for (;;) {
		if (xQueueReceive(xFatTaskQueue, &(rxMessage), __QueueRecvTicksToWait) == pdTRUE) {
			event = rxMessage.msg_event;
			rxData = rxMessage.msg_data;

			// convert event to a string
			if ((event >= APP_MSG_FATFSTASK_FIRST) && (event < APP_MSG_FATFSTASK_LAST)) {
				eventString = fatFsTaskEventString[event - APP_MSG_FATFSTASK_FIRST];
			} else {
				eventString = "Unexpected";
			}

			XP_LT_CYAN
			xprintf("\nFatFS Task ");
			XP_WHITE;
			xprintf("received event '%s' (0x%04x). Rx data = 0x%08x\r\n", eventString, event, rxData);

			old_state = fatFs_task_state;

			// switch on state - and call individual event handling functions
			switch (fatFs_task_state) {

			case APP_FATFS_STATE_UNINIT:
				txMessage = handleEventForUninit(rxMessage);
				break;

			case APP_FATFS_STATE_IDLE:
				txMessage = handleEventForIdle(rxMessage);
				break;

			case APP_FATFS_STATE_BUSY:
				txMessage = handleEventForBusy(rxMessage);
				break;

			default:
				// should not happen
				txMessage = flagUnexpectedEvent(rxMessage);
				break;
			}

			if (old_state != fatFs_task_state) {
				// state has changed
				XP_LT_CYAN;
				xprintf("FatFS Task state changed ");
				XP_WHITE;
				xprintf("from '%s' (%d) to '%s' (%d)\r\n",
						fatFsTaskStateString[old_state], old_state,
						fatFsTaskStateString[fatFs_task_state], fatFs_task_state);
			}

			// The processing functions might want us to send a message to another task
			if (txMessage.destination != NULL) {
				sendMsg = txMessage.message;
				targetQueue = txMessage.destination;

				if (xQueueSend(targetQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE) {
					xprintf("FatFS task sending event 0x%x failed\r\n", sendMsg.msg_event);
				} else {
					xprintf("FatFS task sending event 0x%04x. Tx data = 0x%08x\r\n", sendMsg.msg_event, sendMsg.msg_data);
				}
			}
		}
	} // for(;;)
}

/********************************** Public Functions  *************************************/

/**
 * Creates the FatFS task.
 *
 * The task itself initialises the FatFS and then manages requests to access it.
 *
 * Not sure how big the stack needs to be...
 */

TaskHandle_t fatfs_createTask(int8_t priority, APP_WAKE_REASON_E wakeReason) {

	if (priority < 0) {
		priority = 0;
	}

	// Save this. Determines inactivity period at cold boot
	woken = wakeReason;

	xFatTaskQueue = xQueueCreate(FATFS_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xFatTaskQueue == 0) {
		xprintf("Failed to create xFatTaskQueue\n");
		configASSERT(0); // TODO add debug messages?
	}

	if (xTaskCreate(vFatFsTask, (const char *)"FAT",
					3 * configMINIMAL_STACK_SIZE + CLI_CMD_LINE_BUF_SIZE + CLI_OUTPUT_BUF_SIZE,
					NULL, priority,
					&fatFs_task_id) != pdPASS) {
		xprintf("Failed to create vFatFsTask\n");
		configASSERT(0); // TODO add debug messages?
	}

	// Semaphore to flag that the final message has been sent and we can enter DPD
	xSDInitDoneSemaphore = xSemaphoreCreateBinary();

	if (xSDInitDoneSemaphore == NULL) {
		xprintf("Failed to create xSDInitDoneSemaphore\n");
		configASSERT(0); // TODO add debug messages?
	}
	
	return fatFs_task_id;
}

/**
 * Returns the internal state as a number
 */
uint16_t fatfs_getState(void) {
	return fatFs_task_state;
}

/**
 * Returns true of the filesystem is mounted
 *
 * Use thsi before attempting disk operations, if unsure
 *
 * @return true if mounted
 */
bool fatfs_mounted(void) {
	return mounted;
}

/**
 * Returns the internal state as a string
 */
const char *fatfs_getStateString(void) {
	return *&fatFsTaskStateString[fatFs_task_state];
}

/**
 * Get one of the Operational Parameters
 *
 * Typically these are saved in a file on SD card and can be read to set operational
 * behaviour at boot time.
 *
 * Values are set to a default and can be over-written by raeding from a file
 * or by the setter function.
 *
 * @param parameter - one of a list of possible parameters
 * @return - the value (or 0 if parameter is not recognised)
 */
uint16_t fatfs_getOperationalParameter(OP_PARAMETERS_E parameter) {

	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES)) {
		return op_parameter[parameter];
	} else {
		return 0;
	}
}

/**
 * Get the image sequence number
 *
 * Short-hand version of fatfs_getOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER)
 */
uint16_t fatfs_getImageSequenceNumber(void) {
	return op_parameter[OP_PARAMETER_SEQUENCE_NUMBER];
}

/**
 * Set one of the Operational Parameters
 *
 * Typically these are saved in a file on SD card and can be read to set operational
 * behaviour at boot time.
 *
 * Values are set to a default and can be over-written by reading from a file
 * or by the setter function.
 *
 * Values should be saved to the SD card before entering DPD
 *
 * @param parameter - one of a list of possible parameters
 * @param value - the value
 */
void fatfs_setOperationalParameter(OP_PARAMETERS_E parameter, int16_t value) {

	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES)) {
		op_parameter[parameter] = value;
	} else {
		// error
	}
}

/**
 * Increment one of teh state variables
 */
void fatfs_incrementOperationalParameter(OP_PARAMETERS_E parameter) {
	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES)) {
		// TODO - do we need to prevent roll-over?
		op_parameter[parameter]++;
	} else {
		// error
	}
}

/**
 * Load labels from SD card text file (public API)
 *
 * @param path Path to labels file
 * @param labels 2D array to store labels
 * @param label_count Pointer to store count
 * @param max_labels Maximum labels supported
 * @param max_label_len Maximum length per label
 * @return 0 on success, -1 on failure
 */
int8_t fatfs_load_labels(const char *path, char labels[][MAX_LABEL_LEN], uint8_t *label_count, uint8_t max_labels, uint8_t max_label_len) {
	return load_labels_from_sd(path, labels, label_count, max_labels, max_label_len);
}

#ifdef UNZIPMANIFEST
/**
 * Unzip Manifest.zip (public API)
 * Extracts manifest/labels.txt and manifest/MOD0000X.tfl files
 * Only supports method 0 (STORE - uncompressed)
 *
 * @return 0 on success, -1 on failure
 */
int fatfs_unzip_manifest(void) {
	return fatfs_unzip_manifest_zip();
}
#endif //UNZIPMANIFEST

/**
 * Reconstruct deployment ID UUID from operational parameters OP20-OP27
 * 
 * Algorithm per FIRMWARE_DEPLOYMENT_ID_SPEC.md:
 * 1. Read OP20-OP27 (8 uint16_t values)
 * 2. If all are 0, return "00000000-0000-0000-0000-000000000000"
 * 3. Convert each to 4-char hex string (with leading zeros)
 * 4. Insert hyphens at positions 8, 12, 16, 20 (UUID format)
 * 
 * @param deployment_id_buffer Output buffer (min 37 bytes for UUID + null)
 * @param buffer_size Size of output buffer
 */
void fatfs_getDeploymentId(char *deployment_id_buffer, size_t buffer_size) {
	if (buffer_size < 37) {
		// Buffer too small for UUID format
		deployment_id_buffer[0] = '\0';
		return;
	}
	
	// Read 8 chunks from OP20-OP27
	uint16_t chunks[8];
	bool all_zero = true;
	
	for (int i = 0; i < 8; i++) {
		chunks[i] = fatfs_getOperationalParameter(OP_PARAMETER_DEPLOYMENT_ID_CHUNK_1 + i);
		if (chunks[i] != 0) {
			all_zero = false;
		}
	}
	
	// All zeros = no deployment
	if (all_zero) {
		snprintf(deployment_id_buffer, buffer_size, "%s", DEPLOYMENT_ID_ZERO_UUID);
		return;
	}
	
	// Reconstruct UUID: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
	// Chunks map to: 01-2-3-4-567 (each chunk = 4 hex chars)
	snprintf(deployment_id_buffer, buffer_size,
			 "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
			 chunks[0], chunks[1],  // 8 chars
			 chunks[2],              // 4 chars
			 chunks[3],              // 4 chars
			 chunks[4],              // 4 chars
			 chunks[5], chunks[6], chunks[7]  // 12 chars
	);
}

/**
 * Prints the CWD
 *
 */
void fatfs_printCwd(void) {
	FRESULT res;
	char cur_dir[128]; //8.3? or full path?
	UINT len = 128;

	res = f_getcwd(cur_dir, len);      /* Get current directory */
	if (res) {
		xprintf("Error %d with f_getcwd\n", res);
	}
	else {
		xprintf("CWD is '%s'\n", cur_dir);
	}
}
