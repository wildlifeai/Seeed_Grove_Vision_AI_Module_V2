/*
 * ledFlash.c
 *
 *  Created on: 14 Aug 2025
 *      Author: charl
 */



/********************************** Includes ******************************************/

#include <stdbool.h>

// FreeRTOS includes
#include "FreeRTOS.h"
#include "timers.h"

#include "app_msg.h"
#include "xprintf.h"
#include "printf_x.h"	// Print colours

#include "fatfs_task.h"
#include "ledFlash.h"
#include "pca9574.h"

#include "hm0360_md.h"
#include "hx_drv_rtc.h"

/*************************************** Defines **************************************/

#define LF_NUMCHANNELS 	8

// Defines for bits on the  PCA9574
#define LF_BRSEL0		(1 << 0)
#define LF_BRSEL1		(1 << 1)
#define LF_BRSEL2		(1 << 2)
#define LF_BRSEL3		(1 << 3)
#define LF_VISENABLE 	(1 << 4)
#define LF_IRENABLE		(1 << 5)
#define LF_RFU			(1 << 6)
#define LF_FLENABLE		(1 << 7)


/*************************************** Local Function Declarations ******************/

#ifdef TIMER_TURNS_OFF_FLASH
static void FlashOffTimerCallback(TimerHandle_t xTimer);
#endif // TIMER_TURNS_OFF_FLASH

/*************************************** External variables *******************************************/

// These are the handles for the input queues of the two tasks. So we can send them messages
extern QueueHandle_t xImageTaskQueue;

/*************************************** Local variables ******************************/

// What determines how to figure out if the LED shoud flash
static FlashLedMode_t flashMode = FLASH_MODE_OFF;

static bool ledFlashInitialised = false;

// The flash should be operating as determined by OpParam settings, AE values and time of day
static bool flashActive = false;

// Need to maintain a copy of bits sent to the control/status chip, so we can change individual bits
static uint8_t controlBits = 0;

#ifdef TIMER_TURNS_OFF_FLASH
static TimerHandle_t flashOffTimer;
#endif // TIMER_TURNS_OFF_FLASH

/*************************************** Local Function Definitions *******************/

#ifdef TIMER_TURNS_OFF_FLASH


/**
 * Timer has asked us to turn off the flash LED.
 * Since this has come from the timer we must use the 'fromISR' function
 *
 * The message is delivered to the image_task queue and the result
 * is that the LED is turned off.
 *
 * TODO - probaby we can mamage the iamge_task state machine so it
 * turns off the flash LED based on state transitions. But for now....
 *
 */
static void FlashOffTimerCallback(TimerHandle_t xTimer) {
	//ledFlashDisable();

    APP_MSG_T timerOffMsg;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    timerOffMsg.msg_event = APP_MSG_IMAGETASK_FLASH_OFF;
    timerOffMsg.msg_data = 0;
    timerOffMsg.msg_parameter = 0;

    xQueueSendFromISR(xImageTaskQueue, &timerOffMsg, &xHigherPriorityTaskWoken);

    if (xHigherPriorityTaskWoken)  {
        taskYIELD();
    }
}
#endif // TIMER_TURNS_OFF_FLASH

/********************************** Public Function Definitions ***********************/


/**
 * Enables the LED Flash system.
 *
 * Leaves with both LEDs deselected, brightness at minimum
 *
 * @return -true on success
 */
bool ledFlashInit(void) {
	HX_CIS_ERROR_E ret;

	flashMode = FLASH_MODE_OFF;
	flashActive = false;

	ret = pca9574_init(PCA9574_I2C_ADDRESS_0);

	if (ret != HX_CIS_NO_ERROR) {
		// Failure. Maybe no PCA9574
		ledFlashInitialised = false;
	    XP_LT_RED;
	    xprintf("Failed to initialise Flash LED\n");
	    XP_WHITE;
		return false;
	}

	ledFlashInitialised = true;

	// pca9574_init() has set all output bits to output, 0
	controlBits = 0;

	// Note that at this stage both the IR and visible LEDs are enabled.
	// The application should call ledFlashSelectLED() to change this.
	ledFlashSelectLED(0);	// de-select both LEDs

#ifdef TIMER_TURNS_OFF_FLASH
    flashOffTimer = xTimerCreate("FlashOffTimer",
            pdMS_TO_TICKS(10),    // initial dummy period
            pdFALSE,                // one-shot timer
            NULL,                   // timer ID (optional)
			FlashOffTimerCallback);
#endif // TIMER_TURNS_OFF_FLASH

	return true;
}

/**
 * Selects the LED brightness
 *
 * Takes the brightness percentage and approximates a value to write to the MOSFETs.
 * This is rough! Might need to revise this e.g. with a switch(brightness) statement
 *
 * Use operational_parameter[OP_PARAMETER_FLASH_LED] to inhibit flash
 *
 * @param brightness - a  value that determines brightness - percentage.
 */
