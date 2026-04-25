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
#include "directory_manager.h"
#include "cvapp.h"

#ifdef __cplusplus
extern "C" {
#endif


/**************************************** Global Defines  *************************************/

// TODO Experimental: set a limit on the name of files
#define	FNAMELEN 16
// required length for a 128-bit UUID (including trailing '\0')
#define	UUIDLENGTH	37

#define DEPLOYMENT_ID_ZERO_UUID "00000000-0000-0000-0000-000000000000"

// Uncomment this to include the unzipping code
// See error report 12/01/26 in MANIFEST_info.md
//#define UNZIPMANIFEST

/**************************************** Type declarations  *************************************/

// Operational parameters to get/set.
// Typically the values are saved to SD card before entering DPD
// OP_PARAMETER_NUM_ENTRIES is only used to establish the number of entries
// IMPORTANT: If this list is changed then it must be changed in the MKL62BA code also in aiProcessor.h
// IMPORTANT: ensure default values are set in vFatFsTask()

/*
 * This enum enumerates the index numbers of the Operational Parameters array, op_parameter[]
 *
 */
typedef enum {
	OP_PARAMETER_SEQUENCE_NUMBER,	// 0 Image file number. Used as part of the image file name. Increments when the file is written.
	OP_PARAMETER_NUM_NN_ANALYSES,	// 1 The number of times the neural network model has run.
	OP_PARAMETER_NUM_POSITIVE_NN_ANALYSES,	// 2 The number of times the neural network model detects the target.
	OP_PARAMETER_NUM_COLD_BOOTS,	// 3 The number of AI processor cold boots.
	OP_PARAMETER_NUM_WARM_BOOTS,	// 4 The number of AI processor warm boots.
	OP_PARAMETER_NUM_PICTURES,		// 5 The number of images to capture each time the processor receives a motion detect event or a time lapse event.
	OP_PARAMETER_PICTURE_INTERVAL,	// 6 The interval (in ms) between each of the above images. Limited to about 2000 for HM0360
	OP_PARAMETER_TIMELAPSE_INTERVAL,// 7 The interval (in s) between entering DPD and waking again to take the next timelapse image (0 inhibits)
	OP_PARAMETER_INTERVAL_BEFORE_DPD, // 8 The interval (in ms) between when all FreeRTOS task activity ceases and the AI processor entering DPD.
	OP_PARAMETER_LED_BRIGHTNESS_PERCENT,	// 9 Flash LED duty cycle (brightness) in percent (approximately, 0 means 'dim', not 'off')
	OP_PARAMETER_CAMERA_ENABLED,	// 10 0 = Camera and NN system disabled, 1 = Camera and NN system enabled
	OP_PARAMETER_MD_INTERVAL,		// 11 Interval (ms) between frames in motion detect mode (0 inhibits motion detection)
	OP_PARAMETER_FLASH_DURATION,	// 12 Duration (ms) that LED flash is on
	OP_PARAMETER_FLASH_LED,			// 13 Flash LED bit mask: visible LED used = 1, infra-red LED used =2, none = 0
	OP_PARAMETER_MODEL_PROJECT,		// 14 Model project ID used for the NN model
	OP_PARAMETER_MODEL_VERSION,		// 15 Model version number used for the NN model
	OP_PARAMETER_MODEL_THRESHOLD,	// 16 Logit threshold for detection (0-127)
	OP_PARAMETER_MD_SENSITIVITY,	// 17 Motion Detection Sensitivity: 0=off, 1=low, 2=medium, 3=high
	OP_PARAMETER_TEST_MODE_BITS,	// 18 To manage test configurations: bit or bits indicate a test function
	OP_PARAMETER_IMAGES_COUNT,		// 19 Count of images in the current image folder. Use this to decide to create a new image folder.
	OP_PARAMETER_IMAGES_FILE_INDEX,	// 20 Count of image folders

	OP_PARAMETER_NUM_ENTRIES		// Not an Operational Parameters - serves to define the size of the op_parameter[] array
} OP_PARAMETERS_E;

/**
 * Defines bits which might be used in tests
 *
 * Set one or more bits to invoke the test.
 *
 * This allows tests to be turned on and off via the app, without needing re-compilation
 * on order to run the test. To get a sensible test, other operational parameters might need to be changed as well.
 */
typedef enum {
	TEST_BIT_TONE_MAPPING = (1 << 0),	// Select a new tone after each image. Set OP_PARAMETER_NUM_PICTURES to 4
	TEST_BIT_SAVE_BMP = (1 << 1),			// Alternate between JPG and BMP files. Set OP_PARAMETER_NUM_PICTURES to an even number
	TEST_BIT_FLASH_BRIGHTNESS = (1 << 2),	// increment LED flash with every picture. Set OP_PARAMETER_NUM_PICTURES to 7 and select OP_PARAMETER_FLASH_LED to 1 or 2
	TEST_BIT_SKIP_FILE_CREATION = (1 << 3),	// Don't save images to disk. Still streams MD and AE data to app.
											// Consider making OP_PARAMETER_NUM_PICTURES = a large number and OP_PARAMETER_PICTURE_INTERVAL = 1
} TEST_MODE_BITS_E;

/**
 *
Preliminary discussions between developers suggest that further operational parameters should be added,
to further control the operation of the WW500. These might include:

	OP_PARAMETER_NN_THRESHOLD		// NN output value (percentage) that determines of an image is saved to SD card
	OP_PARAMETER_IMAGE_RESOLUTION	// Value determines the size of image saved to SD card  (options t.b.d. e.g. 640x480, 1280x960)
	OP_PARAMETER_NN_INDEX			// Index of NN model to deploy (0 means no NN processing).
	OP_PARAMETER_NN_NUM_CHANNELS	// Number of channels used in the NN model (e.g. 3 for RGB)
	OP_PARAMETER_NN_X_RESOLUTION	// Camera image is scaled to this X resolution to provide to the NN
	OP_PARAMETER_NN_Y_RESOLUTION	// Camera image is scaled to this Y resolution to provide to the NN

 */

// The states for the fatfs_task
// APP_FATFS_STATE_NUMSTATES is only used to establish the number of states
typedef enum {
	APP_FATFS_STATE_UNINIT						=0x0000,
	APP_FATFS_STATE_IDLE						=0x0001,
	APP_FATFS_STATE_BUSY						=0x0002,
	APP_FATFS_STATE_NUMSTATES					=0x0003
} APP_FATFS_STATE_E;

// Supplementary structure when there are multiple buffers to write to a file
typedef struct {
	uint8_t *	buffer;		// Pointer to the buffer containing file contents
	uint32_t 	length;		// Number of bytes to write or read
} fileBufferInfo_t;

// Structure to use for file operations:
// Initially for reading and writing a file
typedef struct {
	char *		fileName;
	uint8_t *	buffer;		// Pointer to the buffer containing file contents
	uint32_t 	length;		// Number of bytes to write or read
	FRESULT 	res;		// Result code returned from fatFs
	bool		closeWhenDone;	// If true the file is closed when the operation completes
	bool		unmountWhenDone;	// If true the SD card is unmounted when the operation completed
	bool		deleteOnClose;	// If true the file is deleted after closing (used by CLOSE_FILE on error)
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

// Get deployment ID UUID string (prefers 'I ' line form; falls back to OP20-OP27 chunks)
void fatfs_getDeploymentId(char *deployment_id_buffer, size_t buffer_size);

// Set deployment ID UUID string (persisted to CONFIG.TXT on next save_configuration())
void fatfs_setDeploymentId(const char *uuid_string);

// Load labels from SD card text file
int8_t fatfs_load_labels(const char *path, char labels[][MAX_LABEL_LEN], uint8_t *label_count, uint8_t max_labels, uint8_t max_label_len);

void fatfs_printCwd(void);

// Recursive directory create
FRESULT fatfs_mkdir_recursive(const char *path);

#ifdef UNZIPMANIFEST
// Unzip Manifest.zip (method 0 STORE only - no compression)
int fatfs_unzip_manifest(void);
#endif // UNZIPMANIFEST

#ifdef __cplusplus
}
#endif

#endif /* APP_WW_PROJECTS_WW500_MD_FATFS_TASK_H_ */
