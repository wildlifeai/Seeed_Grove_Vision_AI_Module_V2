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

/*
 * Phase C colour pipeline (order mirrors the Raspberry Pi ISP for this
 * sensor): black level -> WB gains -> colour correction matrix -> gamma.
 * Reference data from libcamera's imx708.json tuning file - see
 * _Documentation/rp3-image-quality-plan.md section 2.
 */

// Sensor black pedestal (imx708.json black_level 4096/65536 = 16/255) and the
// x256 rescale that restores full range after subtraction: 256*255/(255-16)
#define BLACK_LEVEL			16
#define BLACK_RESCALE_Q8	273

// 4640 K colour correction matrix, Q8.8, rows renormalised to sum 256.
// (Daylight-ish; good general default. CCT-switched blending is Phase C3.)
static const int32_t CCM_Q8[9] = {
	392,  -90,  -46,
	-72,  427,  -99,
	  4, -146,  398,
};

// Gamma LUT from the imx708.json contrast curve (16-bit anchors 0->0,
// 1024->5040, 4096->15312, 16384->40642, 65535->65535), resampled to 8-bit.
// Strong shadow lift, gentle highlight shoulder.
static const uint8_t GAMMA_LUT[256] = {
	  0,   5,  10,  15,  20,  23,  26,  30,  33,  36,  40,  43,  46,  50,  53,  56,
	 60,  62,  64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  87,  89,  91,
	 93,  95,  97,  99, 101, 103, 105, 107, 109, 111, 113, 115, 117, 119, 122, 124,
	126, 128, 130, 132, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 155, 157,
	158, 159, 159, 160, 160, 161, 161, 162, 162, 163, 163, 164, 164, 165, 165, 166,
	166, 167, 167, 168, 168, 169, 169, 170, 170, 171, 171, 172, 172, 173, 173, 174,
	174, 175, 175, 176, 176, 177, 178, 178, 179, 179, 180, 180, 181, 181, 182, 182,
	183, 183, 184, 184, 185, 185, 186, 186, 187, 187, 188, 188, 189, 189, 190, 190,
	191, 191, 192, 192, 193, 193, 194, 194, 195, 195, 196, 196, 197, 197, 198, 198,
	199, 199, 200, 200, 201, 201, 202, 202, 203, 203, 204, 204, 205, 205, 206, 206,
	207, 207, 208, 208, 209, 209, 210, 210, 211, 211, 212, 212, 213, 213, 214, 214,
	215, 215, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 222, 223,
	223, 224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231,
	231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239,
	239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247,
	247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255,
};

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

				// Black level -> WB gains -> CCM -> gamma (shared transform)
				img_correct_transform_rgb(&R, &G, &B, rGain, bGain);

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

/*************************************** Shared colour transform *************/

void img_correct_transform_rgb(int32_t *r, int32_t *g, int32_t *b,
							   uint16_t rGainQ8, uint16_t bGainQ8) {
	int32_t R = *r, G = *g, B = *b;

	// Black level: subtract the sensor pedestal, restore range
	R = (R > BLACK_LEVEL) ? (((R - BLACK_LEVEL) * BLACK_RESCALE_Q8) >> 8) : 0;
	G = (G > BLACK_LEVEL) ? (((G - BLACK_LEVEL) * BLACK_RESCALE_Q8) >> 8) : 0;
	B = (B > BLACK_LEVEL) ? (((B - BLACK_LEVEL) * BLACK_RESCALE_Q8) >> 8) : 0;

	// White balance: scale red and blue (green is the reference)
	R = (R * rGainQ8) >> 8;
	B = (B * bGainQ8) >> 8;
	if (R > 255) R = 255;
	if (B > 255) B = 255;

	// Colour correction matrix (4640 K, Q8.8)
	int32_t Rc = (CCM_Q8[0] * R + CCM_Q8[1] * G + CCM_Q8[2] * B) >> 8;
	int32_t Gc = (CCM_Q8[3] * R + CCM_Q8[4] * G + CCM_Q8[5] * B) >> 8;
	int32_t Bc = (CCM_Q8[6] * R + CCM_Q8[7] * G + CCM_Q8[8] * B) >> 8;
	if (Rc < 0) Rc = 0; else if (Rc > 255) Rc = 255;
	if (Gc < 0) Gc = 0; else if (Gc > 255) Gc = 255;
	if (Bc < 0) Bc = 0; else if (Bc > 255) Bc = 255;

	// Tone curve
	*r = GAMMA_LUT[Rc];
	*g = GAMMA_LUT[Gc];
	*b = GAMMA_LUT[Bc];
}

/*************************************** Auto white balance ******************/

// Warmth bias so "balanced" matches the phone reference rendering rather than
// strict grey: measured target on the bright quartile is G/R ~ 0.94 (red a
// touch above green) and G/B ~ 1.06 (blue a touch below). x256 fixed point.
#define WB_AUTO_R_BIAS_Q8	271		// 1.06: red ends ~6% above grey-world
#define WB_AUTO_B_BIAS_Q8	242		// 0.94: blue ends ~6% below grey-world
#define WB_AUTO_GAIN_MIN	230		// 0.9x  - clamp band from the imx708.json
#define WB_AUTO_GAIN_MAX	640		// 2.5x    CT-curve endpoints, with margin
#define WB_AUTO_PEDESTAL	16		// sensor black pedestal, subtracted first

/**
 * Grey-world measurement: subsampled RGB means of the YUV420 frame.
 * Samples every 4th chroma column of every 4th chroma row (1/16 of blocks).
 */
