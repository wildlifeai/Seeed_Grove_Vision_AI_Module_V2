/*
 * cisdp_sensor.h
 *
 *  Created on: 2022/11/18
 *      Author: 901912
 */

#ifndef APP_SCENARIO_CISDP_SENSOR_H_
#define APP_SCENARIO_CISDP_SENSOR_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "WE2_device.h"
#include "sensor_dp_lib.h"
#include "cisdp_cfg.h"
#include "hx_drv_scu.h"

#ifdef __cplusplus
extern "C" {
#endif
int cisdp_sensor_init(bool sensor_init);
int cisdp_dp_init(bool inp_init, SENSORDPLIB_PATH_E dp_type, sensordplib_CBEvent_t cb_event, uint32_t jpg_ratio, APP_DP_INP_SUBSAMPLE_E subs);
void cisdp_sensor_start();
void cisdp_sensor_stop();
void cisdp_get_jpginfo(uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr);

int cisdp_sensor_md_init(void);

uint32_t app_get_jpeg_addr();
uint32_t app_get_jpeg_sz();
uint32_t app_get_raw_addr();
uint32_t app_get_raw_sz();
uint32_t app_get_raw_width();
uint32_t app_get_raw_height();
uint32_t app_get_raw_channels();

// New for combined jpeg and exif buffer
uint32_t app_get_jpeg_exif_addr();
uint32_t app_get_jpeg_exif_size();

#ifdef __cplusplus
}
#endif

#endif /* APP_SCENARIO_CISDP_SENSOR_H_ */
