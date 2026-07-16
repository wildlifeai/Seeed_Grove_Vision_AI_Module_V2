/*
 * demosaic.c
 *
 * CPU bilinear Bayer demosaic + fused colour pipeline. See demosaic.h.
 *
 * Bilinear rules per site:
 *   at a red/blue site:   the other diagonal colour = avg of 4 diagonals,
 *                         green = avg of 4 edge neighbours
 *   at a green site:      red/blue = avg of the 2 horizontal or 2 vertical
 *                         neighbours (which is which depends on the row)
 * Edges clamp. Quality is adequate for a wildlife trail-cam JPEG; the
 * sharper malvar kernel can be swapped in later without API changes.
 */

#include <stdint.h>
#include <stdbool.h>

#include "img_correct.h"	// img_correct_transform_rgb()
#include "demosaic.h"

// Colour of the CFA site at (x, y) for a pattern: 0=R 1=G 2=B
static inline int site_colour(demosaic_pattern_t p, uint32_t x, uint32_t y) {
	static const uint8_t site[4][4] = {
		// (even,even) (odd,even) (even,odd) (odd,odd)
		{ 0, 1, 1, 2 },		// RGGB
		{ 1, 0, 2, 1 },		// GRBG
		{ 1, 2, 0, 1 },		// GBRG
		{ 2, 1, 1, 0 },		// BGGR
	};
	return site[p][((y & 1u) << 1) | (x & 1u)];
}

static inline uint32_t clamp_u(int32_t v, int32_t lo, int32_t hi) {
	if (v < lo) return (uint32_t)lo;
	if (v > hi) return (uint32_t)hi;
	return (uint32_t)v;
}

// Fetch with edge clamp
static inline int32_t px(const uint8_t *b, uint32_t w, uint32_t h,
						 int32_t x, int32_t y) {
	uint32_t cx = clamp_u(x, 0, (int32_t)w - 1);
	uint32_t cy = clamp_u(y, 0, (int32_t)h - 1);
	return (int32_t)b[cy * w + cx];
}

/**
 * Reconstruct R,G,B at (x,y) by bilinear interpolation.
 */
static void demosaic_px(const uint8_t *b, uint32_t w, uint32_t h,
						demosaic_pattern_t p, int32_t x, int32_t y,
						int32_t *R, int32_t *G, int32_t *B) {
	int c = site_colour(p, (uint32_t)x, (uint32_t)y);
	int32_t self = px(b, w, h, x, y);

	int32_t edges = (px(b, w, h, x - 1, y) + px(b, w, h, x + 1, y)
			+ px(b, w, h, x, y - 1) + px(b, w, h, x, y + 1) + 2) >> 2;
	int32_t diags = (px(b, w, h, x - 1, y - 1) + px(b, w, h, x + 1, y - 1)
			+ px(b, w, h, x - 1, y + 1) + px(b, w, h, x + 1, y + 1) + 2) >> 2;
	int32_t horiz = (px(b, w, h, x - 1, y) + px(b, w, h, x + 1, y) + 1) >> 1;
	int32_t vert  = (px(b, w, h, x, y - 1) + px(b, w, h, x, y + 1) + 1) >> 1;

	if (c == 0) {						// red site
		*R = self;
		*G = edges;
		*B = diags;
	}
	else if (c == 2) {					// blue site
		*B = self;
		*G = edges;
		*R = diags;
	}
	else {								// green site
		*G = self;
		// which colour lies horizontally beside this green depends on the row:
		// the row containing red sites has R horizontal, B vertical (and vice
		// versa). Row contains red iff site_colour at (x^1, y) == 0.
		if (site_colour(p, (uint32_t)(x ^ 1), (uint32_t)y) == 0) {
			*R = horiz;
			*B = vert;
		}
		else {
			*R = vert;
			*B = horiz;
		}
	}
}

void demosaic_measure(const uint8_t *bayer, uint32_t w, uint32_t h,
					  demosaic_pattern_t pattern,
					  uint32_t *rMean, uint32_t *gMean, uint32_t *bMean,
					  uint32_t *gP75) {
	uint32_t sum[3] = {0, 0, 0};
	uint32_t cnt[3] = {0, 0, 0};
	uint32_t hist[32] = {0};
	uint32_t nG = 0;

	// Sample every 8th row/column pair (keeps all four CFA phases sampled)
	for (uint32_t y = 0; y + 1 < h; y += 8) {
		for (uint32_t x = 0; x + 1 < w; x += 8) {
			// the 2x2 cell at (x,y) contains each colour at least once
			for (uint32_t dy = 0; dy < 2; dy++) {
				for (uint32_t dx = 0; dx < 2; dx++) {
					uint8_t v = bayer[(y + dy) * w + (x + dx)];
					int c = site_colour(pattern, x + dx, y + dy);
					sum[c] += v;
					cnt[c]++;
					if (c == 1) {
						hist[v >> 3]++;
						nG++;
					}
				}
			}
		}
	}

	*rMean = cnt[0] ? (sum[0] / cnt[0]) : 0;
	*gMean = cnt[1] ? (sum[1] / cnt[1]) : 0;
	*bMean = cnt[2] ? (sum[2] / cnt[2]) : 0;

	uint32_t threshold = (nG * 3u) / 4u;
	uint32_t cum = 0;
	*gP75 = 255;
	for (int bin = 0; bin < 32; bin++) {
		cum += hist[bin];
		if (cum >= threshold) {
			*gP75 = ((uint32_t)bin << 3) + 4;
			break;
		}
	}
}

void demosaic_strip_yuv420(const uint8_t *bayer, uint32_t w, uint32_t h,
						   uint32_t rowStart, demosaic_pattern_t pattern,
						   uint16_t rGainQ8, uint16_t bGainQ8,
						   uint8_t *stripYuv) {
	uint8_t *yPlane = stripYuv;
	uint8_t *uPlane = stripYuv + (w * 16u);
	uint8_t *vPlane = uPlane + ((w / 2u) * 8u);
	uint32_t cw = w / 2u;

	// Process in 2x2 blocks (YUV420: 4 Y share one Cb,Cr)
	for (uint32_t by = 0; by < 16; by += 2) {
		uint32_t srcY = rowStart + by;
		for (uint32_t bx = 0; bx < w; bx += 2) {
			int32_t sumCb = 0, sumCr = 0;

			for (uint32_t dy = 0; dy < 2; dy++) {
				for (uint32_t dx = 0; dx < 2; dx++) {
					int32_t R, G, B;
					demosaic_px(bayer, w, h, pattern,
								(int32_t)(bx + dx), (int32_t)(srcY + dy), &R, &G, &B);

					// black level -> WB -> CCM -> gamma (shared pipeline)
					img_correct_transform_rgb(&R, &G, &B, rGainQ8, bGainQ8);

					// RGB -> YCbCr (JPEG full range, x256 coefficients)
					int32_t Y = (77 * R + 150 * G + 29 * B) >> 8;
					if (Y > 255) Y = 255;
					yPlane[(by + dy) * w + (bx + dx)] = (uint8_t)Y;

					sumCb += ((-43 * R - 85 * G + 128 * B) >> 8);
					sumCr += ((128 * R - 107 * G - 21 * B) >> 8);
				}
			}

			int32_t cb = 128 + (sumCb >> 2);
			int32_t cr = 128 + (sumCr >> 2);
			if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
			if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
			uPlane[(by >> 1) * cw + (bx >> 1)] = (uint8_t)cb;
			vPlane[(by >> 1) * cw + (bx >> 1)] = (uint8_t)cr;
		}
	}
}
