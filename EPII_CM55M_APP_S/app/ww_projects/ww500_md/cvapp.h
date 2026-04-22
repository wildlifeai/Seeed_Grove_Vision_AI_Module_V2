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

#include "ww500_md.h"
#include "xip_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Project ID (last 4 digits only) for model naming convention: <last4digits>V<version>.tfl
// Example: for project ID 2782, the model file would be named 2782V24.tfl (where 24 is the version)
// Set this value to match your Edge Impulse project's last 4 digits
#define PROJECT_ID 0
#define PROJECT_VER 0
// Logit value (0-127)
#define MODEL_THRESHOLD 15

// Enable/disable transforming of tensor output to percentages
// Uncomment this to use percentages
// CGP 24/1/26 - Disable this. Use logits instead
// When debugged, remove this code
//#define USE_PERCENTAGE

#ifdef USE_PERCENTAGE

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

#endif // USE_PERCENTAGE

/********************************** Public Functions Declarations *************************************/

int cv_init(bool security_enable, bool privilege_enable, uint16_t project_id, uint16_t deploy_version, APP_WAKE_REASON_E woken);
int cv_deinit(void);

void cv_get_model_info(int *project_id, int *deploy_version);
// Update current model identifiers (used by unzip when discovering a TFL filename)
void cv_set_model_info(int project_id, int deploy_version);

// CGP I am asking the NN processing to return an array
TfLiteStatus cv_run(int8_t *outCategories, uint8_t *categoriesCount) ;

#ifdef USE_PERCENTAGE
// Get the most recent confidence scores with labels
bool cv_get_confidence_data(ClassConfidenceData *data);
#else
const char * cv_getLabel(uint8_t index);
#endif // USE_PERCENTAGE

// App seeks to erase the start of the model XIP flash area
void cv_eraseModel(void);

// App seeks to load a new model
void cv_newModel(uint16_t project_id, uint16_t deploy_version);

// True if a model is ready to be used
bool cv_modelLoaded(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_ */
