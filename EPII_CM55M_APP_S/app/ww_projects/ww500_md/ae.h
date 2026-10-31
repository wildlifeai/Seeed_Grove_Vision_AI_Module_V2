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
 *
 * The exposure and gain reached are kept in OP_PARAMETER_CAM_AE_EXPOSURE and
 * OP_PARAMETER_CAM_AE_GAIN (ops 37/38, saved to CONFIG.TXT before DPD) and
 * restored at the next sensor init, so each wake starts where the last one
 * finished instead of at the init-table value (#158).
 */

#ifndef AE_H_
#define AE_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>

/*********************************************** Global Defines **********************************************/

// Limits of the AE loop, also the valid ranges of ops 37/38 (fatfs_isValidOperationalParameter())
#define AE_EXPOSURE_MIN		8		// lines (bright daylight needs very short exposures)
#define AE_EXPOSURE_MAX		5000	// lines (bench-verified; frame time grows beyond)
#define AE_GAIN_CODE_MAX	960		// 16x  (gain = 1024 / (1024 - code))


/*********************************************** Global Types ************************************************/


/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

/**
 * @brief Restart AE after a sensor init, from the saved exposure if there is one.
 *
 * Call after a full sensor init (cisdp_sensor_init(true)) and the staged
 * register file - the registers have just reverted to the tables. If AE is on
 * and ops 37/38 hold a saved exposure and gain, they are written to the sensor
 * and the loop continues from them; otherwise it restarts from the table values.
 */
void ae_notifySensorInit(void);

/**
 * @brief Measure the frame and adjust exposure/gain toward the target.
 *
 * Call from the image task on FRAME_READY, before the next capture is armed.
 *
 * @param yAddr address of the Y plane (demosaic output, w*h bytes)
 * @param w     frame width
 * @param h     frame height
 * @return true if a register adjustment was made (next frame will differ)
 */
bool ae_process(uint32_t yAddr, uint16_t w, uint16_t h);

#endif /* AE_H_ */
