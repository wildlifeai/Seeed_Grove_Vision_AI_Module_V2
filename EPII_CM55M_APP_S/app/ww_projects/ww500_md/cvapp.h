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

#define MODEL_NUMBER 00002 // Model number used for the NN model
#define MODEL_MAGIC_HEADER 0xDEADBEEF // Magic number to identify valid model

// Flash configuration - based on memory map
#define FLASH_START_SAFE_ADDR   0x00180000  // Physical flash address after 2MB reserved for firmware
#define FLASH_MODEL_AREA_SIZE   (14 * 1024 * 1024)  // 14MB available for models
#define FLASH_SECTOR_SIZE       4096        // Typical sector size, adjust if needed
#define MODEL_FLASH_ADDR        FLASH_START_SAFE_ADDR

// Memory mapping - convert physical flash address to virtual CPU address
#define FLASH_VIRTUAL_BASE      0x3A000000  // Virtual base address for flash memory
#define FLASH_PHYSICAL_BASE     0x00000000  // Physical base address for flash memory
// Convert physical flash address to virtual address for XIP access
#define FLASH_PHYS_TO_VIRT(addr) ((addr) - FLASH_PHYSICAL_BASE + FLASH_VIRTUAL_BASE)

int cv_init(bool security_enable, bool privilege_enable);

// CGP I am asking the NN processing to return an array
//int cv_run();
TfLiteStatus cv_run(int8_t * outCategories, uint16_t categoriesCount);

int cv_deinit();
#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_CVAPP_ */
