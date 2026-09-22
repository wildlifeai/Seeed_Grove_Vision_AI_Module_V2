/*
 * fatfs_task.c
 *
 *  Created on: 21 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that owns the SD card and the FatFS file system on it. See fatfs_task.h.
 *
 *  The SD card is powered from the 3V3_WE rail, which is on only while the processor is running (PA1 low), so
 *  the task waits for it to settle before mounting.
 *
 *  Three functions here have names that other code requires, so they do not follow the fatfs_task_ prefix:
 *  SSPI_CS_GPIO_Pinmux(), SSPI_CS_GPIO_Output_Level() and SSPI_CS_GPIO_Dir() (called by the SD card driver in
 *  middleware/fatfs/port/mmc_spi/mmc_we2_spi.c to manage the SPI chip select) and get_fattime() (called by FatFS).
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "hx_drv_gpio.h"
#include "hx_drv_scu.h"

#include "printf_x.h"
#include "xprintf.h"

#include "app_msg.h"
#include "barrier.h"
#include "fatfs_task.h"
#include "rtc_util.h"
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

#define FATFS_TASK_QUEUE_LEN			5

// The default drive. There is only one volume.
#define DRV								""

// The time to wait for the 3V3_WE rail to settle before mounting the card
#define SUPPLY_SETTLE_MS				10

// The boot count in the file is always this many bytes: ten digits and a line end
#define BOOT_COUNT_FILE_SIZE			11

// Room for the boot count as text, with the trailing '\0' (must be more than BOOT_COUNT_FILE_SIZE)
#define BOOT_COUNT_TEXT_LENGTH			16

// The value returned by get_fattime() if the RTC cannot be read: 2024-01-01 00:00:00
#define DEFAULT_FAT_TIME				((DWORD) (44 << 25) | (1 << 21) | (1 << 16))

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

extern Barrier_t startupBarrier;	// Object that calls a function when all tasks are ready
extern Barrier_t shutdownBarrier;	// Object that calls a function when all tasks are ready to shut down

/********************************************** Local Variables **********************************************/

// The queue that other tasks use to send messages to this task
static QueueHandle_t xFatTaskQueue;

// This is the handle of the task
static TaskHandle_t fatfs_task_id;

static volatile FATFS_TASK_STATE_E fatFsState = FATFS_TASK_STATE_UNINIT;

// FatFS working areas. Static, as they are large (each holds a 512 byte sector buffer)
static FATFS fs;
static FIL file;

static bool mounted = false;
static bool bootCountOk = false;
static uint32_t bootCount = 0;

// Details of the card, worked out when it is mounted
static uint32_t capacityMB = 0;
static BYTE fsType = 0;

// Strings for each of the states. Values must match FATFS_TASK_STATE_E in fatfs_task.h
static const char * fatFsStateString[FATFS_TASK_NUMSTATES] = {
		"Uninitialised",
		"No card",
		"Idle",
		"Busy",
};

/**************************************** Local Function Declarations ****************************************/

static void vFatFsTask(void *pvParameters);
static FRESULT mountCard(void);
static bool updateBootCount(void);
static FRESULT writeFile(fileOperation_t *fileOp);
static FRESULT readFile(fileOperation_t *fileOp);
static void handleFileEvent(APP_MSG_EVENT_E event, fileOperation_t *fileOp);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Mounts the SD card.
 *
 * Waits for the supply to settle first. There is no card-detect signal, so if the mount succeeds there is a card.
 * Works out the card capacity and file system type. The mount reads the card, which takes a while: with no card
 * it can take a second or more before the driver gives up.
 *
 * @return FR_OK if the card was mounted, otherwise the FatFS error.
 */
static FRESULT mountCard(void) {
	FRESULT res;

	vTaskDelay(pdMS_TO_TICKS(SUPPLY_SETTLE_MS));

	XP_CYAN;
	xprintf("Mounting FatFS on SD card ");
	XP_WHITE;

	res = f_mount(&fs, DRV, 1);

	if (res == FR_OK) {
		mounted = true;
		fsType = fs.fs_type;
		// (number of clusters) * (sectors per cluster) * 512 bytes, in MB
		capacityMB = (uint32_t) (((uint64_t) (fs.n_fatent - 2) * fs.csize) / 2048);
		xprintf("OK\n");
	}
	else {
		mounted = false;
		XP_RED;
		xprintf("failed, error = %d\n", (int) res);
		XP_WHITE;
	}

	return res;
}

