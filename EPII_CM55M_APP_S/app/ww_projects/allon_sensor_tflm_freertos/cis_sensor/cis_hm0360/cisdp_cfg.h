/*
 * cisdp_cfg.h
 *
 *  Created on: 20241118
 *      Author: 901912
 *
 *      HW5X5
 *      RGB: R = W*H byte, G = W*H byte, B = W*H byte
 *      YUV422: Y= W*H byte, U = ((W*H)>>1) byte, V = ((W*H)>>1) byte
 *      YUV420: Y= W*H byte, U = ((W*H)>>2) byte, V = ((W*H)>>2) byte
 *
 *      JPEG
 *      RAW8(YUV400) x4 Compress = ((613+ (W/16)*(H/16)* 64 + 35) >>2 ) <<2 byte
 *      RAW8(YUV400) x10 Compress = ((613+ (W/16)*(H/16)* 24 + 35) >>2 ) <<2 byte
 *      YUV422 x4 Compress = ((623+ (W/16)*(H/16)* 128 + 35) >>2 ) <<2 byte
 *      YUV422 x10 Compress = ((623+ (W/16)*(H/16)* 50 + 35) >>2 ) <<2  byte
 *      YUV420 x4 Compress = ((623+ (W/16)*(H/16)* 96 + 35) >>2 ) <<2  byte
 *      YUV420 x10 Compress = ((623+ (W/16)*(H/16)* 38 + 35) >>2 ) <<2  byte
 *
 *      CDM
 *      Hot Pixel No Pack + No Meta Data: High= W*H byte, Low= W*H byte, Hot Pixel = W*H byte
 *      Hot Pixel Pack + No Meta Data: High= W*H byte, Low= W*H byte, Hot Pixel = W*H/8 byte
 *      Hot Pixel No Pack + Meta Data: High= W*H byte, Low= W*H byte, Hot Pixel = W*H+3*4+H+W+(H<<1) byte
 *      Hot Pixel Pack + Meta Data: High= W*H byte, Low= W*H byte, Hot Pixel = (W*H>>3)+3*4+H+W+(H<<1) byte
 */

#ifndef APP_SCENARIO_CISDP_CFG_H_
#define APP_SCENARIO_CISDP_CFG_H_

#include "hx_drv_gpio.h"
#include "hx_drv_inp.h"

#ifdef TRUSTZONE_SEC
#define EXT_RAM_START   BASE_ADDR_SRAM0_ALIAS
#else
#ifndef TRUSTZONE
#define EXT_RAM_START   BASE_ADDR_SRAM0_ALIAS
#else
#define EXT_RAM_START   BASE_ADDR_SRAM0
#endif
#endif

// #define GROVE_VISION_AI

typedef enum
{
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_1X,
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_2X,
	APP_DP_RES_RGB640x480_INP_SUBSAMPLE_4X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_2X,
	APP_DP_RES_YUV640x480_INP_SUBSAMPLE_4X,
}APP_DP_INP_SUBSAMPLE_E;

#define HM0360_SENSOR_I2CID				(0x24)
#define HM0360_MIPI_CLOCK_FEQ			(168)	// MHz
#define HM0360_MIPI_LANE_CNT			(1)
#define HM0360_MIPI_DPP					(8)		// depth per pixel
#define HM0360_MIPITX_CNTCLK_EN			(1)		// continuous clock output enable
#define DYNAMIC_ADDRESS

#ifdef GROVE_VISION_AI
#define CIS_ENABLE_MIPI_INF				(0x01)	// 0x00: off/0x01: on
#else
#define CIS_ENABLE_MIPI_INF				(0x00)	// 0x00: off/0x01: on
#endif
#define CIS_MIRROR_SETTING				(0x00)	// 0x00: off/0x01:H-Mirror/0x02:V-Mirror/0x03:HV-Mirror
#define CIS_I2C_ID						(HM0360_SENSOR_I2CID)
#define CIS_ENABLE_HX_AUTOI2C			(0x00)	// 0x00: off/0x01: on/0x2: on and XSLEEP KEEP HIGH
#define CIS_ENABLE_XSLEEP_TRIG_FRM		(0x00)
#define DEAULT_XHSUTDOWN_PIN    		AON_GPIO2

#define HM0360_INT_INDC_REG				0x2064
#define HM0360_INT_CLEAR_REG			0x2065
#define HM0360_MD_INT_BIT				0x08

/*
 * SENDPLIB PARAMETERS
 */
