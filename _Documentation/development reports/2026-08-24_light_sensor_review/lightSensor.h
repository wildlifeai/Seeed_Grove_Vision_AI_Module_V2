/*
 * lightSensor.h  --  CANDIDATE FOR REVIEW, NOT WIRED INTO THE BUILD
 *
 * Drafted for the "Advise on moving light meter code to a separate .c & .h
 * file" sub-task of CLAUDE_light_sensor_review.md. This file lives in the
 * review folder, not in ww500_md/, until Charles has reviewed it. No source
 * files have been changed to match it yet.
 *
 * Supersedes the earlier light_meter.h draft in this folder: that draft
 * exposed HM0360_AE_STATS_T and the AE_SAMPLE_COUNT/AE_HYSTERESIS/etc.
 * #defines as part of the public API. This version hides all of that -
 * callers get behaviour (is it dark? what's the reading? take a fresh
 * reading), not the HM0360-specific mechanics behind it. See "Design notes"
 * at the end of this file for what that costs and what it buys.
 *
 * Author: Claude (drafted for review by Charles Palmer)
 * Date: 25 Aug 2026
 */

#ifndef LIGHTSENSOR_H_
#define LIGHTSENSOR_H_

/********************************** Includes ******************************************/

#include <stdint.h>
#include <stdbool.h>

// Deliberately nothing else. No hm0360_md.h, no ledFlash.h: every function
// below takes/returns only plain types, so callers do not need to know the
// light sensor is currently an HM0360 read in a particular way.

/*****************************************************************************
 * SECTION 1 - PROPOSED CHANGES TO hm0360_md.c / hm0360_md.h
 *
 * lightSensor.c (not shown here - .c files are for after your review) needs
 * 3 small additions to the HM0360 driver, all following the existing
 * hm0360_md_getGainRegs() pattern (checks hm0360_present, wraps the I2C
 * slave-ID swap via the private saveMainCameraConfig()/restoreMainCameraConfig()
 * helpers). These are implementation details of lightSensor.c, not part of
 * its own public API below - shown here because they touch a different file
 * and need review before anything is written.
 *
 *   1. hm0360_md_getGainCeilings() - NEW - reads MAX_AGAIN / MAX_DGAIN_H/L,
 *      needed for the gain-railed ("too dark to expose for") detection.
 *   2. hm0360_md_getMode()           - NEW - reads MODE_SELECT.
 *   3. hm0360_md_setModeSelectOnly() - NEW - writes MODE_SELECT, with none
 *      of hm0360_md_setMode()'s side effects (that function forces an
 *      interim MODE_SLEEP, rewrites PMU_CFG_3/7/8/9, and toggles the MD
 *      interrupt - all wrong for a transient "wake sensor to sample, then
 *      put it back exactly as it was" nudge, and hm0360_md_setMode() can't
 *      even be called correctly for the "put it back" half since the
 *      original context/numFrames/sleepTime were never captured).
 *
 * hm0360_md_getAEStats() and HM0360_AE_STATS_T are proposed for REMOVAL from
 * hm0360_md.c/.h - lightSensor.c recreates that sampling loop itself from
 * these 3 primitives plus the existing hm0360_md_getGainRegs(), and keeps
 * the aggregated-stats representation entirely private to itself.
 *****************************************************************************/