static void wb_measure_yuv420(const uint8_t *yuv, uint32_t w, uint32_t h,
							  uint32_t *rMean, uint32_t *gMean, uint32_t *bMean) {
	const uint8_t *yPlane = yuv;
	const uint8_t *uPlane = yuv + (w * h);
	const uint8_t *vPlane = uPlane + ((w * h) >> 2);
	uint32_t cw = w >> 1;
	uint32_t ch = h >> 1;
	uint32_t rSum = 0, gSum = 0, bSum = 0, n = 0;

	for (uint32_t cy = 0; cy < ch; cy += 4) {
		const uint8_t *u = uPlane + cy * cw;
		const uint8_t *v = vPlane + cy * cw;
		const uint8_t *y0 = yPlane + (cy * 2) * w;

		for (uint32_t cx = 0; cx < cw; cx += 4) {
			int32_t Y = y0[cx * 2];
			int32_t cb = (int32_t)u[cx] - 128;
			int32_t cr = (int32_t)v[cx] - 128;

			int32_t R = Y + ((359 * cr) >> 8);
			int32_t G = Y - ((88 * cb + 183 * cr) >> 8);
			int32_t B = Y + ((454 * cb) >> 8);
			if (R < 0) R = 0; else if (R > 255) R = 255;
			if (G < 0) G = 0; else if (G > 255) G = 255;
			if (B < 0) B = 0; else if (B > 255) B = 255;

			rSum += (uint32_t)R;
			gSum += (uint32_t)G;
			bSum += (uint32_t)B;
			n++;
		}
	}
	if (n == 0) n = 1;
	*rMean = rSum / n;
	*gMean = gSum / n;
	*bMean = bSum / n;
}

bool img_correct_gains_from_means(uint32_t rMean, uint32_t gMean, uint32_t bMean,
								  uint16_t *rGainQ8, uint16_t *bGainQ8) {
	// Pedestal-correct; if the scene is essentially black, don't guess
	uint32_t rM = (rMean > WB_AUTO_PEDESTAL) ? (rMean - WB_AUTO_PEDESTAL) : 0;
	uint32_t gM = (gMean > WB_AUTO_PEDESTAL) ? (gMean - WB_AUTO_PEDESTAL) : 0;
	uint32_t bM = (bMean > WB_AUTO_PEDESTAL) ? (bMean - WB_AUTO_PEDESTAL) : 0;
	if ((gM < 4) || (rM < 2) || (bM < 2)) {
		return false;
	}

	uint32_t rGain = (WB_AUTO_R_BIAS_Q8 * gM) / rM;
	uint32_t bGain = (WB_AUTO_B_BIAS_Q8 * gM) / bM;
	if (rGain < WB_AUTO_GAIN_MIN) rGain = WB_AUTO_GAIN_MIN;
	if (rGain > WB_AUTO_GAIN_MAX) rGain = WB_AUTO_GAIN_MAX;
	if (bGain < WB_AUTO_GAIN_MIN) bGain = WB_AUTO_GAIN_MIN;
	if (bGain > WB_AUTO_GAIN_MAX) bGain = WB_AUTO_GAIN_MAX;

	xprintf("WB auto: means R=%u G=%u B=%u -> gains R x%u/256, B x%u/256\n",
			(unsigned)rM, (unsigned)gM, (unsigned)bM,
			(unsigned)rGain, (unsigned)bGain);

	*rGainQ8 = (uint16_t)rGain;
	*bGainQ8 = (uint16_t)bGain;
	return true;
}

/**
 * Compute warmth-biased grey-world gains for the current frame.
 * Returns false if the frame is too dark to measure reliably.
 */
static bool wb_auto_gains(const uint8_t *yuv, uint32_t w, uint32_t h,
						  uint16_t *rGainQ8, uint16_t *bGainQ8) {
	uint32_t rM, gM, bM;
	wb_measure_yuv420(yuv, w, h, &rM, &gM, &bM);
	return img_correct_gains_from_means(rM, gM, bM, rGainQ8, bGainQ8);
}

/*************************************** Public API **************************/

bool img_correct_process_mode(uint8_t mode, uint16_t rManualQ8, uint16_t bManualQ8,
							  bool flashLit) {
	if (mode == IMG_CORRECT_MODE_OFF) {
		corrected_valid = false;
		return false;
	}
	if (mode == IMG_CORRECT_MODE_AUTO && !flashLit) {
		uint32_t yuvAddr = app_get_raw_addr();
		uint32_t w = app_get_raw_width();
		uint32_t h = app_get_raw_height();
		uint16_t rAuto, bAuto;

		if ((yuvAddr != 0) && (w != 0) && (h != 0)) {
			// The frame is DMA-written; refresh the CPU's view before measuring
			SCB_InvalidateDCache_by_Addr((void *)yuvAddr, (int32_t)((w * h * 3) / 2));
			if (wb_auto_gains((const uint8_t *)yuvAddr, w, h, &rAuto, &bAuto)) {
				return img_correct_process(rAuto, bAuto);
			}
		}
		// Too dark / no buffer: fall through to the manual gains
	}
	// Manual mode, flash-lit frame (spectrum differs - keep it predictable),
	// or auto measurement failed
	return img_correct_process(rManualQ8, bManualQ8);
}

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

void img_correct_set_external_jpeg(uint32_t size, uint32_t addr) {
	corrected_valid = (size > 0) && (addr != 0);
	corrected_size = size;
	corrected_addr = addr;
}