#define SENDPLIB_PERIODIC_TIMER_MS   		10
#define SENDPLIB_WDMA_START_ADDR			(EXT_RAM_START+0x30000)
#define SENDPLIB_WDMA1_ADDR					SENDPLIB_WDMA_START_ADDR
#define SENDPLIB_WDMA2_ADDR					(SENDPLIB_WDMA1_ADDR+0x5F400)
#define SENDPLIB_WDMA3_ADDR					(SENDPLIB_WDMA2_ADDR+0x4B000)
#define SENDPLIB_JPEG_YUV400_AUTOFILL_ADDR	(SENDPLIB_WDMA3_ADDR+640*480)
#define SENDPLIB_JPEG_YUV420_AUTOFILL_ADDR	(SENDPLIB_WDMA3_ADDR+640*480+640*480/2)
#define SENDPLIB_JPEG_YUV422_AUTOFILL_ADDR	(SENDPLIB_WDMA3_ADDR+640*480*2)
#define SENDPLIB_JPEG_RGB_AUTOFILL_ADDR		(SENDPLIB_WDMA3_ADDR+640*480*3)


/*
 * DP SENCTRL CFG
 */
#ifdef GROVE_VISION_AI
#define SENCTRL_SENSOR_TYPE			SENSORDPLIB_SENSOR_HM2170_MIPI
#define SENCTRL_STREAM_TYPE			SENSORDPLIB_STREAM_NONEAOS
#else
// #define SENCTRL_SENSOR_TYPE			SENSORDPLIB_SENSOR_HM0360_MODE3
#define SENCTRL_SENSOR_TYPE			SENSORDPLIB_SENSOR_HM0360_MODE1
#define SENCTRL_STREAM_TYPE			SENSORDPLIB_STREAM_NONEAOS
#endif

#if (CIS_ENABLE_HX_AUTOI2C == 0x01)
#define SENCTRL_STREAM_TYPE			SENSORDPLIB_STREAM_NONEAOS_AUTOI2C
#elif (CIS_ENABLE_HX_AUTOI2C == 0x02)
#define SENCTRL_STREAM_TYPE			SENSORDPLIB_STREAM_NONEAOS_AUTOI2C_XSLEEP_HIGH
#endif
#define SENCTRL_SENSOR_WIDTH 		640
#define SENCTRL_SENSOR_HEIGHT 		480
#define SENCTRL_SENSOR_CH	 		3


/*
 * DP INP CFG
 *
 * SENSOR --> INP_CROP --> INP_BINNING --> INP_SUBSAMPLE
 *
 * CROP DISABLE: DP_INP_CROP_START_X/DP_INP_CROP_START_Y/DP_INP_CROP_WIDTH/DP_INP_CROP_HEIGHT all 0
 */
#define DP_INP_CASE					2
#if(DP_INP_CASE == 1)
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_DISABLE
#elif(DP_INP_CASE == 2)
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_4TO2
#elif(DP_INP_CASE == 3)
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_8TO2
#else
#define DP_INP_SUBSAMPLE			INP_SUBSAMPLE_DISABLE
#endif

#define DP_INP_BINNING				INP_BINNING_DISABLE
#define DP_INP_CROP_START_X			0
#define DP_INP_CROP_START_Y			0
#define DP_INP_CROP_WIDTH          	0
#define DP_INP_CROP_HEIGHT         	0
#if (DP_INP_CASE == 1)
#define DP_INP_OUT_WIDTH 		    SENCTRL_SENSOR_WIDTH
#define DP_INP_OUT_HEIGHT 		    SENCTRL_SENSOR_HEIGHT
#elif(DP_INP_CASE == 2)
#define DP_INP_OUT_WIDTH 		    320
#define DP_INP_OUT_HEIGHT 		    240
#elif(DP_INP_CASE == 3)
#define DP_INP_OUT_WIDTH 		    160
#define DP_INP_OUT_HEIGHT 		    120
#else
#define DP_INP_OUT_WIDTH 		    SENCTRL_SENSOR_WIDTH
#define DP_INP_OUT_HEIGHT 		    SENCTRL_SENSOR_HEIGHT
#endif

#define DP_INP_1X_SUBSAMPLE			INP_SUBSAMPLE_DISABLE
#define DP_INP_1X_OUT_WIDTH 	    640
#define DP_INP_1X_OUT_HEIGHT	    480


/*
 * DP HW2X2 CFG
 *
 * LIMITATION:
 * 2X2 SUBSAMPLE LT,LB,RT,RBBINNING (WIDTH/2, HEIGHT/2, MIN:2X2, MAX:640X480)
 * 1/3 SUBSAMPLE (WIDTH/3, HEIGHT/3, MIN:3X3, MAX:639X480)
 */
#define DP_HW2X2_PATH				HW2x2_PATH_THROUGH
#define DP_HW2X2_PROCESS_MODE		HW2x2_MODE_UNITY
#define DP_HW2X2_CROP_START_X		0
#define DP_HW2X2_CROP_START_Y		0
#define DP_HW2X2_CROP_WIDTH			DP_INP_OUT_WIDTH
#define DP_HW2X2_CROP_HEIGHT		DP_INP_OUT_HEIGHT
#define DP_HW2X2_ROUND_MODE			HW2x2_ROUNDMODE_FLOOR
#define DP_HW2X2_OUT_WIDTH          (DP_INP_OUT_WIDTH)
#define DP_HW2X2_OUT_HEIGHT         (DP_INP_OUT_HEIGHT)


