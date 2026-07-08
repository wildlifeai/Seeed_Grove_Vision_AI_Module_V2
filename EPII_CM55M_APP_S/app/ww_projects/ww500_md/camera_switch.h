/*
 * camera_switch.h
 *
 * Dual-image camera switching support.
 *
 * The WW500 uses two firmware images built from the same source:
 *   - HM0360 image (USE_HM0360): mono, sees IR - used in the dark with the IR flash
 *   - RP3 image (USE_RP3, IMX708): colour, better quality - used in daylight
 *
 * Both images live in the two XIP flash slots (see xip_manager.c). Each image
 * labels its own slot at boot, so the 'slots' CLI command (and the app) can see
 * which variant is in each slot, and 'switchslot' boots the other one.
 *
 * Switching modes:
 *   - MANUAL (always available): the app user selects a camera and the app
 *     issues 'switchslot'.
 *   - AUTOMATIC (OP_PARAMETER_SLOT_SWITCH == 1): after each AE light check
 *     (HM0360 AE registers, sampled around captures and every
 *     OP_PARAMETER_AE_CHECK_INTERVAL minutes) the hysteresis-filtered
 *     dark/bright decision (OP_PARAMETER_AE_FLASH_STATE) is compared with the
 *     running variant; when they disagree - and the other slot is labelled
 *     with the wanted variant - cameraSwitch_autoSwitchCheck() switches the
 *     boot slot and schedules a reset at the next sleep.
 *
 * See _Documentation/camera-field-tuning-roadmap.md and
 * _Documentation/AE_Light_Sensor_Roadmap.md
 */

#ifndef CAMERA_SWITCH_H_
#define CAMERA_SWITCH_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * The camera variant this firmware was built as (XIP_SLOT_VARIANT_x).
 */
uint8_t cameraSwitch_thisVariant(void);

/**
 * Human-readable name for a variant value. Never returns NULL.
 */
const char * cameraSwitch_variantName(uint8_t variant);

/**
 * Record this image's variant against the currently active slot.
 * Cheap when already recorded (no flash write). Call once per wake cycle.
 */
void cameraSwitch_labelBootSlot(void);

/**
 * Automatic switching check - call after each AE light-level decision.
 * When OP_PARAMETER_SLOT_SWITCH == 1 and the light wants the other camera
 * variant, switches the boot slot and schedules a reset at the next sleep.
 * Returns true if a switch was scheduled.
 */
bool cameraSwitch_autoSwitchCheck(void);

#endif /* CAMERA_SWITCH_H_ */
