/*
 * img_correct.c
 *
 * Software white balance for the RP (IMX708) camera, with a software JPEG
 * re-encode. See img_correct.h for the why.
 *
 * The RP camera has no white balance anywhere in its hardware pipeline (raw
 * Bayer sensor + WE2 HW5x5 demosaic-only), so images come out green. This runs
 * on the CPU between capture and file save:
 *   1. Apply per-channel WB gains to the YUV420 frame the HW5x5 wrote to memory.
 *   2. Re-encode the corrected YUV to JPEG in software (sw_jpeg.c).
 *
 * The hardware JPEG encoder cannot be driven from memory in this firmware's
 * datapath context (it hangs the datapath - see
 * _Documentation/RP3_white_balance_reencode_issue.md), so the re-encode is done
 * on the CPU. YUV420 is JPEG-native, so this is just DCT + quantise + Huffman.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "xprintf.h"
#include "printf_x.h"
#include "WE2_device.h"

#include "FreeRTOS.h"
#include "task.h"

#include "sw_jpeg.h"
#include "img_correct.h"

// JPEG quality for the re-encode (1..100). ~85 gives good quality at a size
// comparable to the hardware encoder's 4x table.
#define IMG_CORRECT_JPEG_QUALITY  85

/*************************************** Local variables *********************/

// Valid when the last capture was corrected + re-encoded
static bool corrected_valid;
static uint32_t corrected_size;
static uint32_t corrected_addr;

/*************************************** Buffer accessors ********************/

// Provided by the active cis_sensor/cisdp_sensor.c
extern uint32_t app_get_raw_addr(void);
extern uint32_t app_get_raw_width(void);
extern uint32_t app_get_raw_height(void);
extern uint32_t app_get_jpeg_addr(void);
extern uint32_t app_get_jpeg_sz(void);

/*************************************** White balance ***********************/

/**
 * Apply per-channel white-balance gains to a planar YUV420 image, in place.
 *
 * For each 2x2 luma block (sharing one Cb,Cr): convert to RGB (JPEG
 * full-range BT.601), scale R and B by the Q8.8 gains, convert back.
 * All arithmetic is integer; coefficients are the standard x256 values.
 */
static void wb_apply_yuv420(uint8_t *yuv, uint32_t w, uint32_t h,
							uint16_t rGain, uint16_t bGain) {
	uint8_t *yPlane = yuv;
	uint8_t *uPlane = yuv + (w * h);
	uint8_t *vPlane = uPlane + ((w * h) >> 2);
	uint32_t cw = w >> 1;
	uint32_t ch = h >> 1;

	for (uint32_t cy = 0; cy < ch; cy++) {
		uint8_t *u = uPlane + cy * cw;
		uint8_t *v = vPlane + cy * cw;
		uint8_t *y0 = yPlane + (cy * 2) * w;
		uint8_t *y1 = y0 + w;

		for (uint32_t cx = 0; cx < cw; cx++) {
			int32_t cb = (int32_t)u[cx] - 128;
			int32_t cr = (int32_t)v[cx] - 128;

			// Chroma-derived contributions (shared by the 4 luma pixels)
			int32_t rOfs = (359 * cr) >> 8;					// 1.402 * Cr'
			int32_t gOfs = (88 * cb + 183 * cr) >> 8;		// 0.344*Cb' + 0.714*Cr'
			int32_t bOfs = (454 * cb) >> 8;					// 1.772 * Cb'

			int32_t sumCb = 0;
			int32_t sumCr = 0;

			uint8_t *yPix[4] = { &y0[cx * 2], &y0[cx * 2 + 1],
								 &y1[cx * 2], &y1[cx * 2 + 1] };

			for (int i = 0; i < 4; i++) {
				int32_t Y = *yPix[i];

				int32_t R = Y + rOfs;
				int32_t G = Y - gOfs;
				int32_t B = Y + bOfs;

				// Clamp BEFORE the gains: out-of-gamut YUV combinations push R/B
				// past 255, and scaling those artifacts gives them more post-gain
				// weight than genuinely saturated pixels - clipped highlights then
				// come out unevenly tinted. Clamping first makes every saturated
				// pixel respond to the gain identically (and matches G's handling).
				if (R < 0) R = 0; else if (R > 255) R = 255;
				if (G < 0) G = 0; else if (G > 255) G = 255;
				if (B < 0) B = 0; else if (B > 255) B = 255;

				// White balance: scale red and blue (green is the reference)
				R = (R * rGain) >> 8;
				B = (B * bGain) >> 8;
				if (R > 255) R = 255;
				if (B > 255) B = 255;

				// Back to YCbCr (JPEG full range)
				int32_t newY = (77 * R + 150 * G + 29 * B) >> 8;
				if (newY > 255) newY = 255;
				*yPix[i] = (uint8_t)newY;

				sumCb += ((-43 * R - 85 * G + 128 * B) >> 8);
				sumCr += ((128 * R - 107 * G - 21 * B) >> 8);
			}

			int32_t newCb = 128 + (sumCb >> 2);
			int32_t newCr = 128 + (sumCr >> 2);
			if (newCb < 0) newCb = 0; else if (newCb > 255) newCb = 255;
			if (newCr < 0) newCr = 0; else if (newCr > 255) newCr = 255;
			u[cx] = (uint8_t)newCb;
			v[cx] = (uint8_t)newCr;
		}
	}
}

