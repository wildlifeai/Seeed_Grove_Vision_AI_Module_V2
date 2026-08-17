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

#endif /* SW_JPEG_H_ */
