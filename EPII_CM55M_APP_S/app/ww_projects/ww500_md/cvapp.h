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

// Maximum number of classes supported
#define MAX_CLASSES 16

    // Structure to hold class confidence scores
    typedef struct
    {
        int class_count;                     // Number of classes
        int confidence_percent[MAX_CLASSES]; // Confidence as integer percentage (0-100)
        const char *labels[MAX_CLASSES];     // Pointers to label strings (or NULL)
    } ClassConfidenceData;

// Flash configuration - based on memory map
#define FLASH_START_SAFE_ADDR   0x00200000  // Physical flash address after 2MB reserved for firmware
#define FLASH_MODEL_AREA_SIZE   (14 * 1024 * 1024)  // 14MB available for models
#define FLASH_SECTOR_SIZE       4096        // Typical sector size, adjust if needed
#define MODEL_FLASH_ADDR        FLASH_START_SAFE_ADDR

// Memory mapping - convert physical flash address to virtual CPU address
#define FLASH_VIRTUAL_BASE 0x3A000000  // Virtual base address for flash memory
#define FLASH_PHYSICAL_BASE 0x00000000 // Physical base address for flash memory

    static inline uint32_t virt_to_phys(uint32_t virt)
    {
        return virt - FLASH_VIRTUAL_BASE + FLASH_PHYSICAL_BASE;
    }
    static inline uint32_t phys_to_virt(uint32_t phys)
    {
        return phys - FLASH_PHYSICAL_BASE + FLASH_VIRTUAL_BASE;
    }

    int cv_init(bool security_enable, bool privilege_enable, int project_id, int deploy_version);
    void cv_get_model_info(int *project_id, int *deploy_version);
    // Update current model identifiers (used by unzip when discovering a TFL filename)
    void cv_set_model_info(int project_id, int deploy_version);
    int get_model_id();

    // CGP I am asking the NN processing to return an array
    TfLiteStatus cv_run(int8_t *outCategories, uint16_t categoriesCount);

    // Get the most recent confidence scores with labels
    bool cv_get_confidence_data(ClassConfidenceData *data);

    // int load_model_cli_command(int model_selection);

    int cv_deinit();
#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_ */
