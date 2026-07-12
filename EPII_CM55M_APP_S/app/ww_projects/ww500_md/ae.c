/*
 * ae.c
 *
 * Minimal auto-exposure for the RP (Sony IMX) cameras. See ae.h.
 *
 * Control law: work in the product domain P = exposure_lines x gain (the
 * total light multiplier), scale P by target/measured each step (damped),
 * then split P back into exposure first, analog gain for the remainder.
 *
 * Bench measurements behind the constants (10 Jul 2026, see
 * _Documentation/rp3-image-quality-plan.md):
 *   - exposure responds linearly ~100..5000+ lines (no frame-length clamp
 *     seen up to 5120)
 *   - analog gain works across the full Sony range (code 0..960 = 1..16x)
 *   - the sensor pedestal shows as a constant offset in the Y plane
 */

#include <stdint.h>
#include <stdbool.h>

#include "WE2_core.h"		// SCB_InvalidateDCache_by_Addr
#include "xprintf.h"

#include "hx_drv_CIS_common.h"	// hx_drv_cis_set_reg
#include "fatfs_task.h"		// operational parameters
#include "preview.h"		// preview_isActive(): unbounded AE while previewing
#include "cisdp_sensor.h"	// cisdp_select_main_camera_i2c()
#include "ae.h"

// Sony register layout (IMX708/IMX219): 16-bit big-endian pairs
#define AE_REG_EXPOSURE_H	0x0202
#define AE_REG_EXPOSURE_L	0x0203
#define AE_REG_GAIN_H		0x0204
#define AE_REG_GAIN_L		0x0205

#define AE_EXPOSURE_DEFAULT	0x0940	// the init-table value (IMX708_EXPOSURE_SETTING)
#define AE_EXPOSURE_MIN		8		// lines (bright daylight needs very short exposures)
#define AE_EXPOSURE_MAX		5000	// lines (bench-verified; frame time grows beyond)
#define AE_GAIN_CODE_MAX	960		// 16x  (gain = 1024 / (1024 - code))
#define AE_GAIN_Q8_UNITY	256
#define AE_GAIN_Q8_MAX		4096	// 16x in Q8.8

#define AE_PEDESTAL			16		// sensor black pedestal in the Y plane
// Highlight-weighted metering: expose so the bright quartile (p75) of the
// frame sits here. Mean metering underexposes high-key scenes (a mostly-white
// subject gets dragged to grey); anchoring the bright parts renders them just
// below white after the tone curve, like a phone's metering.
#define AE_TARGET_DEFAULT	95		// raw p75 target (op30: 0 = this default)
#define AE_DEADBAND			8		// no adjustment within +/- this of target
// Per-step damping: scale ratio clamped to [1/3 .. 3] (Q8: 85..768)
#define AE_RATIO_Q8_MIN		85
#define AE_RATIO_Q8_MAX		768
#define AE_MAX_STEPS_PER_WAKE	3	// battery bound (ignored during live preview)

static uint16_t curExposure = AE_EXPOSURE_DEFAULT;
static uint16_t curGainCode = 0;
static uint8_t stepsThisWake = 0;

void ae_notifySensorInit(void) {
	// Registers just reverted to the init tables (+ any camreg staged file)
	curExposure = AE_EXPOSURE_DEFAULT;
	curGainCode = 0;
	stepsThisWake = 0;
}

/**
 * Bright-quartile (p75) luma of the Y plane, subsampled every 8th pixel.
 * Uses a 32-bin histogram: finds the bin where the cumulative count crosses
 * 75%, then refines with the bin's midpoint.
 */
static uint32_t brightLuma(uint32_t yAddr, uint16_t w, uint16_t h) {
	const uint8_t *y = (const uint8_t *)yAddr;
	uint32_t hist[32] = {0};
	uint32_t n = 0;

	SCB_InvalidateDCache_by_Addr((void *)yAddr, (int32_t)((uint32_t)w * h));

	for (uint16_t row = 0; row < h; row += 8) {
		const uint8_t *line = y + ((uint32_t)row * w);
		for (uint16_t col = 0; col < w; col += 8) {
			hist[line[col] >> 3]++;
			n++;
		}
	}
	if (n == 0) {
		return 0;
	}

	uint32_t threshold = (n * 3u) / 4u;		// 75th percentile
	uint32_t cum = 0;
	for (int bin = 0; bin < 32; bin++) {
		cum += hist[bin];
		if (cum >= threshold) {
			return ((uint32_t)bin << 3) + 4;	// bin midpoint
		}
	}
	return 255;
}

