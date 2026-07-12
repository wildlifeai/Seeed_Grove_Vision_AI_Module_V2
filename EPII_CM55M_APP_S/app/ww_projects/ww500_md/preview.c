/*
 * preview.c
 *
 * Live image preview over the console UART. See preview.h and
 * _Documentation/live_preview.md
 *
 * Frame line format (one line per frame, CR ... LF):
 *
 *   {"type": 1, "name": "INVOKE", "code": 0, "data": {"count": N,
 *    "resolution": [W, H], "boxes": [], "md_blocks": M, "md": "<64 hex>",
 *    "image": "<base64 JPEG>"}}
 *
 * "md" is the HM0360 16x16 motion grid (32 MD_ROI_OUT bytes as hex, LSB-first
 * within each byte) and "md_blocks" the count of blocks flagged as moving, so a
 * viewer can overlay a motion heatmap on the live image. Both are omitted-safe:
 * older/other viewers ignore the extra fields.
 *
 * This is the same framing the Himax/SSCMA scenario apps use (see
 * app/scenario_app/tflm_fd_fm/send_result.cpp), so as well as
 * _Tools/live_view.py the Himax AI web toolkit can display the stream.
 *
 * The base64 is generated and written in small chunks so no frame-sized
 * buffer is needed. Writes go through xprintf() like all other console
 * output; a console print from another task can occasionally land inside
 * a frame line, which corrupts that frame only - the viewer drops it and
 * carries on.
 *
 * Bandwidth: 921600 baud is ~92 KB/s. A VGA JPEG is typically 25-50 KB,
 * +33% for base64, so expect roughly 1.5-2.5 fps from 'capture 1000 0'.
 */

#include <stdint.h>
#include <stdbool.h>

#include "WE2_core.h"		// SCB_InvalidateDCache_by_Addr
#include "xprintf.h"

#include "cisdp_sensor.h"	// cisdp_get_jpginfo(), app_get_raw_width/height()
#include "img_correct.h"	// img_correct_get_jpeg()
#include "preview.h"
#include "hm0360_md.h"		// hm0360_md_getMDOutput(), hm0360_md_prepare()
#include "hm0360_regs.h"	// ROIOUTENTRIES (the 32 MD_ROI_OUT registers)
#include "fatfs_task.h"		// fatfs_getOperationalParameter(OP_PARAMETER_MD_INTERVAL)

// A VGA JPEG is well under 200 KB and a 1280x960 hi-res JPEG (hires.c)
// under ~400 KB; anything bigger means the JPEG info is implausible and
// the frame is skipped rather than streamed.
#define PREVIEW_MAX_JPEG_BYTES	(430080)	// = hires.c JPEG cap (its overflow guard binds first)

// Base64 input chunk. Must be a multiple of 3 so '=' padding can only
// occur at the very end of the frame.
#define PREVIEW_CHUNK_IN	510

static volatile PREVIEW_MODE_E previewMode = PREVIEW_OFF;
static uint32_t frameCount = 0;

// While previewing, the HM0360 MD engine is armed to run concurrently with the
// main-camera stream (hm0360_md_prepare(true,...)), so each frame can carry a
// fresh motion grid for the viewer to overlay. Armed lazily on the first
// streamed frame - in preview_sendFrame(), which runs in the image-task context
// where the HM0360 I2C slave-swap inside getMDOutput()/prepare() is safe - and
// re-armed whenever preview is toggled off then on again.
static bool mdArmedForPreview = false;

// Fallback MD frame interval (ms) when OP_PARAMETER_MD_INTERVAL is 0 (i.e. field
// motion detection is disabled but we still want a live grid while previewing).
#define PREVIEW_MD_DEFAULT_INTERVAL_MS	100

