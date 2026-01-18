/*
 * cvapp.h
 *
 *  Created on: 2018�~12��4��
 *      Author: 902452
 */

#ifndef APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_
#define APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_

#include "spi_protocol.h"
#include "c_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif


// Flash configuration - based on memory map
#define FLASH_START_SAFE_ADDR   0x00200000  // Physical flash address after 2MB reserved for firmware
#define FLASH_MODEL_AREA_SIZE   (14 * 1024 * 1024)  // 14MB available for models
#define FLASH_SECTOR_SIZE       4096        // Typical sector size, adjust if needed
#define MODEL_FLASH_ADDR        FLASH_START_SAFE_ADDR

// Memory mapping - convert physical flash address to virtual CPU address
#define FLASH_VIRTUAL_BASE 0x3A000000  // Virtual base address for flash memory
#define FLASH_PHYSICAL_BASE 0x00000000 // Physical base address for flash memory

// CGP - TODO - how does MODEL_XIP_ADDR relate to FLASH_VIRTUAL_BASE?
#define MODEL_XIP_ADDR 			0x3A200000
#define MODEL_XIP_INFO_SIZE 	16		// Must be divisible by 4

// Maximum number of classes supported
#define MAX_CLASSES 			16
#define MAX_LABEL_LEN 			20	// e.g. 'no person\0'

#if 1
// Structure to hold class confidence scores
typedef struct {
	// 84 bytes
	const char *labels[MAX_CLASSES];     // Pointers to label strings (or NULL)
	uint8_t confidence_percent[MAX_CLASSES]; // Confidence as integer percentage (0-100)
	uint8_t class_count;                     // Number of classes
} ClassConfidenceData;
#else
typedef struct {
	// 132 bytes
	int class_count;                     // Number of classes
	int confidence_percent[MAX_CLASSES]; // Confidence as integer percentage (0-100)
	const char *labels[MAX_CLASSES];     // Pointers to label strings (or NULL)
} ClassConfidenceData;

#endif

/********************************** Public Functions Declarations *************************************/

int cv_init(bool security_enable, bool privilege_enable, int project_id, int deploy_version);
int cv_deinit(void);

void cv_get_model_info(int *project_id, int *deploy_version);
// Update current model identifiers (used by unzip when discovering a TFL filename)
void cv_set_model_info(int project_id, int deploy_version);

// CGP I am asking the NN processing to return an array
TfLiteStatus cv_run(int8_t *outCategories, uint16_t categoriesCount);

// Get the most recent confidence scores with labels
bool cv_get_confidence_data(ClassConfidenceData *data);

#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_ */