/**
 * @brief Read the HM0360's configured AE gain ceilings.
 *
 * MAX_AGAIN (0x202b) and MAX_DGAIN_H/L define when the AE loop has "railed"
 * - run its gain to maximum and so cannot expose any darker.
 *
 * Decode notes (no HM0360 datasheet register-field description available,
 * inferred from the Himax reference init table):
 *   - maxAnalogGain is the low 3 bits of MAX_AGAIN (0x202b & 0x07).
 *   - maxDigitalGain uses the SAME decode formula as the DIGITAL_GAIN
 *     readout in hm0360_md_getGainRegs(), deliberately, so a
 *     'digitalGain >= maxDigitalGain' comparison stays internally
 *     consistent even though the absolute scaling is unverified.
 *
 * Safe to call regardless of which sensor is the main camera - wraps the
 * I2C slave-ID swap the same way hm0360_md_getGainRegs() does.
 *
 * @param maxAnalogGain   [out] Analog gain ceiling code. Not written if the
 *                        HM0360 is missing.
 * @param maxDigitalGain  [out] Digital gain ceiling, same units/scale as
 *                        HM0360_GAIN_T.digitalGain. Not written if the
 *                        HM0360 is missing.
 * @return HX_CIS_NO_ERROR on success. HX_CIS_UNKNOWN_ERROR if the HM0360 is
 *         not present (outputs left unwritten), otherwise the first I2C
 *         error encountered reading the three registers.
 */
// HX_CIS_ERROR_E hm0360_md_getGainCeilings(uint8_t *maxAnalogGain, uint16_t *maxDigitalGain);

/**
 * @brief Read the HM0360's current streaming mode (MODE_SELECT, 0x0100).
 *
 * A plain, side-effect-free register read - the getter counterpart to the
 * existing hm0360_md_setMode() setter. Do NOT use hm0360_md_setMode() to
 * restore a mode read via this getter - use hm0360_md_setModeSelectOnly()
 * instead (see below).
 *
 * Safe to call regardless of which sensor is the main camera - wraps the
 * I2C slave-ID swap the same way hm0360_md_getGainRegs() does.
 *
 * @param mode  [out] The current MODE_SELECT value. Not written if the
 *              HM0360 is missing or the register read fails.
 * @return HX_CIS_NO_ERROR on success, otherwise the I2C error from the
 *         register read (including HX_CIS_UNKNOWN_ERROR if the HM0360 is
 *         not present).
 */
// HX_CIS_ERROR_E hm0360_md_getMode(mode_select_t *mode);

/**
 * @brief Write MODE_SELECT directly, with none of hm0360_md_setMode()'s
 * side effects (no forced MODE_SLEEP interlude, no PMU_CFG_3/7/8/9 rewrite,
 * no MD interrupt enable/disable toggle).
 *
 * Intended for a transient streaming nudge - e.g. waking the sensor so AE
 * registers can be sampled - where the surrounding sleep-count/interrupt/
 * context configuration must be left exactly as it already was, and must be
 * restorable afterward with a second call to this same function (passing
 * back whatever hm0360_md_getMode() returned beforehand). For a real,
 * persistent mode transition use hm0360_md_setMode() instead.
 *
 * @param mode  The mode to write to MODE_SELECT.
 * @return HX_CIS_NO_ERROR on success, otherwise the I2C error from the
 *         register write (including HX_CIS_UNKNOWN_ERROR if the HM0360 is
 *         not present).
 */
// HX_CIS_ERROR_E hm0360_md_setModeSelectOnly(mode_select_t mode);

// (Commented out above since these belong in hm0360_md.h, not lightSensor.h
// - included here as text for review only, so the real prototypes are not
// mistakenly compiled from this file.)


/*****************************************************************************
 * SECTION 2 - lightSensor.h proper
 *
 * Deliberately behavioural, not mechanistic: nothing here mentions the
 * HM0360, AE registers, sample counts, or hysteresis bands. Those become
 * private to lightSensor.c (see "Design notes" at the end of this file).
 *****************************************************************************/

/**
 * @brief Whether anything currently needs a light-level reading.
 *
 * True when either the AE-driven flash or automatic day/night camera
 * switching is enabled (i.e. either consumer of this module's decision is
 * turned on in the operational parameters). Replaces the 2-line test that
 * was previously repeated at 3 call sites in image_task.c - callers should
 * compute this once, early in a wake cycle, and reuse the result rather
 * than calling it repeatedly.
 *
 * @return true if lightSensor_takeReading() would actually be used by
 *         something this wake; false if it would be wasted effort.
 */
