/*
 * preview.c
 *
 * Live image preview over the console UART. See preview.h and
 * _Documentation/live_preview.md
 *
 * Frame line format (one line per frame, CR ... LF):
 *
 *   {"type": 1, "name": "INVOKE", "code": 0, "data": {"count": N,
 *    "resolution": [W, H], "boxes": [], "image": "<base64 JPEG>"}}
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

// A VGA JPEG should be well under this; anything bigger means the JPEG
// info is implausible and the frame is skipped rather than streamed.
#define PREVIEW_MAX_JPEG_BYTES	(200 * 1024)

// Base64 input chunk. Must be a multiple of 3 so '=' padding can only
// occur at the very end of the frame.
#define PREVIEW_CHUNK_IN	510

static volatile PREVIEW_MODE_E previewMode = PREVIEW_OFF;
static uint32_t frameCount = 0;

static const char BASE64_CHARS[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void preview_setMode(PREVIEW_MODE_E mode) {
	previewMode = mode;
	if (mode == PREVIEW_OFF) {
		frameCount = 0;
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

	xprintf("\r{\"type\": 1, \"name\": \"INVOKE\", \"code\": 0, \"data\": {\"count\": %d, "
			"\"resolution\": [%d, %d], \"boxes\": [], \"image\": \"",
			(int)frameCount,
			(int)app_get_raw_width(), (int)app_get_raw_height());

	sendBase64((const uint8_t *)jpegBuffer, jpegLength);

	xprintf("\"}}\n");
}