/**
 * @brief Reads BOOTS.TXT, adds one and writes it back, in place.
 *
 * FatFS is not safe against power being lost part way through an update, and the SD card's supply is cut when the processor
 * enters DPD. So the update is kept as small as possible: the count is a fixed width number (BOOT_COUNT_FILE_SIZE bytes)
 * that overwrites the start of the existing file. That changes one data sector and the directory entry. It does not
 * change the FAT or the free space, which recreating the file at every boot would (about six to eight sector writes). Only
 * the very first boot, which creates the file, allocates a cluster.
 *
 * If the file is missing, empty or not a number the count starts at 1. The new count is used only if it was written successfully.
 *
 * @return true if the count was updated. The new value is then in bootCount.
 */
static bool updateBootCount(void) {
	char text[BOOT_COUNT_TEXT_LENGTH];
	uint32_t count = 0;
	UINT bytes = 0;
	FRESULT res;
	FRESULT closeRes;

	res = f_open(&file, FATFS_TASK_BOOT_COUNT_FILE, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);

	if (res != FR_OK) {
		return false;
	}

	if (f_size(&file) > 0) {
		res = f_read(&file, text, sizeof(text) - 1, &bytes);

		if (res != FR_OK) {
			f_close(&file);
			return false;
		}

		text[bytes] = '\0';
		count = strtoul(text, NULL, 10);	// If it is not a number this is 0
	}

	count++;
	snprintf(text, sizeof(text), "%010lu\n", (unsigned long) count);

	res = f_lseek(&file, 0);

	if (res == FR_OK) {
		res = f_write(&file, text, BOOT_COUNT_FILE_SIZE, &bytes);
	}
	if ((res == FR_OK) && (bytes != BOOT_COUNT_FILE_SIZE)) {
		res = FR_DENIED;	// The card is full
	}
	if ((res == FR_OK) && (f_size(&file) > BOOT_COUNT_FILE_SIZE)) {
		res = f_truncate(&file);	// A longer file, such as one written by hand: remove the excess
	}

	closeRes = f_close(&file);

	if ((res != FR_OK) || (closeRes != FR_OK)) {
		return false;
	}

	bootCount = count;

	return true;
}

/**
 * @brief Writes a file, replacing it if it exists. The file is closed again before returning.
 *
 * @param fileOp The operation. On return fileOp->length is the number of bytes written.
 * @return FR_OK if all the bytes were written. FR_DENIED if the card filled up.
 */
static FRESULT writeFile(fileOperation_t *fileOp) {
	UINT written = 0;
	FRESULT res;
	FRESULT closeRes;

	res = f_open(&file, fileOp->fileName, FA_WRITE | FA_CREATE_ALWAYS);

	if (res == FR_OK) {
		res = f_write(&file, fileOp->buffer, fileOp->length, &written);
		closeRes = f_close(&file);

		if (res == FR_OK) {
			res = closeRes;
		}
		if ((res == FR_OK) && (written != fileOp->length)) {
			res = FR_DENIED;	// FatFS wrote fewer bytes than asked: the card is full
		}
	}

	fileOp->length = written;

	return res;
}

/**
 * @brief Reads a file into a buffer. The file is closed again before returning.
 *
 * @param fileOp The operation. On entry fileOp->length is the size of the buffer. On return it is the number of bytes read.
 * @return FR_OK if the file was read.
 */
static FRESULT readFile(fileOperation_t *fileOp) {
	UINT bytesRead = 0;
	FRESULT res;
	FRESULT closeRes;

	res = f_open(&file, fileOp->fileName, FA_READ);

	if (res == FR_OK) {
		res = f_read(&file, fileOp->buffer, fileOp->length, &bytesRead);
		closeRes = f_close(&file);

		if (res == FR_OK) {
			res = closeRes;
		}
	}

	fileOp->length = bytesRead;

	return res;
}

/**
 * @brief Carries out a file operation and sends the reply.
 *
 * With no card the operation is refused at once with FR_NOT_READY.
 *
 * @param event  APP_MSG_FATFSTASK_WRITE_FILE or APP_MSG_FATFSTASK_READ_FILE.
 * @param fileOp The operation. The result is placed in fileOp->res.
 */
static void handleFileEvent(APP_MSG_EVENT_E event, fileOperation_t *fileOp) {
	APP_MSG_T reply;

	if (!mounted) {
		fileOp->length = 0;
		fileOp->res = FR_NOT_READY;
	}
	else {
		fatFsState = FATFS_TASK_STATE_BUSY;

		if (event == APP_MSG_FATFSTASK_WRITE_FILE) {
			fileOp->res = writeFile(fileOp);
		}
		else {
			fileOp->res = readFile(fileOp);
		}

		fatFsState = FATFS_TASK_STATE_IDLE;
	}

	reply.msg_event = fileOp->doneEvent;
	reply.msg_data = (uint32_t) fileOp;
	reply.msg_parameter = (uint32_t) fileOp->res;

	if (xQueueSend(fileOp->senderQueue, (void *) &reply, WW500_MINIMAL_QUEUE_SEND_TICKS) != pdTRUE) {
		xprintf("FatFS Task: reply 0x%x could not be sent\n", reply.msg_event);
	}
}

