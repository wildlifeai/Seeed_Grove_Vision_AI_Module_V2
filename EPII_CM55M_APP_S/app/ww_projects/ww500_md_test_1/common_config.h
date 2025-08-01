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
 *		in this example, model data is "person_detect_model_data_vela.cc" file.
 *
 *	1: model file will off-line burn to dedicated location in flash,
 *		use flash memory mapped address to load model.
 *		in this example, model data is pre-burn to flash address: 0x180000
 * **/
#define FLASH_XIP_MODEL             0
#define MEM_FREE_POS                (BOOT2NDLOADER_BASE)

#define SUPPORT_FATFS               1       // 0 : send images via SPI, 1 : save images to SD card
#define ENTER_SLEEP_MODE            1       // 0 : always on, 1 : enter Sleep mode
#define SENSOR_AE_STABLE_CNT		3
#define ENTER_PMU_MODE_FRAME_CNT	3

// delay in milli-seconds
#define DELAYBETWEENPICS	1000

#endif /* APP_SCENARIO_ALLON_SENSOR_TFLM_COMMON_CONFIG_H_ */
