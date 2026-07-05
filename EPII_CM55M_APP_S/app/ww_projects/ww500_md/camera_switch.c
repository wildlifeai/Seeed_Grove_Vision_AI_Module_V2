/*
 * camera_switch.c
 *
 * Dual-image camera switching support. See camera_switch.h for the design.
 *
 * Manual switching is done with the 'switchslot' CLI command (CLI-commands.c),
 * which calls xip_switch_slot() directly. This module provides the slot
 * variant labelling that lets the app (via 'slots') see what is in each slot.
 *
 * The planned automatic light-based switching (AE-register inspection of
 * periodic captures, with hysteresis) will live here when implemented;
 * OP_PARAMETER_SLOT_SWITCH is reserved to enable it.
 */

#include "camera_switch.h"

#include "xprintf.h"

#include "xip_manager.h"

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
 * Record this image's variant against the currently active slot.
 *
 * xip_set_slot_variant() only writes flash when the label changes, so this is
 * cheap to call on every wake cycle.
 */
void cameraSwitch_labelBootSlot(void) {
	int activeSlot;
	uint8_t variant;

	variant = cameraSwitch_thisVariant();
	if (variant == XIP_SLOT_VARIANT_UNKNOWN) {
		return;
	}

	activeSlot = xip_get_active_slot();
	if (activeSlot < 0) {
		return;
	}

	xip_set_slot_variant((uint8_t) activeSlot, variant);
}