/*
 * DP CDM CFG
 *
 * LIMITATION:
 * MAX: 480X270
 * MIN: 8X3
 * WIDTH/8
 */
#define DP_CDM_ENABLE				CDM_ENABLE_ON
#define DP_CDM_IN_START_X			0
#define DP_CDM_IN_START_Y			0
#define DP_CDM_IN_WIDTH 			DP_HW2X2_OUT_WIDTH
#define DP_CDM_IN_HEIGHT			DP_HW2X2_OUT_HEIGHT
#define DP_CDM_META_DUMP 			CDM_ENABLE_MATA_DUMP_ON
#define DP_CDM_HT_PACKING 			CDM_ENABLE_HT_PACKING_ON
#define DP_CDM_MIN_ALLOW_DIS 		3
#define DP_CDM_TOLERANCE 			3
#define DP_CDM_REACTANCE 			2
#define DP_CDM_RELAXATION 			1
#define DP_CDM_EROS_TH 				3
#define DP_CDM_NUM_HT_TH 			10
#define DP_CDM_NUM_HT_VECT_TH_X 	8
#define DP_CDM_NUM_HT_VECT_TH_Y 	4
#define DP_CDM_NUM_CONS_HT_BIN_TH_X 1
#define DP_CDM_NUM_CONS_HT_BIN_TH_Y 1
#define DP_CDM_CPU_ACTIVEFLAG 		CDM_CPU_ACTFLAG_ACTIVE
#define DP_CDM_INIT_MAP_FLAG 		CDM_INIMAP_FLAG_ON_ONEFRAME_ONLY


/*
 * DP HW5X5 CFG
 *
 * LIMITATION:
 * MIN: 8X8
 * WIDTH/8
 * HEIGHT/4
 */
#define DP_HW5X5_PATH				HW5x5_PATH_THROUGH_DEMOSAIC
#define DP_HW5X5_DEMOS_BNDMODE		DEMOS_BNDODE_REFLECT
#define DP_HW5X5_DEMOS_COLORMODE	DEMOS_COLORMODE_YUV420

#if (CIS_MIRROR_SETTING == 0x01)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_GBRG
#elif (CIS_MIRROR_SETTING == 0x02)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_GRBG
#elif (CIS_MIRROR_SETTING == 0x03)
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_RGGB
#else
#define DP_HW5X5_DEMOS_PATTERN		DEMOS_PATTENMODE_BGGR
#endif

#define DP_HW5X5_DEMOSLPF_ROUNDMODE DEMOSLPF_ROUNDMODE_FLOOR
#define DP_HW5X5_CROP_START_X 		0
#define DP_HW5X5_CROP_START_Y 		0
#if (DP_INP_CASE == 1)
#define DP_HW5X5_CROP_WIDTH 		640
#define DP_HW5X5_CROP_HEIGHT 		480
#define DP_HW5X5_OUT_WIDTH 			640
#define DP_HW5X5_OUT_HEIGHT 		480
#elif(DP_INP_CASE == 2)
#define DP_HW5X5_CROP_WIDTH 		320
#define DP_HW5X5_CROP_HEIGHT 		240
#define DP_HW5X5_OUT_WIDTH 			320
#define DP_HW5X5_OUT_HEIGHT 		240
#elif(DP_INP_CASE == 3)
#define DP_HW5X5_CROP_WIDTH 		160
#define DP_HW5X5_CROP_HEIGHT 		120
#define DP_HW5X5_OUT_WIDTH 			160
#define DP_HW5X5_OUT_HEIGHT 		120
#else
#define DP_HW5X5_CROP_WIDTH 		640
#define DP_HW5X5_CROP_HEIGHT 		480
#define DP_HW5X5_OUT_WIDTH 			640
#define DP_HW5X5_OUT_HEIGHT 		480
#endif
#define DP_HW5X5_1X_CROP_WIDTH 		640
#define DP_HW5X5_1X_CROP_HEIGHT 	480
#define DP_HW5X5_1X_OUT_WIDTH 		640
#define DP_HW5X5_1X_OUT_HEIGHT 		480

/*
 * DP JPEG CFG
 *
 * LIMITATION:
 * MAX:640X640
 * MIN: 16X16
 * WIDTH/16
 * HEIGHT/16
 */
#define DP_JPEG_PATH				JPEG_PATH_ENCODER_EN
#define DP_JPEG_ENC_WIDTH 			DP_HW5X5_OUT_WIDTH
#define DP_JPEG_ENC_HEIGHT 			DP_HW5X5_OUT_HEIGHT
#define DP_JPEG_ENCTYPE 			JPEG_ENC_TYPE_YUV420
#define DP_JPEG_ENCQTABLE 			JPEG_ENC_QTABLE_10X


#endif /* APP_SCENARIO_CISDP_CFG_H_ */