bool lightSensor_isRequired(void);

/**
 * @brief Take a fresh light-level reading and update this module's idea of
 * the current brightness and dark/bright state.
 *
 * Internally samples the sensor several times over roughly a couple of
 * seconds (a single reading is not a reliable brightness measure - see
 * "Design notes"), so this call blocks for a similar length of time. Safe
 * to call even if the sensor was asleep - it will be woken for the
 * sampling window and returned to its prior state afterward. Updates the
 * values lightSensor_getReading() and lightSensor_isDark() will return, and
 * persists the dark/bright decision so it survives the next sleep.
 *
 * No-op (does nothing, changes nothing) if !lightSensor_isRequired() -
 * callers are not required to check that themselves first, but should
 * prefer to when skipping the call's ~2s cost matters (e.g. between frames
 * of a multi-image burst - only call this after the last frame).
 *
 * MUST keep printing the "AE light check: mean AE = N (min N, max N) over N
 * frames, threshold = N, gain railed = yes|no -> DARK|BRIGHT" console line
 * (currently in ledFlashNewAEStats(), ledFlash.c) - _Tools/ae_monitor.py
 * parses it live off the console for bench testing (AE_RE in that script:
 * `(?:AE Mean|mean AE)\s*=\s*(\d+).*?threshold\s*=\s*(\d+).*?->\s*(DARK|BRIGHT|flash ON|flash OFF)`).
 * That regex is loose - it strips ANSI colour first and doesn't require the
 * "AE light check:" prefix or the [LS] tag, just "mean AE = N ... threshold
 * = N ... -> DARK|BRIGHT" in that order - but the keywords "mean AE",
 * "threshold", "->", "DARK"/"BRIGHT" must appear verbatim, in that order,
 * from wherever this print ends up inside lightSensor.c.
 */
void lightSensor_takeReading(void);

/**
 * @brief The most recent brightness reading.
 *
 * Currently the HM0360's AE_MEAN register, averaged over the sampling
 * window from the last lightSensor_takeReading() call (0-255, higher =
 * brighter). This unit is an implementation detail of the current sensor
 * and may change if the light sensor is ever reworked to report in a more
 * generic brightness unit - callers should treat this as "a number that
 * goes up in brighter conditions", not attach meaning to specific values
 * beyond that, if avoidable.
 *
 * Undefined (returns 0) if lightSensor_takeReading() has not yet been
 * called since boot/wake. Unlike lightSensor_isDark(), this value is not
 * persisted across sleep.
 *
 * @return The last brightness reading, or 0 if none has been taken yet.
 */
uint16_t lightSensor_getReading(void);

/**
 * @brief Whether the scene is currently decided to be "dark" (flash/IR
 * camera wanted) or "bright" (no flash/colour camera wanted).
 *
 * This is a hysteresis-filtered decision, not a direct threshold test on
 * lightSensor_getReading() - see "Design notes". It also persists across
 * sleep (restored from non-volatile storage at boot), so it has a sensible
 * answer even before lightSensor_takeReading() has been called this wake -
 * unlike lightSensor_getReading().
 *
 * @return true if the current decision is "dark", false if "bright".
 */
bool lightSensor_isDark(void);

// No lightSensor_getCheckDelaySeconds() - see "Design notes" below. Arming
// the periodic RTC wake-up is plain image_task.c/DPD-sleep business: read
// OP_PARAMETER_AE_CHECK_INTERVAL directly (as image_sleepNow() already does
// for other timings), gated by lightSensor_isRequired(). Nothing about that
// conversion is light-sensor-internal enough to be worth hiding.

#endif /* LIGHTSENSOR_H_ */


