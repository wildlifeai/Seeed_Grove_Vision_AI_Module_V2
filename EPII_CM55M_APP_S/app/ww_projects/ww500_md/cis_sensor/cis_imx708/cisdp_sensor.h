/*
 * cisdp_sensor.h
 *
 *  Created on: 20240206
 *      Author: 901912
 */

#ifndef APP_SCENARIO_CISDP_SENSOR_H_
#define APP_SCENARIO_CISDP_SENSOR_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "cisdp_cfg.h"
#include "WE2_device.h"
#include "sensor_dp_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \enum CISDP_INIT_TYPE_E
 * \brief
 */
typedef enum CISDP_INIT_TYPE_S
{
	CISDP_INIT_TYPE_NONE			= 0x00,
	CISDP_INIT_TYPE_VIDEO_STREAM	= 0x01,			/*For CPU usage*/
	CISDP_INIT_TYPE_AOS				= 0x02,			/*For PMU USAGE*/
} CISDP_INIT_TYPE_E;

int cisdp_sensor_init(bool sensor_init);
int cisdp_dp_init(bool inp_init, SENSORDPLIB_PATH_E dp_type, sensordplib_CBEvent_t cb_event, uint32_t jpg_ratio, APP_DP_INP_SUBSAMPLE_E subs);
void cisdp_sensor_start();
void cisdp_sensor_stop();
void cisdp_stream_on();
void cisdp_stream_off();
void set_mipi_csirx_disable();
void set_mipi_csirx_enable();
void cisdp_get_jpginfo(uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr);

uint32_t app_get_jpeg_addr();
uint32_t app_get_raw_addr();
uint32_t app_get_raw_sz();
uint32_t app_get_raw_width();
uint32_t app_get_raw_height();
uint32_t app_get_raw_channels();

/*
 * High-resolution RAW capture support (hires.c). Register a nonzero raw
 * buffer address BEFORE cisdp_sensor_init() to make the sensor stream a
 * centred 1280x960 window (digital crop) and switch the datapath to
 * INP pass-through -> RAW Bayer -> WDMA2 at that address, bypassing
 * HW5x5/JPEG. Register 0 to restore the normal VGA flow.
 */
void cisdp_set_hires_raw(uint32_t rawAddr);

/* Frame-timeout forensics: dump sensor / CSI-RX / INP state on the console
 * (called from the image task's capture-retry path while everything is
 * still powered). */
void cisdp_dump_diag(void);

/* Point the shared CIS I2C bus at the main camera (the HM0360 MD companion
 * can leave the slave ID at itself). Call before writing sensor registers
 * from outside the driver (e.g. the AE loop). */
void cisdp_select_main_camera_i2c(void);

/* CFA phase of the demosaic, as a demosaic_pattern_t value (demosaic.h) */
uint8_t cisdp_get_demos_pattern(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_CISDP_SENSOR_H_ */
