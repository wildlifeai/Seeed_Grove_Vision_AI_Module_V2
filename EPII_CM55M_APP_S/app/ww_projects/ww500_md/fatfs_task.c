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
 * These are probably supporting exFAT. The above ChatGPT conversation suggested I install fat32format.exe
 * from here: http://ridgecrop.co.uk/index.htm?fat32format.htm
 * This worked for me - formatted 64G cards as FAT32
 *
 */

/*************************************** Includes *******************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "WE2_device.h"
#include "WE2_debug.h"
#include "WE2_core.h"
#include "board.h"

#include "printf_x.h"
#include "xprintf.h"

#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

#include "fatfs_task.h"
#include "cis_file.h"
#include "image_task.h"
#include "app_msg.h"
#include "CLI-commands.h"
#include "ff.h"
#include "CLI-FATFS-commands.h"
#include "time_handling.h"

#include "hx_drv_rtc.h"
#include "exif_utc.h"
#include "ww500_md.h"
#include "inactivity.h"
#include "directory_manager.h"

#include "barrier.h"
#include "selfTest.h"
#include "cvapp.h"
#include "exif_gps.h"

// TODO this is for the default project id and version - move elsewhere?
#include "common_config.h"

/*************************************** Definitions *******************************************/

// TODO sort out how to allocate priorities
#define fatfs_task_PRIORITY (configMAX_PRIORITIES - 3)

#define FATFS_TASK_QUEUE_LEN 10

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
//FRESULT list_dir(const char *path);

static FRESULT load_configuration(const char *filename, directoryManager_t *dirManager);
FRESULT save_configuration(const char *filename, directoryManager_t *dirManager);

// ZIP and label handling functions (moved from cvapp.cpp)
static int8_t load_labels_from_sd(const char *path, char labels[][MAX_LABEL_LEN], uint8_t *label_count, uint8_t max_labels, uint8_t max_label_len);

#ifdef UNZIPMANIFEST
static int fatfs_unzip_manifest_zip(void);
int fatfs_unzip_manifest(void);
#endif //  UNZIPMANIFEST

/*************************************** External variables *******************************************/

// GPS location of device can be set from the app, then accessed when needed
extern GPS_Coordinate exif_gps_deviceLat;
extern GPS_Coordinate exif_gps_deviceLon;
extern GPS_Altitude exif_gps_deviceAlt;

extern directoryManager_t dirManager;
// Defined in if_task.c: true while a file-receive session is active. Suppresses the
// high-volume per-packet console logging that throttles the transfer at 921600 baud.
extern volatile bool g_fileRxActive;
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

// Persistent file handle for incremental file writes (OPEN_FILE / APPEND_FILE / CLOSE_FILE)
static FIL transferFile;
static bool transferFileOpen = false;

// Flush FAT metadata with f_sync() every N appends. Without this a long
// transfer accumulates unbounded dirty filesystem state (cluster chain and
// directory updates), which is the suspected cause of the non-deterministic
// f_write() failures ("ftx err 7") seen on transfers beyond ~3-7KB.
// 16 x 241-byte chunks ≈ 3.9KB between syncs; each f_sync costs ~50-100ms,
// amortised to a few ms per packet.
#define TRANSFER_WRITES_PER_SYNC 16
static uint16_t transferWritesSinceSync = 0;

static TickType_t xStartTime;
static TickType_t accumulatedTime;

/* Set non-zero while a fileWriteImage() call is in progress.
 * CLI task polls this before calling hx_drv_rtc_set_time(), which suppresses
 * ARM interrupts for ~1s and would otherwise starve the FatFS task mid-write. */
volatile int g_sdWriteActive = 0;

// Strings for each of these states. Values must match APP_TASK1_STATE_E in task1.h
const char *fatFsTaskStateString[APP_FATFS_STATE_NUMSTATES] = {
	"Uninitialised",
	"Idle",
	"Busy"
};

