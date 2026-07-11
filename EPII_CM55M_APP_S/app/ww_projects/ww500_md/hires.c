/*
 * hires.c
 *
 * Single-file 1280x960 capture. See hires.h for the architecture and
 * _Documentation/hires-capture.md for the design rationale and bench data.
 */

#include <stdint.h>
#include <stdbool.h>

#include "WE2_core.h"		// SCB_InvalidateDCache_by_Addr
#include "xprintf.h"

#include "FreeRTOS.h"
#include "task.h"

#include "fatfs_task.h"
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
// RAW path never touches (no WDMA3 write). Strip: Y 1280*16 + Cb/Cr 640*8*2.
extern uint8_t demosbuf[];
#define HIRES_STRIP_BYTES	((HIRES_WIDTH * 16u) + 2u * ((HIRES_WIDTH / 2u) * 8u))
#define HIRES_JPEG_CAP		(460800u - HIRES_STRIP_BYTES)

static bool active = false;

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

int hires_datapath_init(void *cb_event) {
	uint32_t rawStart = (uint32_t)rawBuf();
	uint32_t rawEnd = rawStart + (HIRES_WIDTH * HIRES_HEIGHT);

	if (rawEnd > SRAM_REGION_END) {
		xprintf("Hi-res: raw buffer 0x%08x..0x%08x exceeds SRAM end - disabled\n",
				(unsigned)rawStart, (unsigned)rawEnd);
		return -1;
	}

	cisdp_set_hires_raw(rawStart);

	// RAW path: INP centre-crop -> WDMA2. Same init call as the normal flow;
	// the hires override (registered above) redirects geometry and target.
	if (cisdp_dp_init(true, SENSORDPLIB_PATH_INP_WDMA2,
					  (sensordplib_CBEvent_t)cb_event, 4,
					  APP_DP_RES_YUV640x480_INP_SUBSAMPLE_1X) < 0) {
		xprintf("Hi-res: RAW datapath init failed\n");
		cisdp_set_hires_raw(0);
		return -1;
	}

	active = true;
	xprintf("Hi-res capture active: %ux%u RAW -> CPU pipeline (raw @0x%08x, "
			"jpeg cap %u)\n", (unsigned)HIRES_WIDTH, (unsigned)HIRES_HEIGHT,
			(unsigned)rawStart, (unsigned)HIRES_JPEG_CAP);
	return 0;
}

void hires_deactivate(void) {
	active = false;
	cisdp_set_hires_raw(0);
}

uint32_t hires_process_frame(void) {
	if (!active) {
		return 0;
	}

	const uint8_t *raw = rawBuf();
	uint32_t start = xTaskGetTickCount();

	// The frame was DMA-written: refresh the CPU's view
	SCB_InvalidateDCache_by_Addr((void *)raw, (int32_t)(HIRES_WIDTH * HIRES_HEIGHT));

	demosaic_pattern_t pattern = (demosaic_pattern_t)cisdp_get_demos_pattern();

	// Scene measurement straight off the Bayer plane: one AE step for the
	// next frame, and this frame's uniform WB gains (no tile seams by
	// construction - one gain pair for the whole image).
	uint32_t rM, gM, bM, gP75;
	demosaic_measure(raw, HIRES_WIDTH, HIRES_HEIGHT, pattern, &rM, &gM, &bM, &gP75);
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
	if (sw_jpeg_stream_begin(&st, HIRES_WIDTH, HIRES_HEIGHT,
							 jpegOut(), HIRES_JPEG_CAP, HIRES_JPEG_QUALITY) != 0) {
		return 0;
	}

	for (uint32_t row = 0; row < HIRES_HEIGHT; row += 16) {
		demosaic_strip_yuv420(raw, HIRES_WIDTH, HIRES_HEIGHT, row, pattern,
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
			(unsigned)HIRES_WIDTH, (unsigned)HIRES_HEIGHT, (unsigned)size,
			(unsigned)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS),
			(unsigned)rGain, (unsigned)bGain);

	return size;
}
