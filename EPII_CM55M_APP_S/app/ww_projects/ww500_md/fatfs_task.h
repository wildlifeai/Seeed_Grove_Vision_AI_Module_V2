/*
 * fatfs_task.h
 *
 *  Created on: 19 Aug 2024
 *      Author: CGP
 *
 *      https://github.com/FreeRTOS/FreeRTOS/blob/520fc225eb2dd5e21c951ca325e1c51eed3a5c13/FreeRTOS-Plus/Demo/Common/FreeRTOS_Plus_CLI_Demos/File-Related-CLI-commands.c
 *      https://github.com/FreeRTOS/FreeRTOS/blob/520fc225eb2dd5e21c951ca325e1c51eed3a5c13/FreeRTOS-Plus/Demo/Common/FreeRTOS_Plus_FAT_SL_Demos/CreateExampleFiles/File-system-demo.c
 *      https://github.com/FreeRTOS/Lab-Project-FreeRTOS-FAT
 *      https://www.freertos.org/Documentation/03-Libraries/05-FreeRTOS-labs/04-FreeRTOS-plus-FAT/file_related_cli_commands
 *
 */

#ifndef APP_WW_PROJECTS_WW500_MD_FATFS_TASK_H_
#define APP_WW_PROJECTS_WW500_MD_FATFS_TASK_H_

/********************************** Includes ******************************************/

#include <stdbool.h>
#include "ff.h"
#include "ww500_md.h"

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "queue.h"

/**************************************** Global Defines  *************************************/

// TODO Experimental: set a limit on the name of files
#define	FNAMELEN 16

/**************************************** Type declarations  *************************************/

// Operational parameters to get/set.
// Typically the values are saved to SD card before entering DPD
// OP_PARAMETER_NUM_ENTRIES is only used to establish the number of entries
// IMPORTANT: If this list is changed then it must be changed in the MKL62BA code also
typedef enum {
	OP_PARAMETER_SEQUENCE_NUMBER,	// 0 Image file number
	OP_PARAMETER_NUM_NN_ANALYSES,	// 1 # times the NN model has run
	OP_PARAMETER_NUM_POSITIVE_NN_ANALYSES,	// 2 # times the NN model says "yes"
	OP_PARAMETER_NUM_COLD_BOOTS,	// 3 # of AI processor cold boots
	OP_PARAMETER_NUM_WARM_BOOTS,	// 4 # of AI processor warm boots
	OP_PARAMETER_NUM_PICTURES,		// 5 Num pics when triggered
	OP_PARAMETER_PICTURE_INTERVAL,	// 6 Pic interval when triggered (ms)
	OP_PARAMETER_TIMELAPSE_INTERVAL,// 7 Interval (s) (0 inhibits)
	OP_PARAMETER_INTERVAL_BEFORE_DPD, // 8 Delay before DPD (ms)
	OP_PARAMETER_LED_BRIGHTNESS_PERCENT,	// 9 in percent (0 inhibits)
	OP_PARAMETER_CAMERA_ENABLED,	// 10 0 = disabled, 1 = enabled
	OP_PARAMETER_MD_INTERVAL,		// 11 Interval (ms) between frames in MD mode (0 inhibits)
	OP_PARAMETER_FLASH_DURATION,	// 12 Duration (ms) that LED Flash is on
	OP_PARAMETER_NUM_ENTRIES		// Count of entries above here
} OP_PARAMETERS_E;

// The states for the fatfs_task
// APP_FATFS_STATE_NUMSTATES is only used to establish the number of states
typedef enum {
	APP_FATFS_STATE_UNINIT						=0x0000,
	APP_FATFS_STATE_IDLE						=0x0001,
	APP_FATFS_STATE_BUSY						=0x0002,
	APP_FATFS_STATE_NUMSTATES					=0x0003
} APP_FATFS_STATE_E;

// Structure to use for file operations:
// Initially for reading and writing a file
typedef struct {
	char *		fileName;
	uint8_t *	buffer;		// Pointer to the buffer containing file contents
	uint32_t 	length;		// Number of bytes to write or read
	FRESULT 	res;		// Result code returned from fatFs
	bool		closeWhenDone;	// If true the file is closed when the operation completes
	bool		unmountWhenDone;	// If true the SD card is unmounted when the operation completed
	QueueHandle_t senderQueue;	// FreeRTOS queue that will get the response
} fileOperation_t;

/**************************************** Global routine declarations  *************************************/

TaskHandle_t fatfs_createTask(int8_t priority, APP_WAKE_REASON_E wakeReason);

uint16_t fatfs_getState(void);

bool fatfs_mounted(void);

const char * fatfs_getStateString(void);

// Get one of the Operational Parameters
uint16_t fatfs_getOperationalParameter(OP_PARAMETERS_E parameter);

// Set one of the operational parameters
void fatfs_setOperationalParameter(OP_PARAMETERS_E parameter, int16_t value);

uint16_t fatfs_getImageSequenceNumber(void);

// Increment one of the Operational Parameters
void fatfs_incrementOperationalParameter(OP_PARAMETERS_E parameter);

#endif /* APP_WW_PROJECTS_WW500_MD_FATFS_TASK_H_ */