/**
 * @brief The FreeRTOS task.
 *
 * Mounts the card and updates the boot count, then reports that it is ready and serves file operations.
 *
 * @param pvParameters Not used.
 */
static void vFatFsTask(void *pvParameters) {
	APP_MSG_T rxMessage;
	TickType_t startTime;

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting FatFS Task\n");
	XP_WHITE;

	startTime = xTaskGetTickCount();

	if (mountCard() == FR_OK) {
		fatFsState = FATFS_TASK_STATE_IDLE;

		bootCountOk = updateBootCount();

		if (bootCountOk) {
			XP_LT_GREEN;
			xprintf("Boot count is %u (SD card ready in %d ms)\n", (unsigned) bootCount,
					(int) ww500_minimal_getElapsedMs(startTime));
			XP_WHITE;
		}
		else {
			XP_RED;
			xprintf("The boot count in '%s' could not be updated\n", FATFS_TASK_BOOT_COUNT_FILE);
			XP_WHITE;
		}
	}
	else {
		fatFsState = FATFS_TASK_STATE_NO_CARD;
		XP_YELLOW;
		xprintf("No SD card. Carrying on without it (%d ms)\n", (int) ww500_minimal_getElapsedMs(startTime));
		XP_WHITE;
	}

	barrier_ready(&startupBarrier);		// Call a function when every task reaches this point

	for (;;) {
		if (xQueueReceive(xFatTaskQueue, &rxMessage, portMAX_DELAY) == pdTRUE) {

			switch (rxMessage.msg_event) {

			case APP_MSG_FATFSTASK_WRITE_FILE:
			case APP_MSG_FATFSTASK_READ_FILE:
				handleFileEvent(rxMessage.msg_event, (fileOperation_t *) rxMessage.msg_data);
				break;

			case APP_MSG_FATFSTASK_INACTIVITY:
				// Any operation in progress has finished, as messages are handled one at a time.
				// The files are closed after every operation, so the card is safe to lose power.
				// When every task has reported in, the barrier calls the function that enters DPD.
				barrier_ready(&shutdownBarrier);
				break;

			default:
				xprintf("FatFS Task: unexpected event 0x%04x\n", rxMessage.msg_event);
				break;
			}
		}
	}
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 *
 * @param priority   The FreeRTOS priority for the task.
 * @param wakeReason The reason for this wakeup (not used).
 * @return The handle of the task.
 */
TaskHandle_t fatfs_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason) {
	(void)wakeReason;

	if (priority < 0) {
		priority = 0;
	}

	xFatTaskQueue = xQueueCreate(FATFS_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xFatTaskQueue == 0) {
		xprintf("Failed to create xFatTaskQueue\n");
		configASSERT(0);
	}

	if (xTaskCreate(vFatFsTask, (const char *)"FatFS",
			configMINIMAL_STACK_SIZE * 6,
			NULL, priority,
			&fatfs_task_id) != pdPASS) {
		xprintf("Failed to create vFatFsTask\n");
		configASSERT(0);
	}

	return fatfs_task_id;
}

/**
 * @brief Returns the internal state as a number.
 *
 * @return The state, a FATFS_TASK_STATE_E value.
 */
uint16_t fatfs_task_getState(void) {
	return fatFsState;
}

/**
 * @brief Returns the internal state as a string.
 *
 * @return A pointer to a constant string.
 */
const char * fatfs_task_getStateString(void) {
	return fatFsStateString[fatFsState];
}

/**
 * @brief Returns true if the SD card was mounted.
 *
 * @return true if there is a card and it was mounted.
 */
bool fatfs_task_mounted(void) {
	return mounted;
}

/**
 * @brief Returns true if the boot count was read, incremented and written back at this boot.
 *
 * The image task uses the count in the names of the files it writes, and should not write an image if this is false.
 *
 * @return true if fatfs_task_getBootCount() is valid.
 */
bool fatfs_task_bootCountValid(void) {
	return bootCountOk;
}

/**
 * @brief Returns the boot count for this boot.
 *
 * @return The count. Only meaningful if fatfs_task_bootCountValid() is true.
 */
uint32_t fatfs_task_getBootCount(void) {
	return bootCount;
}

/**
 * @brief Returns true while the task is starting up or carrying out a file operation.
 *
 * Used to wait before doing something that suppresses interrupts, such as setting the RTC.
 *
 * @return true if the task is busy.
 */
bool fatfs_task_isBusy(void) {
	return (fatFsState == FATFS_TASK_STATE_UNINIT) || (fatFsState == FATFS_TASK_STATE_BUSY);
}

/**
 * @brief Prints the state of the card and the boot count.
 *
 * Uses values worked out when the card was mounted, so it makes no FatFS calls.
 */
