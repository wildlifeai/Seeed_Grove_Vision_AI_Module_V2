/*
 * sw_jpeg.h
 *
 * Minimal baseline (sequential DCT) JPEG encoder for planar YUV420 (4:2:0)
 * images, running entirely on the CPU. Used to re-encode the software
 * white-balance-corrected frame (see img_correct.c) - the WE2 hardware JPEG
 * encoder cannot be driven from memory in this firmware's datapath context
 * (see _Documentation/RP3_white_balance_reencode_issue.md).
 *
 * YUV420 is JPEG's native chroma format, so no colour conversion is needed:
 * the planes are DCT'd, quantised and Huffman-coded directly. Standard
 * (Annex K) quantisation and Huffman tables are used.
 */

#ifndef SW_JPEG_H_
#define SW_JPEG_H_

#include <stdint.h>

/**
 * Encode a planar YUV420 image to a baseline JFIF/JPEG bitstream.
 *
 * Plane layout at yuv: Y (w*h), then Cb (w/2 * h/2), then Cr (w/2 * h/2).
 * w and h should be multiples of 16 (the 4:2:0 MCU size); other sizes are
 * handled by edge replication.
 *
 * @param yuv      planar YUV420 source
 * @param w,h      image dimensions in pixels
 * @param out      output buffer for the JPEG
 * @param outCap   capacity of out in bytes
 * @param quality  1..100 (higher = better quality / larger file)
 * @return JPEG size in bytes, or 0 on error (bad args / output overflow)
 */
uint32_t sw_jpeg_encode_yuv420(const uint8_t *yuv, uint32_t w, uint32_t h,
                               uint8_t *out, uint32_t outCap, uint8_t quality);

/*
 * Streaming (strip) API - encode an image 16 luma rows at a time, so a
 * frame larger than free SRAM never needs a full YUV buffer (used by the
 * 1280x960 high-resolution capture, which demosaics the raw frame strip by
 * strip). Produces a bitstream identical to sw_jpeg_encode_yuv420().
 */

typedef struct {
	uint8_t  *buf;			// output buffer
	uint32_t  cap;
	uint32_t  len;
	uint32_t  bits;			// bit accumulator (internal)
	int       nbits;
	uint8_t   overflow;
	uint32_t  w, h;
	uint32_t  rowsDone;		// luma rows consumed so far
	int       dcY, dcCb, dcCr;	// DC predictors carried across strips
} sw_jpeg_stream_t;

/**
 * Start a streamed encode: writes the JFIF headers for a w x h image.
 * w must be a multiple of 2; strips are 16 luma rows each, so h is padded
 * by replication in the final strip if not a multiple of 16.
 * @return 0 on success, -1 on bad arguments
 */
int sw_jpeg_stream_begin(sw_jpeg_stream_t *s, uint32_t w, uint32_t h,
                         uint8_t *out, uint32_t outCap, uint8_t quality);

/**
 * Encode the next strip of 16 luma rows. stripYuv is planar YUV420 local to
 * the strip: Y (w*16), then Cb (w/2 * 8), then Cr (w/2 * 8). For the final
 * strip of an image whose height is not a multiple of 16, fill the unused
 * rows by replicating the last valid row.
 * @return 0 on success, -1 if called past the image height or after overflow
 */
int sw_jpeg_stream_strip(sw_jpeg_stream_t *s, const uint8_t *stripYuv);

/**
 * Finish the stream (entropy flush + EOI).
 * @return JPEG size in bytes, or 0 on overflow / incomplete image
 */
uint32_t sw_jpeg_stream_end(sw_jpeg_stream_t *s);

#endif /* SW_JPEG_H_ */
