/*
 * hires.c
 *
 * Single-file 1280x960 capture. See hires.h for the architecture and
 * _Documentation/hires-capture.md for the design rationale and bench data.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>		// memset (diag sentinel fill)

#include "WE2_core.h"		// SCB_InvalidateDCache_by_Addr
#include "xprintf.h"

#include "FreeRTOS.h"
#include "task.h"

#include "fatfs_task.h"
#include "hx_drv_xdma.h"	// hx_drv_xdma_set_wdma2_enable (freeze during encode)
#include "cisdp_sensor.h"
#include "img_correct.h"
#include "demosaic.h"
#include "sw_jpeg.h"
#include "ae.h"
#include "cvapp.h"			// cv_modelLoaded()
#include "hires.h"

#define HIRES_JPEG_QUALITY	85

// Raw Bayer frame: 1280*960 = 1,228,800 B. Overlays the tensor arena
// (512 KB, idle when no NN model is loaded) and runs into the free SRAM
// tail after it (nothing else is linked beyond the arena - see the .map).
extern uint8_t __tensor_arena_start__;
#define SRAM_REGION_END		0x34200000u

// JPEG output and the strip buffer overlay demosbuf (460,800 B), which the
// RAW path never touches (no WDMA3 write). Strip: Y 1248*16 + Cb/Cr 624*8*2.
extern uint8_t demosbuf[];
#define HIRES_STRIP_BYTES	((HIRES_PROC_WIDTH * 16u) + 2u * ((HIRES_PROC_WIDTH / 2u) * 8u))
#define HIRES_JPEG_CAP		(460800u - HIRES_STRIP_BYTES)

static bool active = false;
static bool staged = false;

static uint8_t *rawBuf(void)   { return &__tensor_arena_start__; }
static uint8_t *jpegOut(void)  { return demosbuf; }
static uint8_t *stripBuf(void) { return demosbuf + HIRES_JPEG_CAP; }

bool hires_wanted(void) {
#if defined(USE_RP2) || defined(USE_RP3)
	if (fatfs_getOperationalParameter(OP_PARAMETER_CAM_RESOLUTION) != 1) {
		return false;
	}
	if (cv_modelLoaded()) {
		// The raw frame lives in the NN arena - the two are exclusive
		xprintf("Hi-res: NN model loaded - falling back to 640x480 "
				"(set op14 = 0 for high resolution)\n");
		return false;
	}
	return true;
#else
	return false;
#endif
}

bool hires_isActive(void) {
	return active;
}

int hires_stage(void) {
	uint32_t rawStart = (uint32_t)rawBuf();
	uint32_t rawEnd = rawStart + (HIRES_WIDTH * HIRES_HEIGHT);

	if (rawEnd > SRAM_REGION_END) {
		xprintf("Hi-res: raw buffer 0x%08x..0x%08x exceeds SRAM end - disabled\n",
				(unsigned)rawStart, (unsigned)rawEnd);
		return -1;
	}

	// Registers the raw target with the sensor driver. Must happen BEFORE
	// cisdp_sensor_init(): the 1280x960 sensor window is programmed during
	// sensor init, and the MIPI/INP geometry follows it (cisdp_sensor.c).
	cisdp_set_hires_raw(rawStart);

	// Sentinel fill: cisdp_dump_diag() scans for the DMA high-water mark at
	// a WDMA2 abnormal, measuring exactly how many bytes the INP delivered
	memset((void *)rawStart, 0xA5, HIRES_WIDTH * HIRES_HEIGHT);
	SCB_CleanDCache_by_Addr((void *)rawStart, (int32_t)(HIRES_WIDTH * HIRES_HEIGHT));

	staged = true;
	return 0;
}

bool hires_isStaged(void) {
	return staged;
}

int hires_datapath_init(void *cb_event) {
	if (hires_stage() < 0) {
		return -1;
	}

	// RAW path: sensor-windowed frame -> INP pass-through -> WDMA2. Same
	// init call as the normal flow; the hires override (staged above)
	// redirects geometry and target.
	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INP_WDMA2,
					  (sensordplib_CBEvent_t)cb_event, 4,
					  APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		xprintf("Hi-res: RAW datapath init failed\n");
		hires_deactivate();
		return -1;
	}

	active = true;
	xprintf("Hi-res capture active: %ux%u RAW -> CPU pipeline (raw @0x%08x, "
			"jpeg cap %u)\n", (unsigned)HIRES_WIDTH, (unsigned)HIRES_HEIGHT,
			(unsigned)(uint32_t)rawBuf(), (unsigned)HIRES_JPEG_CAP);
	return 0;
}

void hires_deactivate(void) {
	active = false;
	staged = false;
	cisdp_set_hires_raw(0);
	demosaic_clear_line_table();
}

const uint8_t *hires_get_raw(uint32_t *lenOut) {
	if (lenOut != NULL) {
		*lenOut = HIRES_WIDTH * HIRES_HEIGHT;
	}
	return rawBuf();
}

void hires_dump_hwm_and_refill(void) {
	if (!active) {
		return;
	}
	const uint32_t total = HIRES_WIDTH * HIRES_HEIGHT;
	uint8_t *buf = rawBuf();

	// High-water mark of the last DMA arm against the 0xA5 sentinel fill:
	// measures exactly how many bytes the INP delivered before the abnormal
	SCB_InvalidateDCache_by_Addr((void *)buf, (int32_t)total);
	uint32_t hw = total;
	while (hw > 0 && buf[hw - 1] == 0xA5) {
		hw--;
	}
	xprintf("HIRES hwm %u of %u bytes (%u.%u lines of %u)\n",
			(unsigned)hw, (unsigned)total,
			(unsigned)(hw / HIRES_WIDTH),
			(unsigned)((hw % HIRES_WIDTH) * 10u / HIRES_WIDTH),
			(unsigned)HIRES_WIDTH);

	// Refill so the NEXT arm is measured independently
	memset(buf, 0xA5, total);
	SCB_CleanDCache_by_Addr((void *)buf, (int32_t)total);
}

uint32_t hires_process_frame(void) {
	if (!active) {
		return 0;
	}

	const uint8_t *raw = rawBuf();
	uint32_t start = xTaskGetTickCount();

	// Freeze the DMA before touching the buffer: the lib's RAW WDMA2 is
	// cyclic (targetloopCnt=10) and keeps looping subsequent frames into
	// it while the CPU encodes (~1.7s), tearing the image (bench 12 Jul:
	// rainbow line-phase streaks). The per-capture re-arm re-enables it.
	hx_drv_xdma_set_wdma2_enable(0);

	// The frame was DMA-written: refresh the CPU's view. The delivered
	// lines are ~1255 B each; invalidate the whole tracked span.
	const uint32_t avail = HIRES_LINE_STRIDE_B * (HIRES_PROC_HEIGHT + 4u);
	SCB_InvalidateDCache_by_Addr((void *)raw, (int32_t)avail);

	// Lock each delivered line's true start (see HIRES_LINE_STRIDE_A/B)
	demosaic_track_lines(raw, HIRES_PROC_HEIGHT,
						 HIRES_LINE_STRIDE_A, HIRES_LINE_STRIDE_B, avail);

	// Repair the sensor's fixed-position zero runs (masked-PDAF dashes)
	demosaic_repair_zeros((uint8_t *)raw, HIRES_PROC_WIDTH, HIRES_PROC_HEIGHT);

	demosaic_pattern_t pattern = (demosaic_pattern_t)cisdp_get_demos_pattern();

	// Scene measurement straight off the Bayer plane: one AE step for the
	// next frame, and this frame's uniform WB gains (no tile seams by
	// construction - one gain pair for the whole image).
	uint32_t rM, gM, bM, gP75;
	demosaic_measure(raw, HIRES_PROC_WIDTH, HIRES_PROC_HEIGHT, pattern, &rM, &gM, &bM, &gP75);
	ae_process_measured(gP75);

	uint16_t rGain = 256, bGain = 256;
	uint8_t wbMode = (uint8_t)fatfs_getOperationalParameter(OP_PARAMETER_CAM_WB_MODE);
	if (wbMode == IMG_CORRECT_MODE_AUTO) {
		if (!img_correct_gains_from_means(rM, gM, bM, &rGain, &bGain)) {
			// too dark to trust - fall back to the manual gains
			wbMode = IMG_CORRECT_MODE_MANUAL;
		}
	}
	if (wbMode == IMG_CORRECT_MODE_MANUAL) {
		rGain = (uint16_t)fatfs_getOperationalParameter(OP_PARAMETER_WB_RED_GAIN);
		bGain = (uint16_t)fatfs_getOperationalParameter(OP_PARAMETER_WB_BLUE_GAIN);
		if (rGain == 0 || bGain == 0) {
			rGain = 256;
			bGain = 256;
		}
	}

	sw_jpeg_stream_t st;
	if (sw_jpeg_stream_begin(&st, HIRES_PROC_WIDTH, HIRES_PROC_HEIGHT,
							 jpegOut(), HIRES_JPEG_CAP, HIRES_JPEG_QUALITY) != 0) {
		return 0;
	}

	for (uint32_t row = 0; row < HIRES_PROC_HEIGHT; row += 16) {
		demosaic_strip_yuv420(raw, HIRES_PROC_WIDTH, HIRES_PROC_HEIGHT, row, pattern,
							  rGain, bGain, stripBuf());
		if (sw_jpeg_stream_strip(&st, stripBuf()) != 0) {
			xprintf("Hi-res: JPEG overflow at row %u (cap %u)\n",
					(unsigned)row, (unsigned)HIRES_JPEG_CAP);
			return 0;
		}
	}

	uint32_t size = sw_jpeg_stream_end(&st);
	if (size == 0) {
		return 0;
	}

	// The JPEG was CPU-written: flush so later cache-invalidate readers and
	// the FatFS/preview paths see the real bytes
	SCB_CleanDCache_by_Addr((void *)jpegOut(), (int32_t)size);

	// Hand the JPEG to the normal save/preview flow
	img_correct_set_external_jpeg(size, (uint32_t)jpegOut());

	xprintf("Hi-res: %ux%u JPEG %u bytes in %ums (WB R x%u/256 B x%u/256)\n",
			(unsigned)HIRES_PROC_WIDTH, (unsigned)HIRES_PROC_HEIGHT, (unsigned)size,
			(unsigned)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS),
			(unsigned)rGain, (unsigned)bGain);

	return size;
}