static const char BASE64_CHARS[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void preview_setMode(PREVIEW_MODE_E mode) {
	previewMode = mode;
	if (mode == PREVIEW_OFF) {
		frameCount = 0;
		mdArmedForPreview = false;	// re-arm MD next time preview starts
	}
}

PREVIEW_MODE_E preview_getMode(void) {
	return previewMode;
}

bool preview_isActive(void) {
	return (previewMode != PREVIEW_OFF);
}

bool preview_skipsFileSave(void) {
	return (previewMode == PREVIEW_STREAM);
}

/**
 * Base64-encode data and write it to the console in chunks.
 */
static void sendBase64(const uint8_t *data, uint32_t length) {
	// 4 output chars per 3 input bytes, + NUL
	static char out[((PREVIEW_CHUNK_IN / 3) * 4) + 1];

	while (length > 0) {
		uint32_t n = (length > PREVIEW_CHUNK_IN) ? PREVIEW_CHUNK_IN : length;
		uint32_t i = 0;
		uint32_t o = 0;

		while ((i + 3) <= n) {
			uint32_t v = ((uint32_t)data[i] << 16)
					| ((uint32_t)data[i + 1] << 8)
					| (uint32_t)data[i + 2];
			out[o++] = BASE64_CHARS[(v >> 18) & 0x3f];
			out[o++] = BASE64_CHARS[(v >> 12) & 0x3f];
			out[o++] = BASE64_CHARS[(v >> 6) & 0x3f];
			out[o++] = BASE64_CHARS[v & 0x3f];
			i += 3;
		}

		// 1 or 2 leftover bytes - only possible on the final chunk since
		// PREVIEW_CHUNK_IN is a multiple of 3
		if (i < n) {
			uint32_t v = (uint32_t)data[i] << 16;
			if ((i + 1) < n) {
				v |= (uint32_t)data[i + 1] << 8;
			}
			out[o++] = BASE64_CHARS[(v >> 18) & 0x3f];
			out[o++] = BASE64_CHARS[(v >> 12) & 0x3f];
			out[o++] = ((i + 1) < n) ? BASE64_CHARS[(v >> 6) & 0x3f] : '=';
			out[o++] = '=';
		}

		out[o] = '\0';
		xprintf("%s", out);

		data += n;
		length -= n;
	}
}

void preview_sendFrame(void) {
	uint32_t jpegLength = 0;
	uint32_t jpegBuffer = 0;

	if (previewMode == PREVIEW_OFF) {
		return;
	}

	// Same sequence as prepareJpegFile(): hardware encoder JPEG, replaced
	// by the WB-corrected sw_jpeg output if the correction ran
	cisdp_get_jpginfo(&jpegLength, &jpegBuffer);
	img_correct_get_jpeg(&jpegLength, &jpegBuffer);

	if ((jpegBuffer == 0) || (jpegLength < 4) || (jpegLength > PREVIEW_MAX_JPEG_BYTES)) {
		xprintf("Preview: implausible JPEG (addr 0x%08x length %d) - frame skipped\n",
				jpegBuffer, jpegLength);
		return;
	}

	SCB_InvalidateDCache_by_Addr((void *)jpegBuffer, jpegLength);

	frameCount++;

	// Arm the HM0360 MD engine to run alongside the preview stream (once per
	// preview session). cameraSystemEnabled=true keeps MD running while the SoC
	// is awake - the normal firmware only arms MD on the sleep path, which is
	// why the grid reads empty during an ordinary awake capture.
	if (!mdArmedForPreview) {
		uint16_t mdInterval = fatfs_getOperationalParameter(OP_PARAMETER_MD_INTERVAL);
		if (mdInterval == 0) {
			mdInterval = PREVIEW_MD_DEFAULT_INTERVAL_MS;
		}
		hm0360_md_prepare(true, mdInterval);
		mdArmedForPreview = true;
	}

	// Read the 16x16 motion grid (32 MD_ROI_OUT bytes) and render it as hex.
	// getMDOutput() does its own main-camera I2C slave-swap, so it is safe to
	// call here after the frame's own capture has completed.
	uint8_t roiOut[ROIOUTENTRIES];
	uint16_t mdBlocks = hm0360_md_getMDOutput(roiOut, ROIOUTENTRIES);
	static const char HEXDIGIT[] = "0123456789abcdef";
	char mdHex[(ROIOUTENTRIES * 2) + 1];
	for (uint8_t i = 0; i < ROIOUTENTRIES; i++) {
		mdHex[(i * 2)]     = HEXDIGIT[(roiOut[i] >> 4) & 0x0f];
		mdHex[(i * 2) + 1] = HEXDIGIT[roiOut[i] & 0x0f];
	}
	mdHex[ROIOUTENTRIES * 2] = '\0';

	// Frame line gains "md_blocks" (moving-block count) and "md" (64-hex-char
	// 16x16 motion bitmap) beside the JPEG. Existing viewers (live_view.py, the
	// Himax web toolkit) ignore the extra fields, so this stays compatible.
	xprintf("\r{\"type\": 1, \"name\": \"INVOKE\", \"code\": 0, \"data\": {\"count\": %d, "
			"\"resolution\": [%d, %d], \"boxes\": [], \"md_blocks\": %d, \"md\": \"%s\", "
			"\"image\": \"",
			(int)frameCount,
			(int)app_get_raw_width(), (int)app_get_raw_height(),
			(int)mdBlocks, mdHex);

	sendBase64((const uint8_t *)jpegBuffer, jpegLength);

	xprintf("\"}}\n");
}