static void writeReg16(uint16_t regH, uint16_t value) {
	// The HM0360 MD companion shares the CIS I2C bus and can leave the
	// slave ID pointing at itself - always address the main camera
	cisdp_select_main_camera_i2c();
	// hx_drv_cis_set_reg(addr, val, 0) - same call the camreg command uses
	hx_drv_cis_set_reg(regH, (uint8_t)((value >> 8) & 0xFF), 0);
	hx_drv_cis_set_reg((uint16_t)(regH + 1), (uint8_t)(value & 0xFF), 0);
}

bool ae_process(uint32_t yAddr, uint16_t w, uint16_t h) {
	if ((yAddr == 0) || (w == 0) || (h == 0)) {
		return false;
	}
	if (fatfs_getOperationalParameter(OP_PARAMETER_CAM_AE_ENABLE) == 0) {
		return false;	// skip the measurement cost when AE is off
	}
	return ae_process_measured(brightLuma(yAddr, w, h));
}

bool ae_process_measured(uint32_t p75Raw) {
	uint32_t measured = p75Raw;

	if (fatfs_getOperationalParameter(OP_PARAMETER_CAM_AE_ENABLE) == 0) {
		return false;
	}
	if ((stepsThisWake >= AE_MAX_STEPS_PER_WAKE) && !preview_isActive()) {
		return false;
	}

	uint32_t target = fatfs_getOperationalParameter(OP_PARAMETER_CAM_AE_TARGET);
	if ((target == 0) || (target > 250)) {
		target = AE_TARGET_DEFAULT;
	}

	int32_t error = (int32_t)measured - (int32_t)target;
	if ((error >= -AE_DEADBAND) && (error <= AE_DEADBAND)) {
		return false;	// close enough
	}

	// Pedestal-corrected proportional ratio (Q8)
	uint32_t mEff = (measured > (AE_PEDESTAL + 2)) ? (measured - AE_PEDESTAL) : 2;
	uint32_t tEff = (target > AE_PEDESTAL) ? (target - AE_PEDESTAL) : 2;
	uint32_t ratioQ8 = (tEff << 8) / mEff;
	if (ratioQ8 < AE_RATIO_Q8_MIN) ratioQ8 = AE_RATIO_Q8_MIN;
	if (ratioQ8 > AE_RATIO_Q8_MAX) ratioQ8 = AE_RATIO_Q8_MAX;

	// Total light product P (lines, gain folded in), then rescale
	uint32_t gainQ8 = (1024u << 8) / (1024u - curGainCode);
	uint32_t pLines = ((uint32_t)curExposure * gainQ8) >> 8;	// <= 80k
	uint32_t newP = (pLines * ratioQ8) >> 8;					// <= 240k
	if (newP < AE_EXPOSURE_MIN) {
		newP = AE_EXPOSURE_MIN;
	}

	// Split: exposure first, remainder into analog gain
	uint32_t newExposure = newP;
	if (newExposure > AE_EXPOSURE_MAX) newExposure = AE_EXPOSURE_MAX;
	if (newExposure < AE_EXPOSURE_MIN) newExposure = AE_EXPOSURE_MIN;

	uint32_t newGainQ8 = (newP << 8) / newExposure;
	if (newGainQ8 < AE_GAIN_Q8_UNITY) newGainQ8 = AE_GAIN_Q8_UNITY;
	if (newGainQ8 > AE_GAIN_Q8_MAX) newGainQ8 = AE_GAIN_Q8_MAX;

	uint32_t newGainCode = 1024u - ((1024u << 8) / newGainQ8);
	if (newGainCode > AE_GAIN_CODE_MAX) newGainCode = AE_GAIN_CODE_MAX;

	// Skip the write if the change is negligible (avoids register churn)
	uint32_t expDelta = (newExposure > curExposure) ? (newExposure - curExposure)
			: (curExposure - newExposure);
	uint32_t gainDelta = (newGainCode > curGainCode) ? (newGainCode - curGainCode)
			: (curGainCode - newGainCode);
	if ((expDelta < (curExposure / 20u)) && (gainDelta < 8)) {
		return false;
	}

	xprintf("AE: p75=%d (target %d) exposure %d->%d gain code %d->%d\n",
			(int)measured, (int)target,
			(int)curExposure, (int)newExposure,
			(int)curGainCode, (int)newGainCode);

	if (newExposure != curExposure) {
		writeReg16(AE_REG_EXPOSURE_H, (uint16_t)newExposure);
		curExposure = (uint16_t)newExposure;
	}
	if (newGainCode != curGainCode) {
		writeReg16(AE_REG_GAIN_H, (uint16_t)newGainCode);
		curGainCode = (uint16_t)newGainCode;
	}
	stepsThisWake++;
	return true;
}
