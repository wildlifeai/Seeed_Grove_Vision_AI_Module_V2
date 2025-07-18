/*
 * common_config.h
 *
 *  Created on: Nov 22, 2022
 *      Author: bigcat-himax
 */

#ifndef APP_SCENARIO_ALLON_SENSOR_TFLM_COMMON_CONFIG_H_
#define APP_SCENARIO_ALLON_SENSOR_TFLM_COMMON_CONFIG_H_

/** MODEL location:
 *	0: model file is a c file which will locate to memory.
 *
 *	1: model file will off-line burn to dedicated location in flash,
 *		use flash memory mapped address to load model.
 * **/
#define FLASH_XIP_MODEL 1
#define MEM_FREE_POS (BOOT2NDLOADER_BASE)

#define SUPPORT_FATFS 1    // 0 : send images via SPI, 1 : save images to SD card
#define ENTER_SLEEP_MODE 0 // 0 : always on, 1 : enter Sleep mode
#define SENSOR_AE_STABLE_CNT 4
#define ENTER_PMU_MODE_FRAME_CNT 2

// Time to sleep between taking pictures
#define APP_SLEEP_INTERVAL 10000

// Secure base address is for flash(XIP) is 0x3A000000
// Size of rat model is: 72752 bytes = 0x11C10
// Address set at: 0x3AB11C10 and called as memory region parameter: 0xB11C10
// #define RAT_DETECTION_MODEL_ADDR 0x3AB11C10
// Address set at virtual address: 0x3A200000 and called as physical memory region parameter: 0x200000
#define RAT_DETECTION_MODEL_ADDR 0x3A200000

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_COMMON_CONFIG_H_ */
