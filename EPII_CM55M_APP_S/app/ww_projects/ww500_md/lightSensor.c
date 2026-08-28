/*
 * lightSensor.c
 *
 *  Created on: 26 Aug 2026
 *      Author: Claude (for Charles Palmer)
 *
 *  See lightSensor.h.
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>	// snprintf() for the AE_MEAN sample log

#include "FreeRTOS.h"
#include "task.h"

#include "lightSensor.h"
#include "hm0360_md.h"
#include "ledFlash.h"
#include "fatfs_task.h"
#include "ww500_md.h"	// app_getElapsedMs()
#include "xprintf.h"
#include "printf_x.h"	// Print colours

/*********************************************** Local Defines **********************************************/

// Number of AE frames to average, and the gap between them. The window
// (count * gap) must span several AE-loop oscillation periods or the average
// does not smooth: bench testing in a dark box showed an 8 x 40ms (~0.3s)
// window still swinging 5..66 AE_MEAN, because the HM0360 AE limit-cycle is
// slower than that. 16 x 120ms (~1.9s) spans it and yields a stable mean.
#define AE_SAMPLE_COUNT   16
#define AE_SAMPLE_GAP_MS  120

// Time to let the sensor start streaming and its AE loop begin adapting,
// after waking it from sleep for the sampling window (~5 frames at 10fps).
#define AE_WAKE_SETTLE_MS 500

// Hysteresis band (AE_MEAN units, 0-255) above the dark threshold: dark is
// entered below OP_PARAMETER_AE_DARK_THRESHOLD, but only left once brightness
// rises above (threshold + AE_HYSTERESIS), so the decision does not chatter
// when the light sits near the boundary.
#define AE_HYSTERESIS 12

/*********************************************** Local Types ************************************************/

// Aggregated AE statistics over a sampling window - private to the dark/bright
// decision, nothing outside this file needs the per-sample detail.
typedef struct {
	uint8_t  samples;	// frames actually read
	uint16_t meanAE;	// mean AE_MEAN over the samples
	uint8_t  minAE;
	uint8_t  maxAE;
	bool     gainRailed;	// AE gain at maximum on most frames - unambiguously dark
} LightSensorStats_t;

/*********************************************** Local Variables ********************************************/

static uint16_t lastReading = 0;

/*********************************************** Local Function Declarations *********************************/

static bool sampleAeStats(LightSensorStats_t *stats);
static void decideDarkBright(const LightSensorStats_t *stats);

/*********************************************** Local Function Definitions *********************************/

/**
 * @brief Sample AE_MEAN and the gain registers over AE_SAMPLE_COUNT frames.
 *
 * Wakes the HM0360 into streaming first if it was asleep (a sleeping sensor
 * reads AE_MEAN = 0), and restores its prior mode afterward. Also disables the
 * STROBE pin for the duration - if left as whatever the previous DPD sleep
 * armed it to (e.g. MD illumination), the sensor would otherwise fire the
 * flash on every one of these streamed sampling frames. Restored afterward,
 * whether or not the mode itself needed changing.
 *
 * @param stats [out] aggregated statistics, filled if at least one sample was read
 * @return true if stats->samples > 0
 */
static bool sampleAeStats(LightSensorStats_t *stats) {
	HM0360_GAIN_T gain;
	uint8_t maxAnalogGain;
	uint16_t maxDigitalGain;
	uint8_t railedCount = 0;
	uint32_t sumAE = 0;
	mode_select_t priorMode = MODE_SLEEP;
	bool wokeForSampling = false;
	bool priorStrobeEnabled = false;
	bool gotPriorStrobe;
	char aeMeanLog[AE_SAMPLE_COUNT * 4 + 8];	// "nnn " per sample
	uint16_t logOffset = 0;

	stats->samples = 0;
	stats->minAE = 255;
	stats->maxAE = 0;

	if (hm0360_md_getGainCeilings(&maxAnalogGain, &maxDigitalGain) != HX_CIS_NO_ERROR) {
		return false;
	}

	// If the HM0360 strobe is enabled, then disable it.
	gotPriorStrobe = (hm0360_md_getStrobe(&priorStrobeEnabled) == HX_CIS_NO_ERROR);
	if (gotPriorStrobe && priorStrobeEnabled) {
		hm0360_md_configureStrobe(false);
	}

	// If the HM0360 is in SLEEP state then put it in CONTINUOUS mode.
	if ((hm0360_md_getMode(&priorMode) == HX_CIS_NO_ERROR) &&
			((priorMode == MODE_SLEEP) || (priorMode == MODE_SW_NFRAMES_STANDBY))) {
		if (hm0360_md_setModeSelectOnly(MODE_SW_CONTINUOUS) == HX_CIS_NO_ERROR) {
			wokeForSampling = true;
			vTaskDelay(pdMS_TO_TICKS(AE_WAKE_SETTLE_MS));
		}
	}

	// Loop several times reading the gain registers. Record each AE Mean reading in a string.
	aeMeanLog[0] = '\0';
	for (uint8_t i = 0; i < AE_SAMPLE_COUNT; i++) {
		if (hm0360_md_getGainRegs(&gain) == HX_CIS_NO_ERROR) {
			sumAE += gain.aeMean;
			stats->samples++;
			if (logOffset < sizeof(aeMeanLog)) {
				logOffset += snprintf(aeMeanLog + logOffset, sizeof(aeMeanLog) - logOffset, "%u ", gain.aeMean);
			}
			if (gain.aeMean < stats->minAE) {
				stats->minAE = gain.aeMean;
			}
			if (gain.aeMean > stats->maxAE) {
				stats->maxAE = gain.aeMean;
			}
			// "Railed" = both gains at (or above) the ceiling - AE can amplify no further.
			if ((maxAnalogGain > 0) && (gain.analogGain >= maxAnalogGain) &&
					(maxDigitalGain > 0) && (gain.digitalGain >= maxDigitalGain)) {
				railedCount++;
			}
		}

		if (i + 1 < AE_SAMPLE_COUNT) {
			vTaskDelay(pdMS_TO_TICKS(AE_SAMPLE_GAP_MS));
		}
	}

	// Print the several AE mean values.
	XP_CYAN xprintf("[LS] getAEStats: %d AE_MEAN samples: %s\n", stats->samples, aeMeanLog); XP_WHITE

	// Potentially restore HM0360 mode - e.g. to SLEEP
	if (wokeForSampling) {
		if (hm0360_md_setModeSelectOnly(priorMode) != HX_CIS_NO_ERROR) {
			XP_CYAN xprintf("[LS] getAEStats: failed to restore HM0360 mode %d\n", priorMode); XP_WHITE
		}
	}

	// If necessary, re-enable the HM0360 STROBE
	if (gotPriorStrobe && priorStrobeEnabled) {
		hm0360_md_configureStrobe(true);
	}

	if (stats->samples == 0) {
		stats->minAE = 0;
		return false;
	}

	stats->meanAE = (uint16_t)(sumAE / stats->samples);
	stats->gainRailed = (railedCount * 2 > stats->samples);	// majority railed

	return true;
}

