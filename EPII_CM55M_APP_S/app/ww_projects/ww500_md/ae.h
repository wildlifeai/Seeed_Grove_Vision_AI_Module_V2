/*
 * ae.h
 *
 * Minimal auto-exposure for the RP (Sony IMX) cameras.
 *
 * The IMX708/IMX219 are raw sensors with no on-sensor AE, and the WE2
 * datapath has none either, so exposure was a hardcoded register value
 * (0x0202 = 0x0940) and images were dim or blown depending on the light.
 *
 * This module runs after each captured frame: it measures the mean of the
 * Y plane the HW5x5 wrote to memory, compares it with a target, and steps
 * the sensor's exposure (0x0202/03) and analog gain (0x0204/05) toward the
 * target. Exposure is used first; analog gain (up to 16x) only when the
 * exposure range (bench-verified 100..5000 lines) is not enough.
 *
 * Bounded to a few adjustments per wake cycle to protect the battery;
 * unbounded while live preview is active (see preview.c) so convergence
 * is visible in the stream.
 *
 * Op-params: OP_PARAMETER_CAM_AE_ENABLE (0 = off), OP_PARAMETER_CAM_AE_TARGET
 * (target mean luma, default 110). Bench data: _Documentation/rp3-image-quality-plan.md
 */

#ifndef AE_H_
#define AE_H_

#include <stdint.h>
#include <stdbool.h>

/**
 * Reset AE state to the sensor's table defaults. Call after a full sensor
 * init (cisdp_sensor_init(true)) - the registers have just reverted, so the
 * loop must restart from the table values.
 */
void ae_notifySensorInit(void);

/**
 * Measure the frame and adjust exposure/gain toward the target.
 * Call from the image task on FRAME_READY, before the next capture is armed.
 *
 * @param yAddr  address of the Y plane (demosaic output, w*h bytes)
 * @param w,h    frame dimensions
 * @return true if a register adjustment was made (next frame will differ)
 */
bool ae_process(uint32_t yAddr, uint16_t w, uint16_t h);

#endif /* AE_H_ */