void fatfs_task_printStatus(void) {
	const char * typeString;

	xprintf("FatFS task: %s\n", fatfs_task_getStateString());

	if (mounted) {
		switch (fsType) {
		case FS_FAT12:
			typeString = "FAT12";
			break;
		case FS_FAT16:
			typeString = "FAT16";
			break;
		case FS_FAT32:
			typeString = "FAT32";
			break;
		default:
			typeString = "another type of file system";
			break;
		}
		xprintf("SD card: %s, %u MB\n", typeString, (unsigned) capacityMB);
	}
	else {
		xprintf("SD card: not mounted\n");
	}

	if (bootCountOk) {
		xprintf("Boot count: %u\n", (unsigned) bootCount);
	}
	else {
		xprintf("Boot count: not available\n");
	}
}

/**
 * @brief Asks the task to write or read a file. The reply arrives on fileOp->senderQueue.
 *
 * The reply is a message with the event fileOp->doneEvent, msg_data = fileOp and msg_parameter = the FatFS result.
 * fileOp, the file name and the buffer must stay valid until the reply arrives.
 *
 * @param event  APP_MSG_FATFSTASK_WRITE_FILE or APP_MSG_FATFSTASK_READ_FILE.
 * @param fileOp The operation.
 * @return true if the request was queued.
 */
bool fatfs_task_sendFileOp(APP_MSG_EVENT_E event, fileOperation_t *fileOp) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = event;
	sendMsg.msg_data = (uint32_t) fileOp;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xFatTaskQueue, (void *) &sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Tells the task that all tasks are inactive, so it should finish what it is doing and get ready for DPD.
 *
 * Called from the FreeRTOS idle hook (see inactivity.c) so it must not block: the message is dropped if the
 * queue is full.
 */
void fatfs_task_notifyInactivity(void) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_FATFSTASK_INACTIVITY;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	xQueueSend(xFatTaskQueue, (void *)&sendMsg, 0);
}

/**
 * @brief Selects whether PB5 is the SPI chip select or an ordinary GPIO. Called by the SD card driver.
 *
 * The driver drives the chip select itself (as GPIO16) so that it stays low for a whole SD card command.
 *
 * @param setGpioFn true to make PB5 GPIO16, false to make it the SPI master chip select.
 */
void SSPI_CS_GPIO_Pinmux(bool setGpioFn) {
    if (setGpioFn) {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_GPIO16, 0);
    }
    else {
        hx_drv_scu_set_PB5_pinmux(SCU_PB5_PINMUX_SPI_M_CS_1, 0);
    }
}

/**
 * @brief Sets the level of the chip select GPIO (GPIO16). Called by the SD card driver.
 *
 * @param setLevelHigh true for high.
 */
void SSPI_CS_GPIO_Output_Level(bool setLevelHigh) {
    hx_drv_gpio_set_out_value(GPIO16, (GPIO_OUT_LEVEL_E) setLevelHigh);
}

/**
 * @brief Sets the direction of the chip select GPIO (GPIO16). Called by the SD card driver.
 *
 * @param setDirOut true for an output (driven high), false for an input.
 */
void SSPI_CS_GPIO_Dir(bool setDirOut) {
    if (setDirOut) {
        hx_drv_gpio_set_output(GPIO16, GPIO_OUT_HIGH);
    }
    else {
        hx_drv_gpio_set_input(GPIO16);
    }
}

/**
 * @brief Returns the time for the time stamps that FatFS puts on files. Called by FatFS.
 *
 * Uses the RTC. If it cannot be read the time is 2024-01-01 00:00:00, the time set at a cold boot. Note that the first
 * RTC read after a wake from DPD is the time DPD was entered (see WW500_MINIMAL_SYNC_RTC_AFTER_DPD).
 *
 * The result is in the FAT format:
 *   bit31:25 - Year from 1980 (0..127)
 *   bit24:21 - Month (1..12)
 *   bit20:16 - Day (1..31)
 *   bit15:11 - Hour (0..23)
 *   bit10:5  - Minute (0..59)
 *   bit4:0   - Second / 2 (0..29, means 0 to 58 seconds)
 *
 * @return The time in FAT format.
 */
DWORD get_fattime(void) {
	rtc_time tm = {0};

	if ((rtc_util_getTime(&tm) != RTC_NO_ERROR) || (tm.tm_year < 1980) || (tm.tm_year > 2107)) {
		return DEFAULT_FAT_TIME;
	}

	return ((DWORD) (tm.tm_year - 1980) << 25)
			| ((DWORD) tm.tm_mon << 21)
			| ((DWORD) tm.tm_mday << 16)
			| ((DWORD) tm.tm_hour << 11)
			| ((DWORD) tm.tm_min << 5)
			| ((DWORD) tm.tm_sec >> 1);
}
