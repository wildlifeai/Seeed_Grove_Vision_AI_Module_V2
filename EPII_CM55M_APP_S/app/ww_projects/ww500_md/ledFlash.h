/*
 * ledFlash.h
 *
 * Supports LED flash operation: select brightness, IR vs visible, enable
 *
 *  Created on: 14 Aug 2025
 *      Author: charl
 */

#ifndef LEDFLASH_H_
#define LEDFLASH_H_

/********************************** Includes ******************************************/

#include <stdbool.h>
#include "hx_drv_rtc.h"

/*************************************** Definitions *******************************************/

// If uncommented, a timer is used to turn off the flash.
// Probably not needed as the state machine should also turn it off.
//#define TIMER_TURNS_OFF_FLASH

// Bit mask for which LED to enable
typedef enum flashLeds {
    VIS_LED = 1,	 // 1 (bit 0)
    IR_LED = 2,	 	 // 2 (bit 1)
} FlashLeds_t;


// Capture flash mode, set directly from OP_PARAMETER_FLASH_MODE
// (see _Documentation/AE_Light_Sensor_Roadmap.md and
// _Documentation/development reports/2026-08-24_light_sensor_review/flash_led_modes_proposal.md)
typedef enum flashLedMode {
    FLASH_MODE_OFF = 0,		// Off all the time
    FLASH_MODE_AE,			// Determined by light levels
    FLASH_MODE_ALWAYS_ON,	// On all the time
    FLASH_MODE_TIME_OF_DAY,	// On within a configured UTC time window (OP_PARAMETER_FLASH_TOD_START/_DURATION)
} FlashLedMode_t;

// Add some limits for duration of flash
#define LEDFLASHDURATIONMIN 10
#define LEDFLASHDURATIONMAX 2000

/*************************************** Public Function Declarations **************************/

// Enables the chip - returns true if OK
bool ledFlashInit(void);

// Set LED brightness
void ledFlashBrightness(uint8_t brightness);

// Select which LED(s) to activate
void ledFlashSelectLED(FlashLeds_t led);

// Turns on LED for a defined interval
void ledFlashEnable(void);

// Turn off the flash
void ledFlashDisable(void);

// Turn on the flash if conditions are right.
void ledFlashActivate(void);

// return 0 if flash is inactive. Otherwise return  1 (visible) or 2 (IR)
uint8_t ledFlashIsActive(void);

// Setter for flashMode
void ledFlashSetFlashMode(FlashLedMode_t mode);

// Getter for flashMode
FlashLedMode_t ledFlashGetFlashMode(void);

void ledFlashSetFlashModeFromOpParam(uint16_t ledInUse, uint16_t flashModeParam);

// Setter for flashActive - records a dark/bright decision (image_task.c, from
// the light sensor) for the next real capture / DPD-entry STROBE arming to
// read. Does NOT drive the flash hardware itself - see ledFlash.c.
void ledFlash_setActive(bool active);

// Re-evaluate FLASH_MODE_TIME_OF_DAY immediately after the RTC is set (e.g.
// prvSetUtc()) - no-op in any other mode. Only updates the flashActive flag,
// same reasoning as ledFlash_setActive() - does not drive hardware directly.
void ledFlash_reevaluateTimeOfDay(void);

#endif /* LEDFLASH_H_ */
