/*
 * fatfs_task.h
 *
 *  Created on: 21 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that owns the SD card, and the FatFS file system on it. A light-weight version of the
 *  task in ww500_md, which does a great deal more.
 *
 *  What it does:
 *   - Mounts the card at start-up. There is no card-detect signal, so a card is present if the mount succeeds.
 *     The app works with or without a card. The card must not be changed while the app is running.
 *   - Keeps a boot count in BOOTS.TXT in the root directory: one total for every boot. It is incremented once
 *     at start-up. Later the image task will use it in the names of the JPEG files, and skip the image if the
 *     count could not be updated (see fatfs_task_bootCountValid()).
 *   - Writes and reads files on request. Other tasks send it a message containing a fileOperation_t and get a
 *     reply message on their own queue. Only 8.3 file names, in the root directory.
 *   - Takes part in the shutdown barrier, so that DPD is never entered while a file operation is in progress.
 *
 *  All FatFS calls are made by this task (FatFS is not built to be re-entrant), so nothing else may call ff.h
 *  functions.
 */

#ifndef FATFS_TASK_H_
#define FATFS_TASK_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "ff.h"

#include "app_msg.h"
#include "ww500_minimal.h"

/********************************************** Global Defines ***********************************************/

// The name of the file holding the boot count (8.3 format, in the root directory)
#define FATFS_TASK_BOOT_COUNT_FILE		"BOOTS.TXT"

// Longest file name that can be used: 8.3 plus the trailing '\0'
#define FATFS_TASK_FILENAME_LENGTH		13

/*********************************************** Global Types ************************************************/

// The states of the task. Values must match the strings in fatfs_task.c
typedef enum {
	FATFS_TASK_STATE_UNINIT,
	FATFS_TASK_STATE_NO_CARD,
	FATFS_TASK_STATE_IDLE,
	FATFS_TASK_STATE_BUSY,
	FATFS_TASK_NUMSTATES
} FATFS_TASK_STATE_E;

/**
 * A file operation: the request sent to the task and, when it has finished, the result.
 *
 * The structure and the buffer belong to the sender and must stay valid until the reply arrives.
 */
typedef struct {
	const char *	fileName;		// 8.3 name in the root directory
	uint8_t *		buffer;			// Data to write, or the buffer to read into
	uint32_t 		length;			// In: bytes to write, or the size of the buffer for a read. Out: bytes transferred
	FRESULT 		res;			// Out: the FatFS result (FR_OK, FR_NOT_READY if there is no card, ...)
	APP_MSG_EVENT_E	doneEvent;		// The event to send to senderQueue when the operation has finished
	QueueHandle_t	senderQueue;	// The queue that receives the reply. msg_data is this structure, msg_parameter the result
} fileOperation_t;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 */
TaskHandle_t fatfs_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason);

/**
 * @brief Returns the internal state as a number.
 */
uint16_t fatfs_task_getState(void);

/**
 * @brief Returns the internal state as a string.
 */
const char * fatfs_task_getStateString(void);

/**
 * @brief Returns true if the SD card was mounted.
 */
bool fatfs_task_mounted(void);

/**
 * @brief Returns true if the boot count was read, incremented and written back at this boot.
 */
bool fatfs_task_bootCountValid(void);

/**
 * @brief Returns the boot count for this boot. Only meaningful if fatfs_task_bootCountValid().
 */
uint32_t fatfs_task_getBootCount(void);

/**
 * @brief Returns true while the task is carrying out a file operation.
 */
bool fatfs_task_isBusy(void);

/**
 * @brief Prints the state of the card and the boot count.
 */
void fatfs_task_printStatus(void);

/**
 * @brief Asks the task to write or read a file. The reply arrives on fileOp->senderQueue.
 */
bool fatfs_task_sendFileOp(APP_MSG_EVENT_E event, fileOperation_t *fileOp);

/**
 * @brief Tells the task that all tasks are inactive, so it should finish what it is doing and get ready for DPD.
 */
void fatfs_task_notifyInactivity(void);

#ifdef __cplusplus
}
#endif

#endif /* FATFS_TASK_H_ */
