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


// The LED flash is either off or driven by the AE light sensor
// (see _Documentation/AE_Light_Sensor_Roadmap.md)
typedef enum flashLedMode {
    FLASH_MODE_OFF,			// Off all the time
    FLASH_MODE_AE,			// Determined by light levels
} FlashLedMode_t;

// Add some limits for duration of flash
#define LEDFLASHDURATIONMIN 10
#define LEDFLASHDURATIONMAX 2000

// Hysteresis band (in AE_MEAN units, 0-255) above the dark threshold. The flash
// turns ON below OP_PARAMETER_AE_DARK_THRESHOLD but only turns OFF again once
// brightness rises above (threshold + AE_HYSTERESIS), so it does not chatter
// when the light sits near the boundary. See ledFlashNewAEStats().
#define AE_HYSTERESIS 12

// Number of AE frames to average, and the gap between them, for the light-sensor
// reading. See hm0360_md_getAEStats(). The window (count * gap) must span several
// AE-loop oscillation periods or the average does not smooth: bench testing in a
// dark box showed an 8 x 40ms (~0.3s) window still swinging 5..66, because the
// HM0360 AE limit-cycle is slower than that. 16 x 120ms (~1.9s) spans it and
// yields a stable dark mean (~35). Cost is ~1.9s of extra awake time per AE
// check - negligible against the multi-minute wake interval.
#define AE_SAMPLE_COUNT   16
#define AE_SAMPLE_GAP_MS  120

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

void ledFlashSetFlashModeFromOpParam(uint16_t ledInUse);


// Inform the code of the HM0360 AE registers (in case led is determined by brightness)
// Single-frame legacy path - prefer ledFlashNewAEStats() for a robust decision.
void ledFlashNewAEValues(HM0360_GAIN_T * val);

// Decide the flash state from aggregated AE statistics (multi-frame light sensor).
void ledFlashNewAEStats(HM0360_AE_STATS_T * stats);

#endif /* LEDFLASH_H_ */