// Strings for expected messages. Values must match messages directed to fatfs Task in app_msg.h
const char *fatFsTaskEventString[APP_MSG_FATFSTASK_LAST - APP_MSG_FATFSTASK_WRITE_FILE] = {
	"Write file",
	"Write image",
	"Read file",
	"File op done",
	"Save State",
	"Open file",
	"Append file",
	"Close file",
};

// Number of pictures to take after motion detect wake
uint32_t numPicturesToGrab = NUMPICTURESTOGRAB;

// Interval between pictures (for now seconds, but let's change this to ms later
uint32_t pictureInterval = PICTUREINTERVAL;

// Values to read from the CONFIG.TXT file
uint16_t op_parameter[OP_PARAMETER_NUM_ENTRIES] = {
	0,				   // 0 Image file number (0 indicates no SD card)
	0,				   // 1 # times the NN model has run
	0,				   // 2 # times the NN model says "yes"
	0,				   // 3 # of AI processor cold boots
	0,				   // 4 # of AI processor warm boots
	NUMPICTURESTOGRAB, // 5 Num pics when triggered
	PICTUREINTERVAL,   // 6 Pic interval when triggered (ms)
	TIMELAPSEINTERVAL, // 7 Interval (s) (0 inhibits)
	INACTIVITYTIMEOUT, // 8 Delay before DPD (ms)
	FLASHLEDDUTY,	   // 9 in percent (0 inhibits)
	1,				   // 10 0 = camera disabled, 1 = enabled
	DPDINTERVAL,	   // 11 Interval (ms) between frames in MD mode (0 inhibits)
	FLASHDURATION,	   // 12 Duration (ms) that LED Flash is on
	0,				   // 13 LED bit mask: vis=1, IR=2, none=0
	PROJECT_ID,		   // 14 OP_PARAMETER_MODEL_PROJECT (0 disables NN)
	PROJECT_VER,	   // 15 OP_PARAMETER_MODEL_VERSION
	MODEL_THRESHOLD,   // 16 OP_PARAMETER_MODEL_THRESHOLD default
	1,      	   		// 17 Motion Detection Sensitivity: 0=off, 1=low, 2=medium, 3=high
	0,	    	   		// 18 Test Mode Bits - one bit to enable each test function
	0,	    	   		// 19 OP_PARAMETER_IMAGES_COUNT
	0,	    	   		// 20 OP_PARAMETER_IMAGES_COUNT - increment as files are added. Start a new folder when this exceeds a threhsold
	2,	    	   		// 21 OP_PARAMETER_MD_FLASH_LED (2 = IR)
	50,	    	   		// 22 OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT (STROBE-gated ~15ms pulses; 5% too dim in the field)
	65,	    	   		// 23 OP_PARAMETER_AE_DARK_THRESHOLD ('moderate' setting - see AE_Light_Sensor_Roadmap.md)
	15,	    	   		// 24 OP_PARAMETER_AE_CHECK_INTERVAL (minutes; 0 disables)
	0,	    	   		// 25 OP_PARAMETER_AE_FLASH_STATE (runtime state)
	1,	    	   		// 26 OP_PARAMETER_SLOT_SWITCH (0 = off/manual only; 1 = automatic light-based switching)
	286,	   			// 27 OP_PARAMETER_WB_RED_GAIN (Q8.8: 286 = x1.117, the bench-measured neutralising gain; 0 disables)
	326,	   			// 28 OP_PARAMETER_WB_BLUE_GAIN (Q8.8: 326 = x1.273, the bench-measured neutralising gain; 0 disables)
	1,	    	   		// 29 OP_PARAMETER_CAM_AE_ENABLE (RP camera auto-exposure on/off - see ae.c)
	110,	   			// 30 OP_PARAMETER_CAM_AE_TARGET (target mean luma; 0 = built-in default)
	1,	    	   		// 31 OP_PARAMETER_CAM_WB_MODE (1 = auto grey-world; 2 = manual op27/28; 0 = off)
	0,	    	   		// 32 OP_PARAMETER_CAM_RESOLUTION (0 = 640x480; 1 = 1280x960, needs op14 = 0)
};