/*****************************************************************************
 * DESIGN NOTES (not part of the header - for review only)
 *
 * What moved out of the public API, and why it's now safe to hide:
 *
 * - HM0360_AE_STATS_T (samples/meanAE/minAE/maxAE/maxAnalogGain/
 *   maxDigitalGain/railedCount/gainRailed) becomes a plain local variable
 *   inside lightSensor_takeReading() - nothing outside lightSensor.c ever
 *   needs the per-sample detail, only the two summarised outcomes
 *   (getReading(), isDark()).
 * - AE_SAMPLE_COUNT, AE_SAMPLE_GAP_MS, AE_HYSTERESIS become private
 *   #defines (or static consts) at the top of lightSensor.c. Nothing
 *   outside ever passed different values in practice (both current call
 *   sites always used the constants), so the parameterised
 *   lightMeter_sample(nSamples, gapMs, ...) shape from the earlier draft
 *   was unnecessary generality - removed.
 * - The old lightMeter_decideFromSingleReading() (fallback when sampling
 *   fails) becomes a private static helper inside lightSensor.c, called
 *   automatically from within lightSensor_takeReading() - callers never
 *   need to know sampling can fail or that there's a fallback path.
 * - The "hm0360_md_getAEStats() took Nms" timing print moves inside
 *   lightSensor_takeReading() too - callers no longer wrap the call in
 *   their own xTaskGetTickCount()/app_getElapsedMs() pair.
 *
 * MUST NOT be lost in the move: the "AE light check: mean AE = ..." console
 * line, currently printed by ledFlashNewAEStats() (ledFlash.c), which is
 * being deleted. _Tools/ae_monitor.py parses this line live off the console
 * for bench testing - see the full constraint spelled out on
 * lightSensor_takeReading() above. Whatever internal function ends up
 * making the dark/bright decision inside lightSensor.c must keep printing
 * it, in a form ae_monitor.py's AE_RE regex still matches.
 *
 * What did NOT move in, on Charles's review: an earlier draft of this file
 * also had lightSensor_getCheckDelaySeconds(), wrapping
 * OP_PARAMETER_AE_CHECK_INTERVAL's minutes-to-seconds conversion and the
 * 65535 RTC-alarm clamp. Cut - that conversion isn't light-sensor-internal
 * knowledge, it's DPD/RTC-alarm mechanics that belongs with the rest of
 * image_sleepNow()'s timing logic. image_task.c reads
 * OP_PARAMETER_AE_CHECK_INTERVAL directly, still gated by
 * lightSensor_isRequired(). The reading interval need not be exact (per
 * Charles), which is a further reason not to build an abstraction around it.
 *
 * A behavioural question this raises, not yet decided - who drives the LED
 * flash hardware? Previously ledFlashNewAEStats() decided dark/bright AND
 * called ledFlashActivate() in the same function. With the decision now
 * inside lightSensor.c and the hardware call inside ledFlash.c, something
 * has to connect them. Proposed: lightSensor.c does NOT call into ledFlash.c
 * at all (keeps the dependency one-directional: image_task -> lightSensor,
 * image_task -> ledFlash, no lightSensor -> ledFlash) - instead image_task.c
 * does, after calling lightSensor_takeReading():
 *
 *     if (ledFlashGetFlashMode() == FLASH_MODE_AE) {
 *         ledFlash_setActive(lightSensor_isDark());
 *     }
 *     cameraSwitch_autoSwitchCheck();
 *
 * ledFlash_setActive(bool) is a new small ledFlash.h setter (replacing
 * direct writes to the private flashActive static) - the one dependent
 * change outside hm0360_md.c/lightSensor.c this design needs. Not drafted
 * here since it's a one-line addition to an existing header; can be spelled
 * out fully if wanted.
 *
 * A second, smaller question: camera_switch.c currently reads
 * OP_PARAMETER_AE_FLASH_STATE directly rather than through any function.
 * That still works unchanged (lightSensor_isDark() reads/writes the same
 * parameter), but camera_switch.c could optionally call
 * lightSensor_isDark() instead for consistency, now that a proper accessor
 * exists. Not required, just an observation - camera_switch.c was not part
 * of what was asked for here.
 *****************************************************************************/
