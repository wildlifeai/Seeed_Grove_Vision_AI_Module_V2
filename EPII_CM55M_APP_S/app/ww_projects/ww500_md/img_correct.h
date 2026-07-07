/*
 * img_correct.h
 *
 * Software colour correction for the RP (IMX708) camera.
 *
 * Neither the IMX708 (raw Bayer sensor - the V4L2 "colour balance" registers
 * 0x0B90/0x0B92 only NOTIFY the sensor of gains applied by an external ISP)
 * nor the WE2 HW5x5 block (demosaic + FIR only) applies white balance, so the
 * JPEG comes out green: measured G/R = 1.12, G/B = 1.27 on neutral grey.
 * See _Documentation/camera-field-tuning-roadmap.md
 *
 * The fix implemented here runs on the CPU between capture and file save:
 *   1. Apply per-channel white-balance gains to the YUV420 frame that the
 *      HW5x5 has already written to memory (fixed-point YUV->RGB->gain->YUV).
 *   2. Re-encode the corrected frame to JPEG using the hardware encoder via
 *      the TPG datapath (RDMA reads the corrected YUV from memory).
 *
 * Gains are Q8.8 (256 = 1.0x) and come from Operational Parameters so the
 * mobile app can tune them. 0 disables the correction entirely.
 */

#ifndef IMG_CORRECT_H_
#define IMG_CORRECT_H_

#include <stdint.h>
#include <stdbool.h>

// Neutralising gains measured on the bench (white paper, indoor light):
// G/R = 1.119 -> red x1.119 (286/256), G/B = 1.275 -> blue x1.275 (326/256)
#define IMG_CORRECT_RED_GAIN_DEFAULT	286
#define IMG_CORRECT_BLUE_GAIN_DEFAULT	326
#define IMG_CORRECT_GAIN_UNITY			256

/**
 * Colour-correct the current capture and re-encode it to JPEG.
 *
 * Applies white-balance gains to the YUV420 frame in the raw (demosaic
 * output) buffer, then re-encodes it with the hardware JPEG encoder so the
 * saved file is the corrected image. The result replaces the sensor-path
 * JPEG; prepareJpegFile() picks it up via img_correct_get_jpeg().
 *
 * No-op (returns false) when both gains are unity or either is 0 (disabled),
 * or when the re-encode fails - in that case the original JPEG is used.
 *
 * @param rGainQ8  red gain, Q8.8 (256 = 1.0x); 0 disables correction
 * @param bGainQ8  blue gain, Q8.8 (256 = 1.0x); 0 disables correction
 * @return true if the frame was corrected and re-encoded
 */
bool img_correct_process(uint16_t rGainQ8, uint16_t bGainQ8);

/**
 * If the last capture was corrected and re-encoded, overwrite *size and *addr
 * with the corrected JPEG's size and address; otherwise leave them untouched.
 * Call from prepareJpegFile() after cisdp_get_jpginfo().
 */
void img_correct_get_jpeg(uint32_t *size, uint32_t *addr);

#endif /* IMG_CORRECT_H_ */