/**
 * @brief Turn aggregated AE statistics into a hysteresis-filtered dark/bright
 * decision and persist it.
 *
 * Deliberately does NOT drive the flash LED here - this runs from both the
 * real capture path (lightSensor_takeReading()) and the passive on-demand
 * 'light' CLI command (lightSensor_takeReadingForced()), and the latter must
 * not have the side effect of switching hardware on. The flash is driven by
 * the caller instead, only where that is actually wanted (image_task.c).
 *
 * @param stats aggregated AE statistics from sampleAeStats() or a fallback single reading
 */
static void decideDarkBright(const LightSensorStats_t *stats) {
	uint16_t threshold = fatfs_getOperationalParameter(OP_PARAMETER_AE_DARK_THRESHOLD);
	// Hysteresis memory is the persisted decision - the only memory that
	// survives DPD.
	bool wasDark = (fatfs_getOperationalParameter(OP_PARAMETER_AE_FLASH_STATE) == 1);
	bool dark = wasDark;

	if (stats->gainRailed) {
		dark = true;
	}
	else if (stats->meanAE < threshold) {
		dark = true;
	}
	else if (stats->meanAE > (uint16_t)(threshold + AE_HYSTERESIS)) {
		dark = false;
	}
	// else: within the hysteresis band - keep the previous decision

	fatfs_setOperationalParameter(OP_PARAMETER_AE_FLASH_STATE, dark ? 1 : 0);

	XP_CYAN xprintf("[LS] AE light check: mean AE = %d (min %d, max %d) over %d frames, "
			"threshold = %d, gain railed = %s -> %s%s\n",
			stats->meanAE, stats->minAE, stats->maxAE, stats->samples,
			threshold, stats->gainRailed ? "yes" : "no",
			dark ? "DARK (flash wanted)" : "BRIGHT (no flash)",
			(dark == wasDark) ? "" : " (changed)"); XP_WHITE
}

/*********************************************** Global Function Definitions *********************************/

bool lightSensor_isRequired(void) {
	return (ledFlashGetFlashMode() == FLASH_MODE_AE)
			|| (fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) == 1);
}

void lightSensor_takeReading(void) {
	if (!lightSensor_isRequired()) {
		return;
	}

	lightSensor_takeReadingForced();
}

void lightSensor_takeReadingForced(void) {
	LightSensorStats_t stats;
	TickType_t startTime;

	startTime = xTaskGetTickCount();

	if (!sampleAeStats(&stats)) {
		// Sampling failed - fall back to a single reading rather than leaving
		// the decision stale.
		HM0360_GAIN_T gain;
		if (hm0360_md_getGainRegs(&gain) != HX_CIS_NO_ERROR) {
			return;
		}
		stats.samples = 1;
		stats.meanAE = gain.aeMean;
		stats.minAE = gain.aeMean;
		stats.maxAE = gain.aeMean;
		stats.gainRailed = false;
	}

	XP_CYAN xprintf("[LS] AE sampling took %dms\n", app_getElapsedMs(startTime)); XP_WHITE

	decideDarkBright(&stats);
	lastReading = stats.meanAE;
}

uint16_t lightSensor_getReading(void) {
	return lastReading;
}

bool lightSensor_isDark(void) {
	return (fatfs_getOperationalParameter(OP_PARAMETER_AE_FLASH_STATE) == 1);
}
