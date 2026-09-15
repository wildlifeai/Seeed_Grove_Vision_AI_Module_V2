/*
 * lightSensor.h
 *
 *  Created on: 26 Aug 2026
 *      Author: Claude (for Charles Palmer)
 *
 *  Light-level sensing for the AE-driven flash (FLASH_MODE_AE) and automatic
 *  day/night camera switching (OP_PARAMETER_SLOT_SWITCH). Reads the HM0360's
 *  AE registers over several frames - a single reading is not a reliable
 *  brightness measure, see hm0360_md_getGainRegs() vs the sampling loop in
 *  lightSensor.c - and turns that into a hysteresis-filtered dark/bright
 *  decision, persisted across DPD sleep.
 *
 *  See _Documentation/AE_Light_Sensor_Roadmap.md and
 *  _Documentation/development reports/2026-08-24_light_sensor_review/.
 */

#ifndef LIGHTSENSOR_H_
#define LIGHTSENSOR_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>

/*********************************************** Global Defines **********************************************/


/*********************************************** Global Types ************************************************/


/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

// True if something (the AE-driven flash or automatic camera switching)
// actually needs a light-level reading this wake.
bool lightSensor_isRequired(void);

// Take a fresh light-level reading and update the dark/bright decision.
// Blocks for the sampling window (~2s). No-op if !lightSensor_isRequired().
void lightSensor_takeReading(void);

// Same as lightSensor_takeReading(), but always samples, ignoring
// lightSensor_isRequired() - for on-demand bench/debug use (e.g. the 'light'
// CLI command). Prefer lightSensor_takeReading() for normal wake-cycle use.
// Neither function drives the flash LED - that is the caller's job (see
// image_task.c), so a bare light check never has the side effect of
// switching hardware on.
void lightSensor_takeReadingForced(void);

// The last sampled brightness (HM0360 AE_MEAN units, 0-255, higher = brighter).
// 0 if lightSensor_takeReading() has not been called since boot/wake.
uint16_t lightSensor_getReading(void);

// The current hysteresis-filtered dark/bright decision. Persists across DPD
// sleep, so it is valid even before lightSensor_takeReading() is called.
bool lightSensor_isDark(void);

#endif /* LIGHTSENSOR_H_ */