void ledFlashBrightness(uint8_t brightness) {
	uint8_t brBits;

	if (!ledFlashInitialised) {
		return;
	}

	// Convert to a value between 0-15 - approximate!
	brBits = (brightness * 15 / 100);

	if (brBits > 15) {
		brBits = 15;
	}

	// brightness is a value between 0-15 and these should map onto the 4 LS bits of the PCA9574
	// which control the feedback resistors for the PSUs

	controlBits &= ~0x0f;	// clear the 4 LS bits
	controlBits |= brBits;	// sets the 4 LS bits

	// The control bits are only written to hardware by ledFlashEnable() and ledFlashDisable()

	XP_LT_RED;
    xprintf("DEBUG: ledFlashBrightness(%d%%) [brbits = 0x%01x]\n", brightness, brBits);
    XP_WHITE;
}

/**
 * Selects the LED(s) to use
 *
 * In some hardware implementations there is only one LED - this is VISLED
 *
 * This is the place at which we can enable or disable the LED based on the
 * flashMode and other conditions.
 *
 * @param led - a bit mask, one for each LED
 */
void ledFlashSelectLED(FlashLeds_t led) {

	if (!ledFlashInitialised) {
		return;
	}

	if (led & VIS_LED) {
		// Active low, so clear this bit to select the LED
		controlBits &= ~LF_VISENABLE;
	}
	else {
		controlBits |= LF_VISENABLE;
	}

	if (led & IR_LED) {
		// Active low, so clear this bit to select the LED
		controlBits &= ~LF_IRENABLE;
	}
	else {
		controlBits |= LF_IRENABLE;
	}

	// Don't write controlBits except in ledFlashEnable() and ledFlashDisable()
	XP_LT_RED;
	xprintf("DEBUG: ledFlashSelectLED(%d)\n", led);
	XP_WHITE;
}

/**
 * Turns on the Flash LED
 *
 * ledFlashEnable() and ledFlashDisable() are the only functions
 * that writecontrolBits to the hardware.
 *
 * This is not (normally) used if the HM0360 is the main camera.
 *
 * The LED will be turned off when the Frame Ready message arrives.
 * At present the LED will also be turned off later by a timer,
 * but this is redundant. Left in at present as a precaution.
 * Also turned off explicitly before entering DPD, again as a precaution.
 *
 * NOTE: this turns on the FLASHEN bit but does NOT affect the HM0360 whose STROBE pin might also flash the LED.
 */
void ledFlashEnable(void) {

	if (!ledFlashInitialised) {
		return;
	}

	XP_LT_RED;
	xprintf("DEBUG: ledFlashEnable()\n");

	controlBits |= LF_FLENABLE;
	// Now send these bits to the PCA9574
	pca9574_write(PCA9574_I2C_ADDRESS_0, PCA9574_REG_OUT, controlBits);

    XP_WHITE;

#ifdef TIMER_TURNS_OFF_FLASH
    uint16_t duration = fatfs_getOperationalParameter(OP_PARAMETER_FLASH_DURATION);
	// Start a timer that delays for the defined interval.
    if (flashOffTimer != NULL) {
        // Change the period and start the timer
    	// The callback is FlashOffTimerCallback() which calls ledFlashDisable
        xTimerChangePeriod(flashOffTimer, pdMS_TO_TICKS(duration), 0);
    }
#endif // TIMER_TURNS_OFF_FLASH
}

/**
 * Turns off the Flash LED
 *
 * ledFlashEnable() and ledFlashDisable() are the only functions
 * that writecontrolBits to the hardware.
 *
 * NOTE: this turns off the FLASHEN bit but does NOT stop the HM0360 from flashing the LED
 */
void ledFlashDisable(void) {

	if (!ledFlashInitialised) {
		return;
	}

	XP_LT_RED;
	xprintf("DEBUG: ledFlashDisable()\n");

	controlBits &= ~LF_FLENABLE;
	// Now send these bits to the PCA9574
	pca9574_write(PCA9574_I2C_ADDRESS_0, PCA9574_REG_OUT, controlBits);

    XP_WHITE;
}

/**
 * Turns on the Flash LED if conditions are right
 *
 * This calls ledFlashEnable() or ledFlashDisable() which are the only functions
 * that write controlBits to the hardware.
 */
void ledFlashActivate(void) {

	if (!ledFlashInitialised) {
		return;
	}

	if (flashActive) {
		ledFlashEnable();
	}
	else {
		ledFlashDisable();
	}
}

/**
 * Returns whether the LED flash should be in use
 *
 * @return true if the LED is active
 */
bool ledFlashIsActive(void) {
	return flashActive;
}

/**
 * Setter for flashMode from operational parameter values
 *
 * Call when the Operational Parameters have been loaded from SD card
 *
 * The flash for captures is either off, or driven by the AE light sensor
 * (on when the scene is dark). See _Documentation/AE_Light_Sensor_Roadmap.md

| No. |  Case                    | OP_PARAMETER_FLASH_LED |
|-----|--------------------------|------------------------|
| 1   | Always off               | 0                      |
| 2   | Selected by AE           | 1 (visible) or 2 (IR)  |
 *
 */