// Deployment ID UUID string — loaded from 'I ' line in CONFIG.TXT or set via setdid CLI command
static char deployment_id_string[UUIDLENGTH] = DEPLOYMENT_ID_ZERO_UUID;


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
	}
	else {
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
static FRESULT fileWriteImage(fileOperation_t *fileOp, fileBufferInfo_t * extraBlock, directoryManager_t *dirManager) {
	FRESULT res;
	UINT bw;         	// Bytes written
	UINT bwTotal;
	rtc_time time;

	// Guard: capture dir must be set. An empty string causes f_chdir("") to silently
	// leave the CWD unchanged (wherever it was — often /MANIFEST after load_configuration).
	if (dirManager->current_capture_dir[0] == '\0') {
		xprintf("fileWriteImage: capture directory not set\n");
		return FR_NO_PATH;
	}

	// Guard: if a previous write leaked an open file, close it before opening a new one.
	// Without this, f_open would overwrite the FIL object without releasing the FatFS
	// lock-table slot, eventually producing FR_TOO_MANY_OPEN_FILES.
	if (dirManager->imagesOpen) {
		xprintf("fileWriteImage: closing stale open file\n");
		f_close(&dirManager->imagesFile);
		dirManager->imagesOpen = false;
	}

	// (1) Change to the image capture directory
	res = f_chdir(dirManager->current_capture_dir);
	if (res != FR_OK) {
		return res;
	}

	// (2) Open the file
	res = f_open(&dirManager->imagesFile, (TCHAR*) fileOp->fileName,  FA_WRITE | FA_CREATE_ALWAYS);
	dirManager->imagesRes = res;

	if (res != FR_OK)  {
		xprintf("f_open of '%s' failed. res = %d\r\n", fileOp->fileName, res);
		return res;
	}
	dirManager->imagesOpen = true;

    // This ensures that any data in the D-cache is committed to RAM
    SCB_CleanDCache_by_Addr ((void *)fileOp->buffer, fileOp->length);

    // (3a) Write (first chunk of) data to the file
    res = f_write(&dirManager->imagesFile, (void *)fileOp->buffer, fileOp->length, &bw);

	if (res != FR_OK) {
		xprintf("Error writing file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		f_close(&dirManager->imagesFile);
		dirManager->imagesOpen = false;
		return res;
	}
	bwTotal = bw;

	// (3b) Write extra data to the file (extraBlock may be NULL when called from WRITE_FILE)
	if (extraBlock != NULL && extraBlock->length > 0) {

	    SCB_CleanDCache_by_Addr ((void *)extraBlock->buffer, extraBlock->length);
		res = f_write(&dirManager->imagesFile, (void *)extraBlock->buffer, extraBlock->length, &bw);

		if (res != FR_OK) {
			xprintf("Error writing file %s\n", fileOp->fileName);
			fileOp->length = 0;
			fileOp->res = res;
		}
		else {
			bwTotal += bw;
		}
	}

	// (4) Close the file (f_close calls f_sync which calls CTRL_SYNC — card confirmed idle on return)
     res = f_close(&dirManager->imagesFile);
     dirManager->imagesRes = res;

    if (res != FR_OK) {
        xprintf("Failed to close image file: %d\n", res);
    }

    dirManager->imagesOpen = false;

	XP_GREEN
	xprintf("Wrote %d byte image to SD: %s ", bwTotal, fileOp->fileName);
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

	case APP_MSG_FATFSTASK_WRITE_IMAGE:
		// Deliberately fall through
	case APP_MSG_FATFSTASK_WRITE_FILE:
		// Someone wants a file written. Send back an error message

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;
		sendMsg.message.msg_parameter = (uint32_t)fileOp;
		sendMsg.destination = fileOp->senderQueue;

		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		}
		else if (sendMsg.destination == xImageTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		}
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
		// Return an error in this state

		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;

		// Signal to the caller that it may enter DPD.
		sendMsg.destination = xImageTaskQueue; // fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		sendMsg.message.msg_data = (uint32_t)res;

		break;

	case APP_MSG_FATFSTASK_OPEN_FILE:
	case APP_MSG_FATFSTASK_APPEND_FILE:
	case APP_MSG_FATFSTASK_CLOSE_FILE:
		// SD card not mounted — report failure
		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;
		sendMsg.destination = fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
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
	static APP_MSG_DEST_T sendMsg;
	static TickType_t elapsedTime;

	fileOperation_t *fileOp;
	fileBufferInfo_t *extraBlock;

	sendMsg.destination = NULL;
	res = FR_OK;

	event = rxMessage.msg_event;

	fileOp = (fileOperation_t *)rxMessage.msg_data;

	extraBlock = (fileBufferInfo_t*)rxMessage.msg_parameter;

	switch (event) {

	case APP_MSG_FATFSTASK_WRITE_IMAGE:
		// someone wants a file written. Structure including file name a buffer is passed in data

		if (fileOp->fileName == NULL) {
			// Skip the actual file write operation, but send an OK response.
			// This allows the calling code to say "don't really save a file" but all the other code can stay unchnaged
			res = FR_OK;
		}
		else {
			xStartTime = xTaskGetTickCount();

			g_sdWriteActive = 1;
			res = fileWriteImage(fileOp, extraBlock, &dirManager);
			g_sdWriteActive = 0;
			fatfs_incrementOperationalParameter(OP_PARAMETER_IMAGES_COUNT);

			elapsedTime = app_getElapsedMs(xStartTime);
			accumulatedTime += elapsedTime;		// add these all together so we can average them at the end.

			XP_YELLOW;	// Make this stand out while investigation SD card speed
			xprintf("File write took %dms\n", elapsedTime);
			XP_WHITE;
		}

		// Inform the if task that the disk operation is complete
		sendMsg.destination = fileOp->senderQueue;
		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.message.msg_parameter = accumulatedTime;	// Lets image task calculate the average write time

		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		// In practise, at the time of writing, image fiels are written from image_task.
		// There is a CLI-commands command but (probably) not used.

		if (sendMsg.destination == xIfTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		}
		else if (sendMsg.destination == xImageTaskQueue) {
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		}
		else {
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_WRITE_COMPLETE;
		}

		break;

	case APP_MSG_FATFSTASK_WRITE_FILE:
		// TODO - re-write this!
		// someone wants a file written. Structure including file name a buffer is passed in data
		//fatFs_task_state = APP_FATFS_STATE_BUSY;
		xStartTime = xTaskGetTickCount();

		if (fileOp->senderQueue == xImageTaskQueue) {
			// writes image
			g_sdWriteActive = 1;
			res = fileWriteImage(fileOp, NULL, &dirManager);
			g_sdWriteActive = 0;
		} else {
			// writes file
			g_sdWriteActive = 1;
			res = fileWrite(fileOp);
			g_sdWriteActive = 0;
		}

		xprintf("File write took %dms\n", app_getElapsedMs(xStartTime));

		// Inform the if task that the disk operation is complete.
		// msg_parameter echoes the fileOperation_t pointer so a receiver with
		// several outstanding operations can tell which one completed.
		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.message.msg_parameter = (uint32_t)fileOp;
		sendMsg.destination = fileOp->senderQueue;

		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		// In practise, at the time of writing, image fiels are written from image_task.
		// There is a CLI-commands command but (probably) not used.

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

		xprintf("Elapsed time (fileRead) %dms. Result code %d\n",
				app_getElapsedMs(xStartTime), res);

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

		// Close any file transfer that was interrupted (e.g. BLE disconnect before FILE_END).
		if (transferFileOpen) {
			xprintf("WARNING: incomplete file transfer — closing before DPD");
			f_close(&transferFile);
			transferFileOpen = false;
		}

		if (fatfs_mounted()) {
			res = save_configuration(STATE_FILE, &dirManager);
			f_unmount(DRV);

			if (res) {
				xprintf("Error %d saving state\n", res);
			}
			else {
				xprintf("Saved state to SD card. Image sequence number = %d\n",
						fatfs_getImageSequenceNumber());
			}
		}

		// Signal to the caller that it may enter DPD.
		sendMsg.destination = xImageTaskQueue; // fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		sendMsg.message.msg_data = (uint32_t)res;

		break;

	case APP_MSG_FATFSTASK_SAVE_CONFIG:
		// Persist the Operational Parameters to CONFIG.TXT immediately, WITHOUT
		// unmounting (unlike SAVE_STATE). Sent after a setop / BLE parameter
		// change so the new value survives the next sleep even if no capture
		// (which is what normally triggers SAVE_STATE) happens before DPD.
		// Without this, a changed op param (e.g. a white-balance gain or the
		// timelapse interval) is lost on the next wake.
		if (fatfs_mounted()) {
			res = save_configuration(STATE_FILE, &dirManager);
			if (res) {
				xprintf("Error %d saving config\n", res);
			}
			else {
				xprintf("Config saved (op params persisted).\n");
			}
		}
		else {
			xprintf("Cannot save config - SD not mounted\n");
		}
		break;

	case APP_MSG_FATFSTASK_OPEN_FILE:
		// 1/3 commands for sending files from the app to the SD card

		// Open (create/truncate) a file for incremental writing.
		// The file handle is kept open until APP_MSG_FATFSTASK_CLOSE_FILE.
		if (transferFileOpen) {
			// Stale handle from a previous aborted transfer — close it first.
			f_close(&transferFile);
			transferFileOpen = false;
		}

		// fileOp->fileName is a bare 8.3 name; resolve it in the config directory.
		res = f_chdir(dirManager.current_config_dir);
		if (res != FR_OK) {
			xprintf("Failed to chdir to '%s' (err %d)\n", dirManager.current_config_dir, res);
		}
		else {
			res = f_open(&transferFile, fileOp->fileName, FA_WRITE | FA_CREATE_ALWAYS);
		}

		if (res == FR_OK) {
			// FA_CREATE_ALWAYS truncated any existing file, freeing its cluster
			// chain. Commit that (and let the card finish the internal
			// housekeeping) BEFORE the first data write - otherwise re-sending a
			// large file failed on packet 1 with FR_DISK_ERR (ftx err 7) because
			// the card was still busy from freeing ~1000 clusters.
			f_sync(&transferFile);
			transferFileOpen = true;
			transferWritesSinceSync = 0;
			xprintf("Opened '%s' for writing\n", fileOp->fileName);
		}
		else {
			xprintf("Failed to open '%s' (err %d)\n", fileOp->fileName, res);
		}

		fileOp->res = res;

		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;

		break;

	case APP_MSG_FATFSTASK_APPEND_FILE: {
		// 2/3 commands for sending files from the app to the SD card

		// Append a chunk to the currently open transfer file.
		UINT bw = 0;

		if (!transferFileOpen) {
			res = FR_INVALID_OBJECT;
		}
		else {
			// Retry transient SD write failures: the card can be briefly busy
			// (internal garbage collection, or recovering after a large-file
			// truncate), which returns FR_DISK_ERR. A short delay lets it
			// recover; only a persistent failure becomes ftx err 7.
			for (int attempt = 0; ; attempt++) {
				res = f_write(&transferFile, fileOp->buffer, fileOp->length, &bw);
				if (res == FR_OK && bw != fileOp->length) {
					// FR_OK with a short count means the volume is full. Do NOT
					// retry: the file pointer has already advanced by bw, so a
					// rewrite of the full buffer would duplicate those bytes and
					// corrupt the file. Report it as a write error (ftx err 7).
					xprintf("Short write: %u of %lu bytes (volume full?)\n", bw, fileOp->length);
					res = FR_DISK_ERR;
					break;
				}
				if (res == FR_OK || attempt >= 3) {
					break;
				}
				xprintf("SD write err %d, retry %d/3\n", res, attempt + 1);
				vTaskDelay(pdMS_TO_TICKS(15));
			}

			// Periodic metadata flush — see TRANSFER_WRITES_PER_SYNC above.
			// A failed sync is reported like a failed write (ftx err 7).
			if (res == FR_OK && ++transferWritesSinceSync >= TRANSFER_WRITES_PER_SYNC) {
				res = f_sync(&transferFile);
				transferWritesSinceSync = 0;
				if (res != FR_OK) {
					xprintf("f_sync failed (err %d)\n", res);
				}
			}
		}

		fileOp->res = res;

		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;

		break;
	}

	case APP_MSG_FATFSTASK_CLOSE_FILE:
		// 3/3 commands for sending files from the app to the SD card

		// Close the transfer file.  If deleteOnClose is set, delete it afterwards.
		if (transferFileOpen) {
			res = f_close(&transferFile);
			transferFileOpen = false;
		}
		else {
			res = FR_OK;
		}

		if (fileOp->deleteOnClose && fileOp->fileName != NULL) {
			FRESULT delRes = f_unlink(fileOp->fileName);
			if (delRes != FR_OK) {
				xprintf("Warning: failed to delete '%s' (err %d)\n", fileOp->fileName, delRes);
			}
			else {
				xprintf("Deleted '%s'\n", fileOp->fileName);
			}
		}

		fileOp->res = res;

		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;

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
 * Process any GPS string in the CONFIG.TXT file
 *
 * Parses the string and sets exif_gps_deviceLat, exif_gps_deviceLon, exif_gps_deviceAlt
 *
 * The location can be entered manually in CONFIG.TXT but is normally written there
 * before the processor enters DPD.
 *
 * The code was created by Claude which suggested the following test strings
 * for test other formats:
 *
Seems to be working. I can manually edit the file to test the system reads and write the GPS string - example:
G 1°2'3" N 4°5'6" E 7 Above

Can you suggest a few more strings that can be used to test other string formats, including decimal values.11:57Here are some test strings covering the important variations:
G 36°49'55" S 174°47'51" E 31 Above
G 36°49'55.5" S 174°47'51.8" E 31.2 Above
G 36°49'55.68" S 174°47'51.83" E 31.234 Above
G 51°30'26.123" N 0°7'39.456" W 11.5 Above
G 90°0'0" S 180°0'0" W 0 Above
G 0°0'0" N 0°0'0" E 8848.86 Above
G 27°59'17.28" N 86°55'30.12" E 8848.86 Above
G 35°41'22.2" N 139°41'30.5" E 40 Below
The reasoning behind each:

Line 1–3: Auckland roughly, progressively adding decimal places to seconds — good for checking the fractional parsing at different precisions
Line 4: London — tests a near-zero longitude (single digit degrees, W direction)
Line 5: Extreme corner case — maximum lat/lon values, all zeros for seconds
Line 6: Null Island (0,0) with a non-zero altitude — isolates altitude parsing from coordinate parsing
Line 7: Mount Everest summit — realistic high altitude with decimal seconds in both coordinates
Line 8: Tests Below sea level, e.g. Dead Sea area — the only case that exercises the alt->ref == 1 path

@param line = string read from the file (after the "G " characters)
*/
static void processGPS(char * gps_line) {
	exif_gps_parse_full_string(&exif_gps_deviceLat, &exif_gps_deviceLon,
	                           &exif_gps_deviceAlt, gps_line);

	// test that worked:
	// xprintf("Read GPS location from file: '%s'\n", gps_line);
}

/**
 * Loads configuration information from a file.
 *
 * The file comprises several lines each with two integers.
 * The first integer is an index into the configuration[] array.
 * The second integer is the value to place into the array.
 *
 * There is a special case for the GPS location:
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

			// Special case if the first 2 characters are 'G ' for GPS
			if ((line[0] == 'G') && (line[1] == ' ')) {
				processGPS(&line[2]);
			}
			// Special case if the first 2 characters are 'I ' for deployment ID
			else if ((line[0] == 'I') && (line[1] == ' ')) {
				strncpy(deployment_id_string, &line[2], UUIDLENGTH - 1);
				deployment_id_string[UUIDLENGTH - 1] = '\0';
				char *nl = strchr(deployment_id_string, '\n');
				if (nl) {
					*nl = '\0';
				}
				for (char *p = deployment_id_string; *p; p++) {
					*p = tolower((unsigned char)*p);
				}
			}
			else {

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

		// Write the GPS location
		line[0] = 'G';
		line[1] = ' ';	// initialise with "G "
		// Write the GPS string as the next characters
		exif_gps_create_full_string(&exif_gps_deviceLat, &exif_gps_deviceLon,
		                            &exif_gps_deviceAlt, &line[2], sizeof(line) - 2);
		// Append newline (exif_gps_create_full_string does not add one)
		size_t gps_len = strlen(line);
		if (gps_len < sizeof(line) - 1) {
			line[gps_len]     = '\n';
			line[gps_len + 1] = '\0';
		}

		//xprintf("Wrote GPS location to file: '%s'\n", line);

		f_write(&dirManager->configFile, line, strlen(line), &bytesWritten);

		// Write the deployment ID
		snprintf(line, sizeof(line), "I %s\n", deployment_id_string);
		f_write(&dirManager->configFile, line, strlen(line), &bytesWritten);

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

    accumulatedTime = 0;	// we will aggregate file write times so we can average them at the end

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting FatFS Task\n");
	XP_WHITE;

	// One-off initialisation here...
	startTime = xTaskGetTickCount();

	// Initialise GPS coordinates before traying to process them
	exif_gps_init_defaults();

	// TODO - experiment - do I need settling time for 3V3_WE?
	vTaskDelay(pdMS_TO_TICKS(10));
	res = fatFsInit();

	if (res == FR_OK) {
		fatFs_task_state = APP_FATFS_STATE_IDLE;
		// Only if the file system is working should we add CLI commands for FATFS
		cli_fatfs_init();

		// Phase 1: ensure /MANIFEST exists and initialise dirManager state.
		res = dir_mgr_init_config(&dirManager);

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
			}
			else {
				xprintf("'%s' NOT found.\r\n", STATE_FILE);
			}
			// Phase 2: now that op_parameter[] and deployment ID are valid,
			// determine and create the correct image directory.
			dir_mgr_init_image_dir(&dirManager);
		}
	}
	else {
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

	// Load the staged camera register table (camreg command) from the SD card
	// while this task is still the only one doing disk I/O - FatFs is not
	// re-entrant, so this cannot be done lazily from the CLI task
	cis_file_loadStagedFromFile();

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

			if (!g_fileRxActive) {
				XP_LT_CYAN
				xprintf("\nFatFS Task ");
				XP_WHITE;
				xprintf("received event '%s' (0x%04x). Rx data = 0x%08x\r\n", eventString, event, rxData);
			}

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

			if ((old_state != fatFs_task_state) && !g_fileRxActive) {
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
				} else if (!g_fileRxActive) {
					xprintf("FatFS task sending event 0x%04x. Tx data = 0x%08x\r\n", sendMsg.msg_event, sendMsg.msg_data);
				}
			}
		}
	} // for(;;)
}

/********************************** Public Function definitions - fatfs port  ***************************/

/**
 * Control of SD card chip select...
 *
 * Himax notes:
 * app implement GPIO_Output_Level/GPIO_Pinmux/GPIO_Dir for fatfs\port\mmc_spi\mmc_we2_spi.c ARM_SPI_SS_MASTER_SW
 *
 * CGP - could it be that the SSPI_CS_GPIO_Pinmux() sets PB5 to be either a GPIO pin (and "GPIO16" for that matter)
 * then the other two functions set the pin high or low, and input or output.
 *
 * This is used by mmc_we2_spi.c in the fatfs code.
 */
void SSPI_CS_GPIO_Pinmux(bool setGpioFn) {
    if (setGpioFn) {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_GPIO16, 0);
    }
    else {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_M_CS_1, 0);
    }
}

void SSPI_CS_GPIO_Output_Level(bool setLevelHigh) {
    hx_drv_gpio_set_out_value(GPIO16, (GPIO_OUT_LEVEL_E) setLevelHigh);
}

void SSPI_CS_GPIO_Dir(bool setDirOut) {
    if (setDirOut) {
        hx_drv_gpio_set_output(GPIO16, GPIO_OUT_HIGH);
    }
    else {
        hx_drv_gpio_set_input(GPIO16);
    }
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
	}
	else {
		return 0;
	}
}

