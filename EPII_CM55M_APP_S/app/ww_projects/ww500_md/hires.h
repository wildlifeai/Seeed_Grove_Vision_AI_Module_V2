/*
 * hires.h
 *
 * Single-file 1280x960 capture for the RP camera ("Option B" - see
 * _Documentation/hires-capture.md).
 *
 * The WE2's hardware JPEG encoder and HW5x5 demosaic top out at 640x480,
 * which is why Himax's suggested workaround splits a 1280x960 raw frame
 * into four 640x480 JPEGs. This module instead captures the RAW Bayer
 * frame (the sensor streams a centred 1280x960 window of its 2x2-binned
 * readout; the INP passes it through untouched) and runs the whole
 * pipeline on the CPU in 16-row strips: bilinear demosaic ->
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

// Delivered line geometry: the INP swallows a fixed 32 wire-bytes of every
// RAW10 line in this pass-through mode (bench 12 Jul 2026, rawdump stride
// analysis), so a 1280-px sensor line lands as 1254/1255 bytes with +/-1
// jitter. demosaic_track_lines() locks each line's true start.
#define HIRES_LINE_STRIDE_A	1254u
#define HIRES_LINE_STRIDE_B	1255u

// Rows/columns actually processed/encoded. The sensor window is TALLER
// than the image (992 lines, IMX708_hires_window_setting) so the DMA byte
// count completes mid-frame. Valid line content ends at column ~1225: the
// final DMA burst of each line (columns ~1226..1241) carries corrupted
// alternating bytes (bench 13 Jul, per-column noise stats on a raw dump),
// so the width is the largest JPEG-strip multiple (16) below that.
#define HIRES_PROC_WIDTH	1216u
#define HIRES_PROC_HEIGHT	960u

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
 * Stage the raw-buffer target with the sensor driver. Must be called BEFORE
 * cisdp_sensor_init(): the 1280x960 sensor window is programmed during
 * sensor init and the MIPI/INP geometry follows it. Verifies the buffer
 * layout fits. Idempotent; hires_datapath_init() calls it too.
 * @return 0 on success
 */
int hires_stage(void);

/**
 * True after a successful hires_stage() (cleared by hires_deactivate()).
 * The sensor and datapath geometry must follow this, not hires_wanted().
 */
bool hires_isStaged(void);

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
 * Frame-timeout forensics: print how many bytes the last DMA arm delivered
 * (against the sentinel fill from hires_stage()) and refill the sentinel so
 * the next arm is measured independently. Call from the capture-retry path.
 */
void hires_dump_hwm_and_refill(void);

/**
 * Raw Bayer buffer accessor (diagnostics: the 'rawdump' CLI command).
 */
const uint8_t *hires_get_raw(uint32_t *lenOut);

/**
 * Process the raw frame just captured: measure (AE step + auto WB gains),
 * demosaic + colour-correct + JPEG-encode in strips, and register the
 * result via img_correct_set_external_jpeg() so prepareJpegFile() and the
 * live preview pick it up unchanged.
 * @return JPEG size in bytes, 0 on failure
 */
uint32_t hires_process_frame(void);

#endif /* HIRES_H_ */