void ledFlashSetFlashModeFromOpParam(uint16_t ledInUse) {

	// ledFlashSelectLED
	ledFlashSelectLED(ledInUse);

	if (ledInUse == 0) {
		// No LEDs
		flashMode = FLASH_MODE_OFF;
		flashActive = false;
	}
	else {
		// Determined by AE registers (the AE light sensor)
		flashMode = FLASH_MODE_AE;
		// Restore the last AE light decision. It is persisted as an Operational
		// Parameter because RAM is lost in DPD, and the first capture after a
		// motion-detect wake happens before any fresh AE reading exists.
		flashActive = (fatfs_getOperationalParameter(OP_PARAMETER_AE_FLASH_STATE) == 1);
	}

	// debug
	xprintf("In ledFlashSetFlashModeFromOpParam with %d Mode %d\n",
			ledInUse, flashMode);
}



// Setter for flashMode
void ledFlashSetFlashMode(FlashLedMode_t mode) {
	flashMode = mode;
}

// Getter for flashMode
FlashLedMode_t  ledFlashGetFlashMode(void) {
	return flashMode;
}

/**
 * The HM0360 AE registers values have arrived - this might determine LED Flash behaviour
 *
 * Legacy single-frame entry point, kept for callers that only have one reading.
 * Prefer ledFlashNewAEStats(), which is robust against the AE loop oscillation
 * documented there. This wraps the single reading as a one-sample statistic.
 *
 * @param gainRegs
 */
void ledFlashNewAEValues(HM0360_GAIN_T * gainRegs) {
	HM0360_AE_STATS_T stats;

	if (gainRegs == NULL) {
		return;
	}

	stats.samples = 1;
	stats.meanAE = gainRegs->aeMean;
	stats.minAE = gainRegs->aeMean;
	stats.maxAE = gainRegs->aeMean;
	stats.maxAnalogGain = gainRegs->analogGain;
	stats.maxDigitalGain = gainRegs->digitalGain;
	stats.railedCount = 0;
	stats.gainRailed = false;

	ledFlashNewAEStats(&stats);
}

/**
 * Decide the flash state from aggregated AE statistics (the light sensor).
 *
 * A single AE_MEAN reading is unreliable: it is the output of the HM0360's AE
 * control loop, which limit-cycles. Bench testing in a fully dark box showed
 * AE_MEAN swinging between ~3 and ~66 (across the dark threshold), so ~37% of
 * single-frame reads wrongly said "bright". This uses the mean over several
 * frames plus two extra safeguards:
 *
 *   - Hysteresis: turn the flash ON below the dark threshold, but only turn it
 *     OFF again once well above it (threshold + AE_HYSTERESIS). This stops the
 *     flash chattering when the light sits near the boundary.
 *   - Gain-railed override: if the AE has run its gain to maximum on most
 *     frames it cannot expose any darker, so force the flash ON regardless of
 *     the (then meaningless) AE_MEAN value.
 *
 * See _Documentation/AE_Light_Sensor_Roadmap.md
 *
 * @param stats  aggregated AE statistics from hm0360_md_getAEStats()
 */
void ledFlashNewAEStats(HM0360_AE_STATS_T * stats) {
	uint16_t threshold;
	bool wasActive;

    if ((flashMode != FLASH_MODE_AE) || (stats == NULL) || (stats->samples == 0)) {
    	return;
    }

    threshold = fatfs_getOperationalParameter(OP_PARAMETER_AE_DARK_THRESHOLD);
    wasActive = flashActive;

    if (stats->gainRailed) {
        // AE gain maxed out on most frames - unambiguously dark
        flashActive = true;
    }
    else if (stats->meanAE < threshold) {
        // Averaged scene brightness below the dark threshold - flash needed
        flashActive = true;
    }
    else if (stats->meanAE > (uint16_t)(threshold + AE_HYSTERESIS)) {
        // Comfortably bright - flash not needed
        flashActive = false;
    }
    // else: within the hysteresis band - keep the previous decision (flashActive)

    // Persist the decision (written to CONFIG.TXT at DPD entry) so the first
    // capture after the next wake uses it - RAM does not survive DPD
    fatfs_setOperationalParameter(OP_PARAMETER_AE_FLASH_STATE, flashActive ? 1 : 0);

	xprintf("AE light check: mean AE = %d (min %d, max %d) over %d frames, "
			"threshold = %d, gain railed = %s -> flash %s%s\n",
			stats->meanAE, stats->minAE, stats->maxAE, stats->samples,
			threshold, stats->gainRailed ? "yes" : "no",
			flashActive ? "ON" : "OFF",
			(flashActive == wasActive) ? "" : " (changed)");

	ledFlashActivate();	// Turn on Flash LED (conditionally)
}
