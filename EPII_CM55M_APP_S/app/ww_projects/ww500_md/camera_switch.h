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
 * Switching is currently MANUAL: the app user selects a camera and the app
 * issues 'switchslot'.
 *
 * PLANNED: automatic light-based switching. The firmware will inspect the
 * camera AE registers (gain/exposure) of images captured every 15-30 minutes;
 * if the scene is too dark for the colour camera it will switch to the HM0360
 * image (and back when bright), with hysteresis. OP_PARAMETER_SLOT_SWITCH is
 * reserved to enable that mode (0 = off/manual only, 1 = automatic).
 *
 * See _Documentation/camera-field-tuning-roadmap.md
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

#endif /* CAMERA_SWITCH_H_ */
