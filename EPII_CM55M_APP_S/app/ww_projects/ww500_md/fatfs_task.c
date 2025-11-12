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
#include "cvapp.h"

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
static FRESULT save_configuration(const char *filename, directoryManager_t *dirManager);

// ZIP and label handling functions (moved from cvapp.cpp)
static int fatfs_load_labels_from_sd(const char *path, char labels[][48], int *label_count, int max_labels, int max_label_len);
static int fatfs_unzip_manifest_zip(void);
int fatfs_unzip_manifest(void);

/*************************************** External Function Declaraions *******************************************/

extern FRESULT init_directories(directoryManager_t *dirManager);
extern FRESULT add_capture_folder(directoryManager_t *dirManager);

/*************************************** External variables *******************************************/

extern directoryManager_t dirManager;
extern QueueHandle_t xIfTaskQueue;
extern QueueHandle_t xImageTaskQueue;

/*************************************** Local variables *******************************************/

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
	"Busy"};

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

// Values to read from the configuration.txt file
uint16_t op_parameter[OP_PARAMETER_NUM_ENTRIES];

/********************************** Private Function definitions  *************************************/

/** Another task asks us to write a file for them
 *
 */
static FRESULT fileWrite(fileOperation_t *fileOp)
{
	FIL fdst;	 // File object
	FRESULT res; // FatFs function common result code
	UINT bw;	 // Bytes written

	// TODO omit this soon as it might not handle long files or binary files
	xprintf("DEBUG: writing %d bytes to '%s' from address 0x%08x. Contents:\n%s\n",
			fileOp->length, fileOp->fileName, fileOp->buffer, fileOp->buffer);

	res = f_open(&fdst, fileOp->fileName, FA_WRITE | FA_CREATE_ALWAYS);
	if (res)
	{
		xprintf("Fail opening file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		return res;
	}

	res = f_write(&fdst, fileOp->buffer, fileOp->length, &bw);
	if (res)
	{
		xprintf("Fail writing to file %s\n", fileOp->fileName);
		fileOp->length = bw;
		fileOp->res = res;
		return res;
	}

	// TODO experimental:leave file open so it can be appended? TODO need to make stuff static?
	if (fileOp->closeWhenDone)
	{
		res = f_close(&fdst);

		if (res)
		{
			xprintf("Fail closing file %s\n", fileOp->fileName);
			fileOp->length = bw;
			fileOp->res = res;
			return res;
		}
	}

	if (bw != (fileOp->length))
	{
		xprintf("Error. Wrote %d bytes rather than %d\n", bw, fileOp->length);
		res = FR_DISK_ERR; // TODO find a better error code? Disk full?
	}
	else
	{
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
 * 		parameters: fileOperation_t fileOp
 * 		returns: FRESULT res
 */
static FRESULT fileWriteImage(fileOperation_t *fileOp, directoryManager_t *dirManager)
{
	FRESULT res;
	rtc_time time;
	res = f_chdir(dirManager->current_capture_dir);
	if (res != FR_OK)
		return res;

	// fastfs_write_image() expects filename is a uint8_t array
	// TODO resolve this warning! "warning: passing argument 1 of 'fastfs_write_image' makes integer from pointer without a cast"
	res = fastfs_write_image((uint32_t)(fileOp->buffer), fileOp->length, (uint8_t *)fileOp->fileName, dirManager);
	if (res != FR_OK)
	{
		xprintf("Error writing file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		return res;
	}

	XP_GREEN
	xprintf("Wrote image to SD: %s ", fileOp->fileName);
	XP_WHITE;

	exif_utc_get_rtc_as_time(&time);

	xprintf("at %d:%d:%d %d/%d/%d\n",
			time.tm_hour, time.tm_min, time.tm_sec,
			time.tm_mday, time.tm_mon, time.tm_year);

	// Restore base dir
	res = f_chdir(dirManager->base_dir);
	return res;
}

/** Another task asks us to read a file for them
 *
 */
static FRESULT fileRead(fileOperation_t *fileOp)
{
	FIL fsrc;	 // File object
	FRESULT res; // FatFs function common result code
	UINT br;	 // Bytes read

	//	xprintf("DEBUG: reading file %s to buffer at address 0x%08x (%d bytes)\n",
	//			fileOp->fileName, fileOp->buffer, fileOp->length);

	res = f_open(&fsrc, fileOp->fileName, FA_READ);
	if (res)
	{
		xprintf("Fail opening file %s\n", fileOp->fileName);
		fileOp->length = 0;
		fileOp->res = res;
		return res;
	}

	// Read a chunk of data from the source file
	res = f_read(&fsrc, fileOp->buffer, fileOp->length, &br);

	// TODO experimental: leave file open so it can be appended? TODO need to make stuff static?
	if (fileOp->closeWhenDone)
	{
		res = f_close(&fsrc);

		if (res)
		{
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
static APP_MSG_DEST_T handleEventForUninit(APP_MSG_T rxMessage)
{
	APP_MSG_EVENT_E event;
	FRESULT res;
	fileOperation_t *fileOp;
	static APP_MSG_DEST_T sendMsg;

	sendMsg.destination = NULL;
	res = FR_OK;

	event = rxMessage.msg_event;

	fileOp = (fileOperation_t *)rxMessage.msg_data;

	switch (event)
	{

	case APP_MSG_FATFSTASK_WRITE_FILE:
		// someone wants a file written. Send back an error message

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)FR_NO_FILESYSTEM;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		}
		else if (sendMsg.destination == xImageTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_WRITE_COMPLETE;
		//    	}
		else
		{
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
		if (sendMsg.destination == xIfTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_READ_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_READ_COMPLETE;
		//    	}
		else
		{
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
static APP_MSG_DEST_T handleEventForIdle(APP_MSG_T rxMessage)
{
	APP_MSG_EVENT_E event;
	FRESULT res;
	fileOperation_t *fileOp;
	static APP_MSG_DEST_T sendMsg;

	sendMsg.destination = NULL;
	res = FR_OK;

	event = rxMessage.msg_event;

	fileOp = (fileOperation_t *)rxMessage.msg_data;

	switch (event)
	{

	case APP_MSG_FATFSTASK_WRITE_FILE:
		// someone wants a file written. Structure including file name a buffer is passed in data
		fatFs_task_state = APP_FATFS_STATE_BUSY;
		xStartTime = xTaskGetTickCount();

		if (fileOp->senderQueue == xImageTaskQueue)
		{
			// writes image
			res = fileWriteImage(fileOp, &dirManager);
		}
		else
		{
			// writes file
			res = fileWrite(fileOp);
		}

		xprintf("File write from 0x%08x took %dms\n",
				fileOp->buffer,
				(xTaskGetTickCount() - xStartTime) * portTICK_PERIOD_MS);

		fatFs_task_state = APP_FATFS_STATE_IDLE;

		// Inform the if task that the disk operation is complete
		sendMsg.message.msg_data = (uint32_t)res;
		sendMsg.destination = fileOp->senderQueue;
		// The message to send depends on the destination! In retrospect it would have been better
		// if the messages were grouped by the sender rather than the receiver, so this next test was not necessary:
		if (sendMsg.destination == xIfTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_WRITE_COMPLETE;
		}
		else if (sendMsg.destination == xImageTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IMAGETASK_DISK_WRITE_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_WRITE_COMPLETE;
		//    	}
		else
		{
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
		if (sendMsg.destination == xIfTaskQueue)
		{
			sendMsg.message.msg_event = APP_MSG_IFTASK_DISK_READ_COMPLETE;
		}
		//    	// Complete this as necessary
		//    	else if (sendMsg.destination == anotherTaskQueue) {
		//        	sendMsg.message.msg_event = APP_MSG_ANOTHERTASK_DISK_READ_COMPLETE;
		//    	}
		else
		{
			// assumed to be CLI task.
			sendMsg.message.msg_event = APP_MSG_CLITASK_DISK_READ_COMPLETE;
		}

		break;

	case APP_MSG_FATFSTASK_SAVE_STATE:
		// Save the state of the imageSequenceNumber
		// This is the last thing we will do before sleeping.
		if (fatfs_getOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER) > 0)
		{
			res = save_configuration(STATE_FILE, &dirManager);
			f_unmount(DRV);

			if (res)
			{
				xprintf("Error %d saving state\n", res);
			}
			else
			{
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
static APP_MSG_DEST_T handleEventForBusy(APP_MSG_T rxMessage)
{
	APP_MSG_EVENT_E event;
	// uint32_t data;
	APP_MSG_DEST_T sendMsg;
	sendMsg.destination = NULL;

	event = rxMessage.msg_event;
	// data = rxMessage.msg_data;

	switch (event)
	{

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
static APP_MSG_DEST_T flagUnexpectedEvent(APP_MSG_T rxMessage)
{
	APP_MSG_EVENT_E event;
	APP_MSG_DEST_T sendMsg;
	sendMsg.destination = NULL;

	event = rxMessage.msg_event;

	XP_LT_RED;
	if ((event >= APP_MSG_IFTASK_FIRST) && (event < APP_MSG_IFTASK_LAST))
	{
		xprintf("FatFS Task unhandled event '%s' in '%s'\r\n", fatFsTaskEventString[event - APP_MSG_IFTASK_FIRST], fatFsTaskStateString[fatFs_task_state]);
	}
	else
	{
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
static FRESULT fatFsInit(void)
{
	FRESULT res;

	XP_CYAN;
	xprintf("Mounting FatFS on SD card ");
	XP_WHITE;

	// This is probably blocking...
	res = f_mount(&fs, DRV, 1);

	if (res)
	{
		XP_RED;
		xprintf("Failed error = %d\r\n", res);
		XP_WHITE;
		mounted = false;
	}
	else
	{
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
static FRESULT load_configuration(const char *filename, directoryManager_t *dirManager)
{
	FRESULT res;
	char line[64];
	char *token;
	uint8_t index;
	uint16_t value;

	if (!fatfs_mounted())
	{
		xprintf("SD card not mounted.\n");
		return FR_NO_FILESYSTEM;
	}

	if (!dirManager->configOpen)
	{
		res = f_chdir(dirManager->current_config_dir);
		if (res != FR_OK)
			return res;

		// Open the file
		res = f_open(&dirManager->configFile, filename, FA_READ);
		if (res != FR_OK)
		{
			printf("Failed to open config file: %d\n", res);
			dirManager->configRes = res;
			return dirManager->configRes;
		}
		dirManager->configOpen = true;

		// Read lines from the file
		while (f_gets(line, sizeof(line), &dirManager->configFile))
		{
			// Remove trailing newline if present
			char *newline = strchr(line, '\n');
			if (newline)
			{
				*newline = '\0';
			}

			// Skip comments which start with #
			if (line[0] == '#')
			{
				continue;
			}

			// The first call to strtok() should have a pointer to the string which
			// should be split, while any following calls should use NULL as an
			// argument. Each time the function is called a pointer to a different
			// token is returned until there are no more tokens.
			// At that point each function call returns NULL.
			token = strtok(line, " ");
			if (token == NULL)
			{
				continue;
			}

			index = (uint8_t)atoi(token);

			token = strtok(NULL, " ");
			if (token == NULL)
			{
				continue;
			}

			value = (uint16_t)atoi(token);

			// Set array value if index is in range
			if (index >= 0 && index < OP_PARAMETER_NUM_ENTRIES)
			{
				op_parameter[index] = value;
				// xprintf("   op_parameter[%d] = %d\n", index, value);
			}
		}
	}

	// Close file
	res = f_close(&dirManager->configFile);
	if (res != FR_OK)
	{
		printf("Failed to close config file: %d\n", res);
	}
	else
	{
		dirManager->configOpen = false;
	}
	dirManager->configRes = res;
	// Restore the original directory
	f_chdir(dirManager->base_dir);

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
static FRESULT save_configuration(const char *filename, directoryManager_t *dirManager)
{
	FRESULT res;
	UINT bytesWritten;
	char line[MAXCOMMENTLENGTH];
	char comment_lines[MAXNUMCOMMENTS][MAXCOMMENTLENGTH];
	uint16_t comment_count = 0;

	if (!fatfs_mounted())
	{
		xprintf("SD card not mounted.\n");
		return FR_NO_FILESYSTEM;
	}

	if (!dirManager->configOpen)
	{
		res = f_chdir(dirManager->current_config_dir);
		if (res != FR_OK)
			return res;

		// --- First Pass: Try to read existing comment lines ---
		res = f_open(&dirManager->configFile, filename, FA_READ);
		if (res == FR_OK)
		{
			dirManager->configOpen = true;
			while (f_gets(line, sizeof(line), &dirManager->configFile))
			{
				if (line[0] == '#')
				{
					if (comment_count < MAXNUMCOMMENTS)
					{
						strncpy(comment_lines[comment_count], line, MAXCOMMENTLENGTH);
						comment_lines[comment_count][MAXCOMMENTLENGTH - 1] = '\0';
						comment_count++;
					}
					else
					{
						break;
					}
				}
			}
			res = f_close(&dirManager->configFile);
			if (res != FR_OK)
			{
				printf("Failed to close config file: %d\n", res);
			}
			else
			{
				dirManager->configOpen = false;
			}
		}
		else if (res != FR_NO_FILE)
		{
			dirManager->configRes = res;
			return res; // Error reading file (not just "file not found")
		}

		// --- Second Pass: Open for write ---
		res = f_open(&dirManager->configFile, filename, FA_WRITE | FA_CREATE_ALWAYS);
		if (res != FR_OK)
		{
			xprintf("Failed to open file for writing: %d\n", res);
			dirManager->configRes = res;
			return res;
		}
		dirManager->configOpen = true;

		// Write comment lines
		for (uint16_t i = 0; i < comment_count; i++)
		{
			f_write(&dirManager->configFile, comment_lines[i], strlen(comment_lines[i]), &bytesWritten);
		}

		// Write parameters
		for (uint8_t i = 0; i < OP_PARAMETER_NUM_ENTRIES; i++)
		{
			snprintf(line, sizeof(line), "%d %d\n", i, op_parameter[i]);
			f_write(&dirManager->configFile, line, strlen(line), &bytesWritten);
		}

		// Close file and restore original directory
		res = f_close(&dirManager->configFile);
		if (res != FR_OK)
		{
			printf("Failed to close config file: %d\n", res);
		}
		else
		{
			dirManager->configOpen = false;
		}
		dirManager->configRes = res;
		f_chdir(dirManager->base_dir);
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
static int fatfs_load_labels_from_sd(const char *path, char labels[][48], int *label_count, int max_labels, int max_label_len)
{
	FIL f;
	FRESULT res = f_open(&f, path, FA_READ);
	if (res != FR_OK)
	{
		xprintf("Labels open failed: %s (err %d)\n", path, res);
		return -1;
	}
	*label_count = 0;
	char line[96];
	UINT br = 0;
	int pos = 0;
	for (;;)
	{
		char c;
		res = f_read(&f, &c, 1, &br);
		if (res != FR_OK || br == 0)
		{
			// EOF – flush pending line
			if (pos > 0 && *label_count < max_labels)
			{
				line[pos] = '\0';
				strncpy(labels[*label_count], line, max_label_len - 1);
				labels[*label_count][max_label_len - 1] = '\0';
				(*label_count)++;
			}
			break;
		}
		if (c == '\r')
			continue;
		if (c == '\n')
		{
			if (pos > 0 && *label_count < max_labels)
			{
				line[pos] = '\0';
				strncpy(labels[*label_count], line, max_label_len - 1);
				labels[*label_count][max_label_len - 1] = '\0';
				(*label_count)++;
			}
			pos = 0;
		}
		else if (pos < (int)sizeof(line) - 1)
		{
			line[pos++] = c;
		}
	}
	f_close(&f);
	bool labels_loaded = (*label_count > 0);
	if (labels_loaded)
	{
		xprintf("Loaded %d labels from %s\n", *label_count, path);
	}
	else
	{
		xprintf("No labels loaded from %s\n", path);
	}
	return labels_loaded ? 0 : -1;
}

/**
 * Helper to extract one entry from a ZIP file given its local header offset.
 * Only supports method 0 (STORE - uncompressed).
 *
 * @param zf_ptr Pointer to open ZIP file handle
 * @param lofs Local header offset
 * @param out_path Output file path
 * @return 0 on success, -1 on failure
 */
// Extended extractor that accepts a compressed-size hint from the Central Directory
// to handle cases where the local header size fields are zero (data descriptor).
static int fatfs_extract_entry(FIL *zf_ptr, uint32_t lofs, uint32_t csize_hint, const char *out_path)
{
	FIL *zf_local = zf_ptr;
	if (f_lseek(zf_local, lofs) != FR_OK)
		return -1;

	uint8_t lfh[30];
	UINT r = 0;
	if (f_read(zf_local, lfh, sizeof(lfh), &r) != FR_OK || r != sizeof(lfh))
		return -1;
	if (!(lfh[0] == 0x50 && lfh[1] == 0x4b && lfh[2] == 0x03 && lfh[3] == 0x04))
	{
		xprintf("Local header signature mismatch at 0x%08lX\n", (unsigned long)lofs);
		return -1;
	}

	uint16_t method = lfh[8] | (lfh[9] << 8);
	uint32_t csize_lfh = lfh[18] | (lfh[19] << 8) | (lfh[20] << 16) | (lfh[21] << 24);
	uint32_t usize = lfh[22] | (lfh[23] << 8) | (lfh[24] << 16) | (lfh[25] << 24);
	uint16_t fnlen = lfh[26] | (lfh[27] << 8);
	uint16_t xlen = lfh[28] | (lfh[29] << 8);

	// Only support STORE (method 0)
	if (method != 0)
	{
		xprintf("Zip entry %s uses compression method %u (only STORE/method 0 is supported).\n", out_path, method);
		return -1;
	}

	// Skip filename + extra to data
	if (f_lseek(zf_local, lofs + 30 + fnlen + xlen) != FR_OK)
		return -1;

	// Ensure /Manifest directory exists
	f_mkdir("/Manifest");

	FIL out;
	if (f_open(&out, out_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return -1;

	int ret = 0;

	// STORE: simple copy
	uint8_t copybuf[256]; // smaller buffer to reduce stack usage
	// Prefer size from Central Directory if provided (handles data-descriptor case)
	UINT togo = (csize_hint != 0) ? csize_hint : csize_lfh;
	UINT rw = 0;
	while (togo > 0)
	{
		UINT chunk = (togo > sizeof(copybuf)) ? sizeof(copybuf) : togo;
		if (f_read(zf_local, copybuf, chunk, &rw) != FR_OK || rw != chunk)
		{
			ret = -1;
			break;
		}
		UINT bw = 0;
		if (f_write(&out, copybuf, chunk, &bw) != FR_OK || bw != chunk)
		{
			ret = -1;
			break;
		}
		togo -= chunk;
	}

	f_close(&out);

	if (ret == 0)
	{
		xprintf("Extracted %s (%lu bytes, stored)\n", out_path, (unsigned long)usize);
	}
	return ret;
}

/**
 * Minimal unzipper: extract manifest/labels.txt and manifest/MOD0000X.tfl
 * Only supports method 0 (STORE, no compression).
 *
 * @return 0 on success (both files present or at least model), -1 on failure.
 */
static int fatfs_unzip_manifest_zip(void)
{
	const char *zip_path = "/Manifest.zip";
	FIL zf;
	FRESULT res = f_open(&zf, zip_path, FA_READ);
	if (res != FR_OK)
	{
		xprintf("No manifest.zip present (err %d)\n", res);
		return -1;
	}

	DWORD fsize = f_size(&zf);
	xprintf("manifest.zip size=%lu bytes\n", (unsigned long)fsize);

	// Backward scan for EOCD (0x06054b50) up to 64KB
	const uint32_t max_scan = (fsize > 0x10000U) ? 0x10000U : fsize;
	const uint32_t chunk = 256U; // smaller chunk to reduce stack usage
	uint8_t buf[chunk];
	uint32_t eocd_offset = 0;
	for (uint32_t scanned = 0; scanned < max_scan; scanned += chunk)
	{
		// Position start of this chunk
		uint32_t off = (fsize > scanned + chunk) ? (fsize - scanned - chunk) : 0; // clamp at 0
		if (f_lseek(&zf, off) != FR_OK)
		{
			xprintf("EOCD scan: seek fail at off=%lu\n", (unsigned long)off);
			break;
		}
		UINT br = 0;
		if (f_read(&zf, buf, chunk, &br) != FR_OK)
		{
			xprintf("EOCD scan: read fail\n");
			break;
		}
		if (br < 22) // minimal EOCD length
			continue;
		for (int i = (int)br - 4; i >= 0; --i)
		{
			if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06)
			{
				eocd_offset = off + (uint32_t)i;
				xprintf("EOCD found at offset %lu (scanned %lu bytes)\n", (unsigned long)eocd_offset, (unsigned long)scanned + chunk);
				goto have_eocd;
			}
		}
	}
have_eocd:
	if (eocd_offset == 0)
	{
		xprintf("EOCD not found in manifest.zip\n");
		f_close(&zf);
		return -1;
	}

	// Parse EOCD (we need central directory size + offset). EOCD layout:
	// signature(4) disk_num(2) cd_start_disk(2) entries_this_disk(2) entries_total(2) cd_size(4) cd_offset(4) comment_len(2)
	if (f_lseek(&zf, eocd_offset + 12) != FR_OK) // move to cd_size field (offset 12 from start)
	{
		f_close(&zf);
		return -1;
	}
	uint8_t eocd_fields[8];
	UINT br = 0;
	if (f_read(&zf, eocd_fields, sizeof(eocd_fields), &br) != FR_OK || br != sizeof(eocd_fields))
	{
		f_close(&zf);
		return -1;
	}
	uint32_t cd_size = eocd_fields[0] | (eocd_fields[1] << 8) | (eocd_fields[2] << 16) | (eocd_fields[3] << 24);
	uint32_t cd_offset = eocd_fields[4] | (eocd_fields[5] << 8) | (eocd_fields[6] << 16) | (eocd_fields[7] << 24);

	xprintf("Central Directory: size=%lu offset=%lu\n", (unsigned long)cd_size, (unsigned long)cd_offset);

	if (cd_offset + cd_size > fsize)
	{
		xprintf("Central directory out of range\n");
		f_close(&zf);
		return -1;
	}

	if (f_lseek(&zf, cd_offset) != FR_OK)
	{
		f_close(&zf);
		return -1;
	}

	uint32_t labels_lhofs = 0, model_lhofs = 0;
	bool labels_found = false, model_found = false;
	uint32_t model_csize_cd = 0; // compressed size from Central Directory for model
	char model_target_name[32] = {0};
	uint32_t processed = 0;
	uint32_t entry_index = 0;
	bool saw_compressed = false; // track if we encountered any compressed entries
	uint16_t first_compressed_method = 0;
	while (processed < cd_size)
	{
		uint8_t cdhdr[46];
		br = 0;
		if (f_read(&zf, cdhdr, sizeof(cdhdr), &br) != FR_OK || br != sizeof(cdhdr))
		{
			xprintf("CD read break at processed=%lu\n", (unsigned long)processed);
			break;
		}
		if (!(cdhdr[0] == 0x50 && cdhdr[1] == 0x4b && cdhdr[2] == 0x01 && cdhdr[3] == 0x02))
		{
			xprintf("CD signature mismatch at entry %lu\n", (unsigned long)entry_index);
			break; // stop parsing
		}
		uint16_t fnlen = cdhdr[28] | (cdhdr[29] << 8);
		uint16_t xlen = cdhdr[30] | (cdhdr[31] << 8);
		uint16_t clen = cdhdr[32] | (cdhdr[33] << 8);
		uint16_t method = cdhdr[10] | (cdhdr[11] << 8);
		uint32_t csize_cd = cdhdr[20] | (cdhdr[21] << 8) | (cdhdr[22] << 16) | (cdhdr[23] << 24);
		uint32_t lhofs = cdhdr[42] | (cdhdr[43] << 8) | (cdhdr[44] << 16) | (cdhdr[45] << 24);

		char name[128];
		if (fnlen >= sizeof(name))
			fnlen = sizeof(name) - 1;
		br = 0;
		if (f_read(&zf, name, fnlen, &br) != FR_OK || br != fnlen)
		{
			xprintf("Filename read fail at entry %lu\n", (unsigned long)entry_index);
			break;
		}
		name[fnlen] = '\0';

		if (xlen && f_lseek(&zf, f_tell(&zf) + xlen) != FR_OK)
		{
			xprintf("Skip extra fail\n");
			break;
		}
		if (clen && f_lseek(&zf, f_tell(&zf) + clen) != FR_OK)
		{
			xprintf("Skip comment fail\n");
			break;
		}

		processed += 46 + fnlen + xlen + clen;
		entry_index++;

		xprintf("ZIP entry[%lu]: '%s' method=%u local_ofs=%lu\n", (unsigned long)entry_index, name, (unsigned)method, (unsigned long)lhofs);

		if (method != 0)
		{
			xprintf("  Skipping (compressed method %u not supported)\n", (unsigned)method);
			if (!saw_compressed)
			{
				saw_compressed = true;
				first_compressed_method = method;
			}
			continue;
		}

		// Case-insensitive match
		char name_lower[128];
		size_t name_len = strlen(name);
		for (size_t i = 0; i < name_len && i < sizeof(name_lower) - 1; i++)
			name_lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (name[i] + 32) : name[i];
		name_lower[name_len] = '\0';

		if (strcmp(name_lower, "manifest/labels.txt") == 0)
		{
			labels_lhofs = lhofs;
			labels_found = true;
		}
		else if (strncmp(name_lower, "manifest/mod0000", 16) == 0 && strstr(name_lower, ".tfl"))
		{
			model_lhofs = lhofs;
			model_csize_cd = csize_cd;
			const char *slash = strrchr(name, '/');
			strncpy(model_target_name, slash ? slash + 1 : name, sizeof(model_target_name) - 1);
			model_found = true;
		}
	}

	xprintf("ZIP scan complete: labels_offset=%lu, model_offset=%lu, model_name='%s'\n",
			(unsigned long)labels_lhofs, (unsigned long)model_lhofs, model_target_name);

	// If we saw compressed entries and couldn't locate uncompressed targets, give a clear hint
	if ((model_lhofs == 0) && saw_compressed)
	{
		XP_LT_RED;
		xprintf("Manifest.zip uses compression (method %u). This firmware only supports STORE (method 0).\n", (unsigned)first_compressed_method);
		xprintf("Please repackage with no compression, e.g.: 'zip -0 -r Manifest.zip Manifest',\n");
		xprintf("or place unzipped files on the SD at /Manifest/MODxxxxx.tfl and /Manifest/labels.txt.\n");
		XP_WHITE;
	}

	int ok = -1;
	if (labels_found)
		fatfs_extract_entry(&zf, labels_lhofs, 0, "/Manifest/labels.txt");
	if (model_found && model_target_name[0])
	{
		char outpath[64];
		snprintf(outpath, sizeof(outpath), "/Manifest/%s", model_target_name);
		xprintf("Attempting extract: name='%s' lofs=%lu csize=%lu\n", model_target_name, (unsigned long)model_lhofs, (unsigned long)model_csize_cd);
		ok = fatfs_extract_entry(&zf, model_lhofs, model_csize_cd, outpath);
	}
	f_close(&zf);
	return ok;
}

/********************************** FreeRTOS Task  *************************************/

/**
 * FreeRTOS task responsible for handling interactions with the FatFS
 *
 * This is called when the scheduler starts.
 * Various entities have already be set up by fatfs_createTask()
 *
 * After some one-off activities it waits for events to arrive in its xFatTaskQueue
 */
static void vFatFsTask(void *pvParameters)
{
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

	// Initialise the configuration[] array
	op_parameter[OP_PARAMETER_SEQUENCE_NUMBER] = 0; // 0 indicates no SD card
	op_parameter[OP_PARAMETER_NUM_PICTURES] = NUMPICTURESTOGRAB;
	op_parameter[OP_PARAMETER_PICTURE_INTERVAL] = PICTUREINTERVAL;
	op_parameter[OP_PARAMETER_TIMELAPSE_INTERVAL] = TIMELAPSEINTERVAL;
	op_parameter[OP_PARAMETER_INTERVAL_BEFORE_DPD] = INACTIVITYTIMEOUT;
	op_parameter[OP_PARAMETER_LED_FLASH_DUTY] = FLASHLEDDUTY;
	op_parameter[OP_PARAMETER_NUM_NN_ANALYSES] = 0;
	op_parameter[OP_PARAMETER_NUM_COLD_BOOTS] = 0;
	op_parameter[OP_PARAMETER_NUM_WARM_BOOTS] = 0;
	// why would we want the  default (no SD card) to be disabled?
	// op_parameter[OP_PARAMETER_CAMERA_ENABLED] = 0;	// disabled
	op_parameter[OP_PARAMETER_CAMERA_ENABLED] = 1;				  // enabled
	op_parameter[OP_PARAMETER_MD_INTERVAL] = DPDINTERVAL;		  // Interval (ms) between frames in MD mode (0 inhibits)
	op_parameter[OP_PARAMETER_MODEL_NUMBER] = get_model_number(); // Model number of the device

	// One-off initialisation here...
	startTime = xTaskGetTickCount();

	// TODO - experiment - do I need settling time for 3V3_WE?
	vTaskDelay(pdMS_TO_TICKS(10));
	res = fatFsInit();

	if (res == FR_OK)
	{
		fatFs_task_state = APP_FATFS_STATE_IDLE;
		// Only if the file system is working should we add CLI commands for FATFS
		cli_fatfs_init();

		res = init_directories(&dirManager);
		if (res == FR_OK)
		{

			xprintf("SD card initialised. ");

			// Load all the saved configuration values, including the image sequence number
			res = load_configuration(STATE_FILE, &dirManager);
			if (res == FR_OK)
			{
				// File exists and op_parameter[] has been initialised
				enabled = op_parameter[OP_PARAMETER_CAMERA_ENABLED];
				xprintf("'%s' found. (Next image #%d), camera %senabled. Flash duty cycle %d\%\r\n",
						STATE_FILE,
						fatfs_getImageSequenceNumber(),
						(enabled == 1) ? "" : "not ",
						op_parameter[OP_PARAMETER_LED_FLASH_DUTY]);
			}
			else
			{
				fatfs_setOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER, 1);
				xprintf("'%s' NOT found. (Next image #1)\r\n", STATE_FILE);
			}
		}
	}
	else
	{
		// Failure.
		xprintf("SD card initialisation failed (reason %d)\r\n", res);
	}

	elapsedTime = xTaskGetTickCount() - startTime;
	elapsedMs = (elapsedTime * 1000) / configTICK_RATE_HZ;

	xprintf("FatFs setup took %dms\n", elapsedMs);

	// Start a timer that detects inactivity in every task, exceeding op_parameter[OP_PARAMETER_INTERVAL_BEFORE_DPD]
	if (woken == APP_WAKE_REASON_COLD)
	{
		// Short timeout after cold boot.
		inactivityPeriod = INACTIVITYTIMEOUTCB;
		fatfs_incrementOperationalParameter(OP_PARAMETER_NUM_COLD_BOOTS);
	}
	else
	{
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
	if (woken == APP_WAKE_REASON_COLD)
	{
		vTaskDelay(pdMS_TO_TICKS(1000));
	}

	if (xQueueSend(xIfTaskQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
	{
		xprintf("sendMsg=0x%x fail\r\n", sendMsg.msg_event);
	}

	// The task loops forever here, waiting for messages to arrive in its input queue
	for (;;)
	{
		if (xQueueReceive(xFatTaskQueue, &(rxMessage), __QueueRecvTicksToWait) == pdTRUE)
		{
			event = rxMessage.msg_event;
			rxData = rxMessage.msg_data;

			// convert event to a string
			if ((event >= APP_MSG_FATFSTASK_FIRST) && (event < APP_MSG_FATFSTASK_LAST))
			{
				eventString = fatFsTaskEventString[event - APP_MSG_FATFSTASK_FIRST];
			}
			else
			{
				eventString = "Unexpected";
			}

			XP_LT_CYAN
			xprintf("\nFatFS Task ");
			XP_WHITE;
			xprintf("received event '%s' (0x%04x). Rx data = 0x%08x\r\n", eventString, event, rxData);

			old_state = fatFs_task_state;

			// switch on state - and call individual event handling functions
			switch (fatFs_task_state)
			{

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

			if (old_state != fatFs_task_state)
			{
				// state has changed
				XP_LT_CYAN;
				xprintf("FatFS Task state changed ");
				XP_WHITE;
				xprintf("from '%s' (%d) to '%s' (%d)\r\n",
						fatFsTaskStateString[old_state], old_state,
						fatFsTaskStateString[fatFs_task_state], fatFs_task_state);
			}

			// The processing functions might want us to send a message to another task
			if (txMessage.destination != NULL)
			{
				sendMsg = txMessage.message;
				targetQueue = txMessage.destination;

				if (xQueueSend(targetQueue, (void *)&sendMsg, __QueueSendTicksToWait) != pdTRUE)
				{
					xprintf("FatFS task sending event 0x%x failed\r\n", sendMsg.msg_event);
				}
				else
				{
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

TaskHandle_t fatfs_createTask(int8_t priority, APP_WAKE_REASON_E wakeReason)
{

	if (priority < 0)
	{
		priority = 0;
	}

	// Save this. Determines inactivity period at cold boot
	woken = wakeReason;

	xFatTaskQueue = xQueueCreate(FATFS_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xFatTaskQueue == 0)
	{
		xprintf("Failed to create xFatTaskQueue\n");
		configASSERT(0); // TODO add debug messages?
	}

	if (xTaskCreate(vFatFsTask, (const char *)"FAT",
					3 * configMINIMAL_STACK_SIZE + CLI_CMD_LINE_BUF_SIZE + CLI_OUTPUT_BUF_SIZE,
					NULL, priority,
					&fatFs_task_id) != pdPASS)
	{
		xprintf("Failed to create vFatFsTask\n");
		configASSERT(0); // TODO add debug messages?
	}

	return fatFs_task_id;
}

/**
 * Returns the internal state as a number
 */
uint16_t fatfs_getState(void)
{
	return fatFs_task_state;
}

/**
 * Returns true of the filesystem is mounted
 *
 * Use thsi before attempting disk operations, if unsure
 *
 * @return true if mounted
 */
bool fatfs_mounted(void)
{
	return mounted;
}

/**
 * Returns the internal state as a string
 */
const char *fatfs_getStateString(void)
{
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
uint16_t fatfs_getOperationalParameter(OP_PARAMETERS_E parameter)
{

	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES))
	{
		return op_parameter[parameter];
	}
	else
	{
		return 0;
	}
}

/**
 * Get the image sequence number
 *
 * Short-hand version of fatfs_getOperationalParameter(OP_PARAMETER_SEQUENCE_NUMBER)
 */
uint16_t fatfs_getImageSequenceNumber(void)
{
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
void fatfs_setOperationalParameter(OP_PARAMETERS_E parameter, int16_t value)
{

	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES))
	{
		op_parameter[parameter] = value;
	}
	else
	{
		// error
	}
}

/**
 * Increment one of teh state variables
 */
void fatfs_incrementOperationalParameter(OP_PARAMETERS_E parameter)
{
	if ((parameter >= 0) && (parameter < OP_PARAMETER_NUM_ENTRIES))
	{
		// TODO - do we need to prevent roll-over?
		op_parameter[parameter]++;
	}
	else
	{
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
int fatfs_load_labels(const char *path, char labels[][48], int *label_count, int max_labels, int max_label_len)
{
	return fatfs_load_labels_from_sd(path, labels, label_count, max_labels, max_label_len);
}

/**
 * Unzip Manifest.zip (public API)
 * Extracts manifest/labels.txt and manifest/MOD0000X.tfl files
 * Only supports method 0 (STORE - uncompressed)
 *
 * @return 0 on success, -1 on failure
 */
int fatfs_unzip_manifest(void)
{
	return fatfs_unzip_manifest_zip();
}