/**
 * Get the image sequence number
 *
 * Short-hand version of fatfs_getOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER)
 *
 * If tis returns 0 then that means there is no SD card, so it is a way of skipping
 * file write attempts.
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
	}
	else {
		// error
		xprintf("Operational parameter index %d out of range\n", parameter);
	}
}

/**
 * Increment one of teh state variables
 */
void fatfs_incrementOperationalParameter(OP_PARAMETERS_E parameter) {
	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES)) {
		// TODO - do we need to prevent roll-over?
		op_parameter[parameter]++;
	}
	else {
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
 * Get the deployment ID UUID string.
 *
 * Expects string form loaded from the 'I ' line in CONFIG.TXT or set via
 * fatfs_setDeploymentId().
 *
 * @param deployment_id_buffer Output buffer (min UUIDLENGTH bytes)
 * @param buffer_size Size of output buffer
 */
void fatfs_getDeploymentId(char *deployment_id_buffer, size_t buffer_size) {

	deployment_id_buffer[0] = '\0';

	if (buffer_size < UUIDLENGTH) {
		return;
	}

	snprintf(deployment_id_buffer, buffer_size, "%s", deployment_id_string);
}

/**
 * Set the deployment ID UUID string.
 *
 * The value is stored in RAM and written to the 'I ' line in CONFIG.TXT the
 * next time save_configuration() is called.
 *
 * @param uuid_string UUID string in standard format (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
 */
void fatfs_setDeploymentId(const char *uuid_string) {
	strncpy(deployment_id_string, uuid_string, UUIDLENGTH - 1);
	deployment_id_string[UUIDLENGTH - 1] = '\0';

	// Make lower-case
	for (char *p = deployment_id_string; *p; p++) {
		*p = tolower((unsigned char)*p);
	}
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

/**
 * @brief Create a directory path recursively (mkdir -p style).
 *
 * This function ensures that the full directory path exists by creating
 * each intermediate directory level as required. It is intended for use
 * with FatFs, where f_mkdir() can only create a single directory level
 * and will fail with FR_NO_PATH if parent directories do not exist.
 *
 * Example:
 *   Input:  "/MEDIA/ABC12345/IMAGES.000"
 *   Operation: Creates "/MEDIA", then "/MEDIA/ABC12345", then
 *           "/MEDIA/ABC12345/IMAGES.000" as needed.
 *
 * Existing directories are not treated as an error (FR_EXIST is ignored).
 *
 * @param path  Null-terminated string containing the full directory path.
 *
 * @return
 *   FR_OK        All required directories exist or were created successfully.
 *   != FR_OK     Error returned by f_mkdir() for the first failing level.
 *
 * @note
 *   - The input path buffer is not modified.
 *   - Requires DIRNAMELEN to be large enough for a copy of the path.
 *   - Uses '/' as the path separator.
 */
FRESULT fatfs_mkdir_recursive(const char *path) {
    FRESULT res;
    char tmp[DIRNAMELEN];
    char *p = NULL;

    if (strlen(path) >= sizeof(tmp)) {
        return FR_INVALID_NAME;
    }
    strcpy(tmp, path);

    // Walk the path string and create each intermediate directory level.
    // We start at tmp+1 to skip the leading '/' so that we only trigger
    // directory creation when we reach a separator between valid path components.
    // At each '/', the string is temporarily truncated and f_mkdir() is called
    // on the partial path, then restored before continuing.
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';

            res = f_mkdir(tmp);
            if (res != FR_OK && res != FR_EXIST) {
                return res;
            }

            *p = '/';
        }
    }

    // Create final directory
    res = f_mkdir(tmp);
    if (res != FR_OK && res != FR_EXIST) {
        return res;
    }

    return FR_OK;
}

