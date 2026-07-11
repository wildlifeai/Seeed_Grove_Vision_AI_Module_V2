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
#include "hm0360_md.h"

/*************************************** Definitions *******************************************/

// If uncommented, a timer is used to turn off the flash.
// Probably not needed as the state machine should also turn it off.
//#define TIMER_TURNS_OFF_FLASH

// Bit mask for which LED to enable
typedef enum flashLeds {
    VIS_LED = 1,	 // 1 (bit 0)
    IR_LED = 2,	 	 // 2 (bit 1)
} FlashLeds_t;


// Four options to determine if the LED flash occurs
typedef enum flashLedMode {
    FLASH_MODE_OFF,			// Off all the time
    FLASH_MODE_ALWAYS_ON,	// On all the time
    FLASH_MODE_AE,			// Determined by light levels
    FLASH_MODE_TIME_OF_DAY	// Determined by time of day timer
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

// returns whether the LED flash should be in use
bool ledFlashIsActive(void);

// Setter for flashMode
void ledFlashSetFlashMode(FlashLedMode_t mode);

// Getter for flashMode
FlashLedMode_t ledFlashGetFlashMode(void);

void ledFlashSetFlashModeFromOpParam(uint16_t ledInUse, uint16_t startTime, uint16_t duration);

// Inform the code of the time (in case led is determined by time of day)
void ledFlashNewTime(rtc_time tm);

// Inform the code of the HM0360 AE registers (in case led is determined by brightness)
void ledFlashNewAEValues(HM0360_GAIN_T * val);

#endif /* LEDFLASH_H_ */
