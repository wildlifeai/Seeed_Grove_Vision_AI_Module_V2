/*
 * hm0360_md.h
 *
 * Supports HM0360 for motion detection, when it is used with other cameras.
 *
 *  Created on: 8 Jul 2025
 *      Author: charl
 */

#ifndef HM0360_MD_H_
#define HM0360_MD_H_


/********************************** Includes ******************************************/

#include <stdbool.h>
#include "hx_drv_CIS_common.h"

/*************************************** Definitions *******************************************/

#define HM0360_SENSOR_I2CID				(0x24)

// Use Dynamic 1 mode (value 3) as default strobe enabLe setting for STROBE_CFG (0x3080)
#define HM0360_SENSOR_STROBE_MODE		(0x03)

#define HM0360NUMGAINREGS 5

// Structure to contain some values relating to automatic exposure
typedef struct {
	uint16_t integration;	// Value of INTEGRATION_H, INTEGRATION_L
	uint16_t digitalGain;	// Value of DIGITAL_GAIN_H, DIGITAL_GAIN_L
	uint8_t analogGain;		// Value of ANALOG_GAIN
	uint8_t aeMean;			// Value of AE_MEAN
	uint8_t aeConverged;	// Value of AE_CONVERGED
} HM0360_GAIN_T;

// Select streaming mode by writing to 0x0100.
// See data sheet 6.1 & 10.2
typedef enum {
    MODE_SLEEP,				// 0 Hardware sleep
	MODE_SW_CONTINUOUS,		// 1 SW triggers continuous streaming
	MODE_SW_NFRAMES_SLEEP,	// 2 SW trigger, output N frames then sleep for a set interval
	MODE_SW_NFRAMES_STANDBY,// 3 SW trigger, output N frames then s/w standby (needs a reset to restart???)
	MODE_HW_TRIGGER,		// 4 TRIGGER pin starts streaming
	MODE_RFU,				// 5 Not defined
	MODE_HW_NFRAMES_STANDBY,// 6 HW trigger, output N frames then s/w standby (needs a reset to restart???)
	MODE_HW_NFRAMES_SLEEP,	// 7 HW trigger, output N frames then sleep
} mode_select_t;

/*************************************** Public Function Declarations **************************/

bool hm0360_md_isSensorPresent(uint8_t sensorAddress);

bool hm0360_md_isHM0360Present(void);

// Setter and getter for whether the HM0360 is the main camera
void hm0360_md_setIsMainCamera(bool hm0360IsMainCamera);
bool hm0360_md_getIsMainCamera(void);

// HM0360 initialisation only if it is used as MD for a RP camera
void hm0360_md_init();

HX_CIS_ERROR_E hm0360_md_setMode(uint8_t context, mode_select_t newMode, uint8_t numFrames, uint16_t sleepTime);

HX_CIS_ERROR_E hm0360_md_getInterruptStatus(uint8_t * val);

HX_CIS_ERROR_E hm0360_md_clearInterrupt(uint8_t val);
HX_CIS_ERROR_E hm0360_md_enableInterrupt(void);
HX_CIS_ERROR_E hm0360_md_disableInterrupt(void);

// Prepare the MD
HX_CIS_ERROR_E hm0360_md_prepare(bool cameraSystemEnabled, uint16_t mdFrameInterval);
HX_CIS_ERROR_E hm0360_md_getGainRegs(HM0360_GAIN_T * val);

// Read the AE gain ceilings (MAX_AGAIN, MAX_DGAIN_H/L) - used by lightSensor.c
// to detect when the AE loop has railed its gain to maximum.
HX_CIS_ERROR_E hm0360_md_getGainCeilings(uint8_t *maxAnalogGain, uint16_t *maxDigitalGain);

// Read the current MODE_SELECT value. Side-effect-free counterpart to
// hm0360_md_setModeSelectOnly() - do not use hm0360_md_setMode() to restore a
// mode read via this getter.
HX_CIS_ERROR_E hm0360_md_getMode(mode_select_t *mode);

// Write MODE_SELECT directly, with none of hm0360_md_setMode()'s side effects
// (no forced sleep interlude, no PMU_CFG rewrite, no MD interrupt toggle).
// For a transient streaming nudge (e.g. waking the sensor to sample AE
// registers, then restoring the mode read via hm0360_md_getMode() beforehand).
HX_CIS_ERROR_E hm0360_md_setModeSelectOnly(mode_select_t mode);

uint16_t hm0360_md_getMDOutput(uint8_t * regTable, uint8_t length);

void hm0360_md_printGrid(uint8_t *roiOut, uint16_t numBlocks, char *msg, uint16_t msgLen);

// Read whether the STROBE pin is currently configured to drive the flash.
// Side-effect-free counterpart to hm0360_md_configureStrobe().
HX_CIS_ERROR_E hm0360_md_getStrobe(bool *flashEnabled);

// Configure the HM0360 STROBE pin which can drive the flash cct
HX_CIS_ERROR_E hm0360_md_configureStrobe(bool flashRequired);

// Re-program the HM0360 with the long register list
HX_CIS_ERROR_E hm0360_md_reInitialise(void);

// Replaced by hm0360_md_prepare()
//HX_CIS_ERROR_E hm0360_md_enableMD(uint16_t mdFrameInterval);

#endif /* HM0360_MD_H_ */
