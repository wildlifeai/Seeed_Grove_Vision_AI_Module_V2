/*
 * camera_switch.c
 *
 * Dual-image camera switching support. See camera_switch.h for the design.
 *
 * Manual switching is done with the 'switchslot' CLI command (CLI-commands.c),
 * which calls xip_switch_slot() directly. This module provides the slot
 * variant labelling that lets the app (via 'slots') see what is in each slot,
 * and the automatic light-based switching (OP_PARAMETER_SLOT_SWITCH == 1):
 * cameraSwitch_autoSwitchCheck() is called by the image task after each AE
 * light check and points the bootloader at the other slot when the light
 * level wants the other camera.
 */

#include "camera_switch.h"

#include "xprintf.h"

#include "xip_manager.h"
#include "fatfs_task.h"
#include "ww500_md.h"

/**
 * The camera variant this firmware was built as.
 */
uint8_t cameraSwitch_thisVariant(void) {
// USE_RP3 and USE_HM0360 are mutually exclusive in ww500_md.mk (RP3 builds
// get USE_HM0360_MD instead), but check USE_RP3 first as a defence in case
// a future configuration defines both
#if defined(USE_RP3)
	return XIP_SLOT_VARIANT_RP3;
#elif defined(USE_HM0360)
	return XIP_SLOT_VARIANT_HM0360;
#else
	// Other camera builds (RP2 etc.) do not participate in camera switching yet
	return XIP_SLOT_VARIANT_UNKNOWN;
#endif
}

/**
 * Human-readable name for a variant value.
 */
const char * cameraSwitch_variantName(uint8_t variant) {
	switch (variant) {
	case XIP_SLOT_VARIANT_HM0360:
		return "HM0360 (night/IR)";
	case XIP_SLOT_VARIANT_RP3:
		return "RP3 (day/colour)";
	default:
		return "unknown";
	}
}

/**
 * Automatic day/night camera switching (OP_PARAMETER_SLOT_SWITCH == 1).
 *
 * Called by the image task after each AE light check (every capture, and the
 * periodic OP_PARAMETER_AE_CHECK_INTERVAL wakes). If the hysteresis-filtered
 * light decision (OP_PARAMETER_AE_FLASH_STATE: 1 = dark) wants the OTHER
 * camera variant - dark while running the day/colour image, or bright while
 * running the night/IR image - and the other slot is labelled with exactly
 * that variant, point the bootloader at it and schedule a reset for the next
 * sleep (the same graceful mechanism as the 'switchslot' command).
 *
 * Stability: the decision itself carries hysteresis (AE_HYSTERESIS), so it
 * does not chatter at dusk/dawn; the decision persists across the reboot, so
 * the newly booted image sees decision == its own variant and stays put; and
 * a scheduled switch is latched so repeated checks before the sleep cannot
 * fire twice.
 *
 * @return true if a switch was scheduled (caller may notify the app)
 */
bool cameraSwitch_autoSwitchCheck(void) {
	static bool switchScheduled = false;

	if (switchScheduled) {
		return false;	// already on our way to the other camera
	}

	if (fatfs_getOperationalParameter(OP_PARAMETER_SLOT_SWITCH) != 1) {
		return false;	// automatic switching not enabled
	}

	uint8_t self = cameraSwitch_thisVariant();
	if (self == XIP_SLOT_VARIANT_UNKNOWN) {
		return false;	// this build does not participate (e.g. RP2)
	}

	bool dark = (fatfs_getOperationalParameter(OP_PARAMETER_AE_FLASH_STATE) == 1);
	uint8_t wanted = dark ? XIP_SLOT_VARIANT_HM0360 : XIP_SLOT_VARIANT_RP3;
	if (wanted == self) {
		return false;	// already running the right camera for the light level
	}

	int activeSlot = xip_get_active_slot();
	if (activeSlot < 0) {
		return false;
	}

	// Only switch into a slot that has BOOTED and labelled itself as the
	// wanted variant - never into an unknown or mismatched image.
	int otherVariant = xip_get_slot_variant((activeSlot == 0) ? 1 : 0);
	if (otherVariant != (int)wanted) {
		xprintf("Auto camera switch: light wants '%s' but other slot holds '%s' - staying\n",
				cameraSwitch_variantName(wanted),
				cameraSwitch_variantName((uint8_t)((otherVariant < 0) ? 0 : otherVariant)));
		return false;
	}

	int newSlot = xip_switch_slot();
	if (newSlot < 0) {
		xprintf("Auto camera switch failed (%d)\n", newSlot);
		return false;
	}

	switchScheduled = true;
	app_setResetRequest(true);	// reboot into the other image at the next sleep
	xprintf("Auto camera switch: light is %s -> slot %d ('%s'). Reset scheduled.\n",
			dark ? "DARK" : "BRIGHT", newSlot, cameraSwitch_variantName(wanted));
	return true;
}

/**
 * Record this image's variant against the currently active slot.
 *
 * xip_set_slot_variant() only writes flash when the label changes, so this is
 * cheap to call on every wake cycle.
 */
void cameraSwitch_labelBootSlot(void) {
	int activeSlot;
	int result;
	uint8_t variant;

	variant = cameraSwitch_thisVariant();
	if (variant == XIP_SLOT_VARIANT_UNKNOWN) {
		return;		// build variant does not participate in camera switching
	}

	// Failures below are LOUD and retried once: a missed label strands the
	// app's camera switching on 'unknown' until this image happens to boot
	// again. Seen once in the field: the label write was silently lost during
	// the brief mid-update boot of a dual-image firmware update.
	activeSlot = xip_get_active_slot();
	if (activeSlot < 0) {
		xprintf("labelBootSlot: cannot read active slot - variant label NOT written\n");
		return;
	}

	result = xip_set_slot_variant((uint8_t) activeSlot, variant);
	if (result != 0) {
		xprintf("labelBootSlot: slot %c label write failed (%d) - retrying once\n",
				(activeSlot == 0) ? 'A' : 'B', result);
		result = xip_set_slot_variant((uint8_t) activeSlot, variant);
		if (result != 0) {
			xprintf("labelBootSlot: retry failed (%d) - camera switching will see 'unknown'\n", result);
		}
	}
}