/*************************************** Public API **************************/

bool img_correct_process(uint16_t rGainQ8, uint16_t bGainQ8) {
	corrected_valid = false;

	// Gain 0 disables correction entirely -> the hardware sensor-path JPEG is
	// saved (this is the "no software encoding" case). Any non-zero gain runs
	// the software path: unity (256) re-encodes with no colour change - useful
	// for comparing the software encoder against the hardware one on identical
	// content - and larger gains apply white balance.
	if ((rGainQ8 == 0) || (bGainQ8 == 0)) {
		return false;
	}

	uint32_t yuvAddr = app_get_raw_addr();
	uint32_t w = app_get_raw_width();
	uint32_t h = app_get_raw_height();
	uint32_t jpgAddr = app_get_jpeg_addr();
	uint32_t jpgCap = app_get_jpeg_sz();

	if ((yuvAddr == 0) || (jpgAddr == 0) || (w == 0) || (h == 0) || (w & 1) || (h & 1)) {
		return false;
	}

	uint32_t start = xTaskGetTickCount();
	uint32_t yuvSize = (w * h * 3) / 2;

	// The frame was written to demosbuf by WDMA3 (DMA); invalidate the CPU's
	// stale cached copy before reading it.
	SCB_InvalidateDCache_by_Addr((void *)yuvAddr, (int32_t)yuvSize);

	// 1. White balance the YUV in place (CPU).
	wb_apply_yuv420((uint8_t *)yuvAddr, w, h, rGainQ8, bGainQ8);

	// 2. Re-encode to JPEG in software, straight into the JPEG buffer.
	uint32_t jpegSize = sw_jpeg_encode_yuv420((const uint8_t *)yuvAddr, w, h,
											  (uint8_t *)jpgAddr, jpgCap,
											  IMG_CORRECT_JPEG_QUALITY);
	if (jpegSize == 0) {
		xprintf("Colour correction: JPEG encode failed (buffer %u)\n", (unsigned)jpgCap);
		return false;   // fall back to the uncorrected sensor-path JPEG
	}

	// The JPEG was written by the CPU (cached). Flush it to memory so the later
	// SCB_InvalidateDCache in prepareJpegFile reads the correct bytes, and so
	// the FatFS DMA (if any) sees them.
	SCB_CleanDCache_by_Addr((void *)jpgAddr, (int32_t)jpegSize);

	corrected_valid = true;
	corrected_size = jpegSize;
	corrected_addr = jpgAddr;

	xprintf("Colour correction: R x%d/256, B x%d/256, SW-JPEG %u bytes in %ums\n",
			rGainQ8, bGainQ8, (unsigned)jpegSize,
			(unsigned)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS));

	return true;
}

void img_correct_get_jpeg(uint32_t *size, uint32_t *addr) {
	if (corrected_valid) {
		*size = corrected_size;
		*addr = corrected_addr;
	}
}
