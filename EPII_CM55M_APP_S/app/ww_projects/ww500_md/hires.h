/*
 * hires.h
 *
 * Single-file 1280x960 capture for the RP camera ("Option B" - see
 * _Documentation/hires-capture.md).
 *
 * The WE2's hardware JPEG encoder and HW5x5 demosaic top out at 640x480,
 * which is why Himax's suggested workaround splits a 1280x960 raw frame
 * into four 640x480 JPEGs. This module instead captures the RAW Bayer
 * frame (INP centre-crop of the sensor's 2304x1296 stream) and runs the
 * whole pipeline on the CPU in 16-row strips: bilinear demosaic ->
 * black level -> auto WB -> CCM -> gamma -> streaming software JPEG.
 * Result: ONE JPEG, one EXIF block, no tile seams, no server-side
 * stitching - at ~2-3 s per capture.
 *
 * Memory (all within existing regions, checked at runtime):
 *   raw frame 1,228,800 B  -> overlays the (NN-idle) tensor arena + the
 *                             free SRAM tail after it
 *   JPEG out + strip buffer -> overlay demosbuf (unused: no WDMA3 here)
 * High-resolution capture therefore REQUIRES the NN to be disabled
 * (op14 = 0), matching the "trail-cam without NN" use case.
 *
 * Selected per wake via OP_PARAMETER_CAM_RESOLUTION (op32): 0 = 640x480
 * (normal integrated pipeline), 1 = 1280x960 via this module.
 */

#ifndef HIRES_H_
#define HIRES_H_

#include <stdint.h>
#include <stdbool.h>

#define HIRES_WIDTH		1280u
#define HIRES_HEIGHT	960u

/**
 * True when high-resolution capture can run: op32 selects it, the camera
 * is an RP colour camera, and the NN is not loaded (its arena holds the
 * raw frame).
 */
bool hires_wanted(void);

/**
 * True after hires_datapath_init() succeeded - the current datapath is the
 * RAW capture path and FRAME_READY events carry a raw Bayer frame.
 */
bool hires_isActive(void);

/**
 * Configure the RAW datapath (call in place of the normal cisdp_dp_init()
 * flow, before the sensor starts). Verifies the buffer layout fits.
 * @return 0 on success
 */
int hires_datapath_init(void *cb_event);

/**
 * Deactivate (restores the normal WDMA2 binding for the next init).
 */
void hires_deactivate(void);

/**
 * Process the raw frame just captured: measure (AE step + auto WB gains),
 * demosaic + colour-correct + JPEG-encode in strips, and register the
 * result via img_correct_set_external_jpeg() so prepareJpegFile() and the
 * live preview pick it up unchanged.
 * @return JPEG size in bytes, 0 on failure
 */
uint32_t hires_process_frame(void);

#endif /* HIRES_H_ */
