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

// Per-line start offsets. The hi-res RAW datapath delivers slightly SHORT
// lines with +/-1 byte jitter (the INP swallows a fixed 32 wire-bytes per
// line, fractional in pixels - see hires-capture.md), so line n does not
// start at n*w. demosaic_track_lines() locks each line's start against the
// same-parity line two above; when the table is not set, rows are dense
// at stride w (the natural layout).
#define DEMOSAIC_MAX_LINES	960u
static uint32_t s_lineOff[DEMOSAIC_MAX_LINES];
static uint32_t s_lineCnt = 0;

static inline const uint8_t *row_ptr(const uint8_t *b, uint32_t w, uint32_t y) {
	if (y < s_lineCnt) {
		// The per-line loss is TAIL truncation (drain side), so every line
		// starts at sensor column 0: the offset itself carries no column-
		// phase meaning and must be used as-is. (A "+1 on odd offsets"
		// correction here produced full-width colour banding - 12 Jul.)
		return b + s_lineOff[y];
	}
	return b + (uint32_t)y * w;
}

// Sparse sum of absolute differences between two candidate line starts
static uint32_t line_sad(const uint8_t *a, const uint8_t *b) {
	uint32_t s = 0;
	for (uint32_t i = 64; i < 1184; i += 6) {
		int32_t d = (int32_t)a[i] - (int32_t)b[i];
		s += (uint32_t)(d < 0 ? -d : d);
	}
	return s;
}

// Parity signal of a candidate line start: sum(even positions) minus
// sum(odd positions) over the line. On the TRUE offset this reflects the
// CFA channel split of the row (R-G rows vs G-B rows); off-by-one offsets
// flip its sign. Which sign is "correct" depends on the scene's channel
// balance, so the tracker is run under all four per-row-parity sign
// hypotheses and the winner is selected by green-diagonal equality below.
static int32_t line_par(const uint8_t *p) {
	int32_t s = 0;
	for (uint32_t i = 0; i + 1 < 1240u; i += 2) {
		s += (int32_t)p[i] - (int32_t)p[i + 1];
	}
	return s;
}

// Working table for the hypothesis under evaluation
static uint32_t s_tryOff[DEMOSAIC_MAX_LINES];

// One tracking pass under a fixed sign hypothesis. sEven/sOdd are +1/-1:
// the penalty pushes row-parity classes toward negative/positive parity.
// Returns the line count that fit in `avail`.
static uint32_t track_pass(const uint8_t *bayer, uint32_t h,
						   uint32_t strideA, uint32_t strideB, uint32_t avail,
						   int32_t sEven, int32_t sOdd, uint32_t *off) {
	off[0] = 0;
	for (uint32_t n = 1; n < h; n++) {
		uint32_t prev = off[n - 1];
		uint32_t ref = (n >= 2) ? off[n - 2] : off[0];
		int32_t s = (n & 1u) ? sOdd : sEven;
		uint32_t oa = prev + strideA;
		uint32_t ob = prev + strideB;
		if (ob + strideB > avail) {
			return n;	// ran out of delivered bytes
		}
		int32_t pa = s * line_par(bayer + oa);
		int32_t pb = s * line_par(bayer + ob);
		uint32_t ca = line_sad(bayer + ref, bayer + oa)
					  + ((pa > 0) ? ((uint32_t)pa >> 1) : 0u);
		uint32_t cb = line_sad(bayer + ref, bayer + ob)
					  + ((pb > 0) ? ((uint32_t)pb >> 1) : 0u);
		off[n] = (ca <= cb) ? oa : ob;
	}
	return h;
}

// Physical-truth selector: with correct offsets the two green diagonals of
// the Bayer quad have (nearly) equal means; with a phase error they are
// really the R and B planes and differ. Sampled sums, |G1-G2| in x16 units.
static uint32_t green_split(const uint8_t *bayer, const uint32_t *off, uint32_t h) {
	int64_t g1 = 0, g2 = 0;
	uint32_t rows = 0;
	for (uint32_t n = 0; n + 1 < h; n += 4) {
		const uint8_t *e = bayer + off[n];		// even row: G at odd cols
		const uint8_t *o = bayer + off[n + 1];	// odd row: G at even cols
		for (uint32_t x = 8; x < 1240u; x += 8) {
			g1 += e[x | 1u];
			g2 += o[x & ~1u];
		}
		rows++;
	}
	if (rows == 0) {
		return 0;
	}
	int64_t d = g1 - g2;
	if (d < 0) {
		d = -d;
	}
	return (uint32_t)(d / (int64_t)rows);
}

void demosaic_track_lines(const uint8_t *bayer, uint32_t h,
						  uint32_t strideA, uint32_t strideB, uint32_t avail) {
	if (h > DEMOSAIC_MAX_LINES) {
		h = DEMOSAIC_MAX_LINES;
	}
	s_lineCnt = 0;
	if (h < 3) {
		return;
	}

	uint32_t bestScore = 0xFFFFFFFFu;
	uint32_t bestCnt = 0;
	for (uint32_t hyp = 0; hyp < 4; hyp++) {
		int32_t sEven = (hyp & 1u) ? 1 : -1;
		int32_t sOdd = (hyp & 2u) ? 1 : -1;
		uint32_t cnt = track_pass(bayer, h, strideA, strideB, avail,
								  sEven, sOdd, s_tryOff);
		uint32_t score = green_split(bayer, s_tryOff, cnt);
		if (score < bestScore) {
			bestScore = score;
			bestCnt = cnt;
			for (uint32_t i = 0; i < cnt; i++) {
				s_lineOff[i] = s_tryOff[i];
			}
		}
	}
	s_lineCnt = bestCnt;
}

void demosaic_clear_line_table(void) {
	s_lineCnt = 0;
}

void demosaic_repair_zeros(uint8_t *bayer, uint32_t w, uint32_t h) {
	// The IMX708 emits short runs of literal 0x00 at fixed (masked-PDAF)
	// positions in the windowed hi-res mode; the colour pipeline turns
	// them into saturated dashes. Raw zero never occurs naturally (the
	// black level is ~16), so zero is a safe defect marker: replace with
	// the same-CFA-colour neighbour two rows away.
	for (uint32_t y = 0; y < h; y++) {
		uint8_t *row = (uint8_t *)row_ptr(bayer, w, y);
		const uint8_t *src = row_ptr(bayer, w, (y >= 2) ? (y - 2) : (y + 2));
		for (uint32_t x = 0; x < w; x++) {
			if (row[x] == 0u) {
				row[x] = src[x];
			}
		}
	}
}

// Colour of the CFA site at (x, y) for a pattern: 0=R 1=G 2=B
static inline int site_colour(demosaic_pattern_t p, uint32_t x, uint32_t y) {
	static const uint8_t site[4][4] = {
		// (even,even) (odd,even) (even,odd) (odd,odd)
		{ 0, 1, 1, 2 },		// RGGB
		{ 1, 0, 2, 1 },		// GRBG
		{ 1, 2, 0, 1 },		// GBRG
		{ 2, 1, 1, 0 },		// BGGR
	};
	// Mask (branchless - this runs per pixel) so an out-of-range pattern value
	// can never index past site[3]; all valid patterns are 0-3.
	return site[(uint32_t)p & 3u][((y & 1u) << 1) | (x & 1u)];
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
	return (int32_t)row_ptr(b, w, cy)[cx];
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
					uint8_t v = row_ptr(bayer, w, y + dy)[x + dx];
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
