/*
 * demosaic.h
 *
 * CPU Bayer demosaic for the high-resolution capture path (hires.c).
 *
 * The WE2's HW5x5 demosaic block tops out at 640x480, so frames captured
 * RAW at 1280x960 are demosaiced on the CM55M instead - one 16-row strip
 * at a time, feeding the streaming software JPEG encoder (sw_jpeg.h), so
 * no full-frame YUV buffer is ever needed. The colour pipeline (black
 * level -> WB -> CCM -> gamma, img_correct_transform_rgb()) is fused into
 * the same pass.
 *
 * Also provides raw-domain scene measurement (channel means for the auto
 * white balance, green-channel bright-quartile for the auto exposure), so
 * gains are computed once per frame - uniform across the image.
 */

#ifndef DEMOSAIC_H_
#define DEMOSAIC_H_

#include <stdint.h>

// CFA phase: colour of the (even col, even row) / (odd,even) / (even,odd) /
// (odd,odd) sites. Values match the order of DEMOS_PATTENMODE_* usage in
// cisdp_cfg.h via cisdp_get_demos_pattern().
typedef enum {
	DEMOSAIC_PATTERN_RGGB = 0,
	DEMOSAIC_PATTERN_GRBG = 1,
	DEMOSAIC_PATTERN_GBRG = 2,
	DEMOSAIC_PATTERN_BGGR = 3,
} demosaic_pattern_t;

/**
 * Subsampled raw-domain scene measurement.
 * Means are per-channel (R/G/B taken only from their own CFA sites);
 * gP75 is the green-channel bright-quartile (32-bin histogram midpoint),
 * the input for ae_process_measured().
 */
void demosaic_measure(const uint8_t *bayer, uint32_t w, uint32_t h,
					  demosaic_pattern_t pattern,
					  uint32_t *rMean, uint32_t *gMean, uint32_t *bMean,
					  uint32_t *gP75);

/**
 * Demosaic one strip of 16 rows (rows rowStart .. rowStart+15) to planar
 * YUV420 with the colour pipeline applied, laid out for
 * sw_jpeg_stream_strip(): Y (w*16), Cb (w/2 * 8), Cr (w/2 * 8).
 *
 * The full Bayer frame must be resident (neighbour rows are read across
 * strip boundaries; edges clamp). w and rowStart must be even; the strip
 * must lie fully inside the image.
 */
void demosaic_strip_yuv420(const uint8_t *bayer, uint32_t w, uint32_t h,
						   uint32_t rowStart, demosaic_pattern_t pattern,
						   uint16_t rGainQ8, uint16_t bGainQ8,
						   uint8_t *stripYuv);

#endif /* DEMOSAIC_H_ */
