/*
 * preview.c
 *
 * Live image preview over the console UART. See preview.h and
 * _Documentation/live_preview.md
 *
 * Frame line format (one line per frame, CR ... LF):
 *
 *   {"type": 1, "name": "INVOKE", "code": 0, "data": {"count": N,
 *    "resolution": [W, H], "boxes": [], "md": "<64 hex>",
 *    "mdstat": {"int": N, "ctrl": N, "ctrl1": N, "thA": N, "thB": N},
 *    "image": "<base64 JPEG>"}}
 *
 * "md" (the HM0360's 32 ROI_OUT motion bytes for this capture) is present
 * only when the image task supplied them - see preview_setMdBits(). "mdstat"
 * (INT_INDIC, MD_CTRL, MD_CTRL1 and the context A and B MD_BLOCK_NUM_TH
 * registers, as decimal numbers) likewise - see preview_setMdStatus().
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

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "WE2_core.h"		// SCB_InvalidateDCache_by_Addr
#include "xprintf.h"

#include "cisdp_sensor.h"	// cisdp_get_jpginfo(), app_get_raw_width/height()
#include "img_correct.h"	// img_correct_get_jpeg()
#include "preview.h"

/*********************************************** Local Defines **********************************************/

// A VGA JPEG should be well under this; anything bigger means the JPEG
// info is implausible and the frame is skipped rather than streamed.
#define PREVIEW_MAX_JPEG_BYTES	(200 * 1024)

// Base64 input chunk. Must be a multiple of 3 so '=' padding can only
// occur at the very end of the frame.
#define PREVIEW_CHUNK_IN	510

// HM0360 motion detection output: 32 ROI_OUT registers, one bit per cell
#define PREVIEW_MD_BYTES	32

/*********************************************** Local Types ************************************************/


/*********************************************** Local Variables ********************************************/

static volatile PREVIEW_MODE_E previewMode = PREVIEW_OFF;
static uint32_t frameCount = 0;

// True while preview_sendFrame() is writing a frame line. Other tasks check it
// (via preview_isSending()) to hold back console prints that would split the line.
static volatile bool frameSending = false;

// HM0360 MD bits read for the frame about to be sent (preview_setMdBits()).
// Sent once as the frame's "md" field, then invalidated.
static uint8_t mdBits[PREVIEW_MD_BYTES];
static bool mdValid = false;

// HM0360 MD interrupt status for the frame about to be sent (preview_setMdStatus()).
// Sent once as the frame's "mdstat" field, then invalidated.
static HM0360_MD_STATUS_T mdStatus;
static bool mdStatusValid = false;

static const char BASE64_CHARS[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*********************************************** Local Function Declarations *********************************/

static void sendBase64(const uint8_t *data, uint32_t length);

/*********************************************** Local Function Definitions *********************************/

/**
 * @brief Base64-encode data and write it to the console in chunks.
 *
 * @param data   bytes to encode.
 * @param length number of bytes in data.
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

/*********************************************** Global Function Definitions *********************************/

/**
 * @brief Set the preview mode. Takes effect from the next captured frame.
 *
 * @param mode PREVIEW_MODE_E to switch to.
 */
void preview_setMode(PREVIEW_MODE_E mode) {
	previewMode = mode;
	if (mode == PREVIEW_OFF) {
		frameCount = 0;
		mdValid = false;
		mdStatusValid = false;
	}
}

/**
 * @brief Get the current preview mode.
 *
 * @return Current PREVIEW_MODE_E.
 */
PREVIEW_MODE_E preview_getMode(void) {
	return previewMode;
}

/**
 * @brief True when frames should be streamed (mode 1 or 2).
 *
 * @return true if preview streaming is active.
 */
bool preview_isActive(void) {
	return (previewMode != PREVIEW_OFF);
}

/**
 * @brief True when preview wants the SD file save skipped (mode 1).
 *
 * @return true if the SD card save should be skipped.
 */
bool preview_skipsFileSave(void) {
	return (previewMode == PREVIEW_STREAM);
}

/**
 * @brief True while a frame line is being written to the console UART.
 *
 * A print from another task during this time lands inside the frame line and
 * corrupts it, so tasks with chatty console output (e.g. the IF task) check
 * this and stay quiet.
 *
 * @return true if preview_sendFrame() is writing a frame.
 */
bool preview_isSending(void) {
	return frameSending;
}

/**
 * @brief Record the HM0360 motion detection bits for the next frame.
 *
 * The image task reads the 32 ROI_OUT registers once per capture; passing
 * them here adds them to that frame's line as "md" (64 hex characters), so
 * the viewer can draw them over the image. Ignored unless preview is on.
 *
 * @param roiOut the ROI_OUT register values, ROI_OUT_0 first.
 * @param length number of bytes in roiOut (must be 32).
 */
void preview_setMdBits(const uint8_t *roiOut, uint8_t length) {
	if ((previewMode == PREVIEW_OFF) || (roiOut == NULL) || (length != PREVIEW_MD_BYTES)) {
		return;
	}

	for (uint8_t i = 0; i < PREVIEW_MD_BYTES; i++) {
		mdBits[i] = roiOut[i];
	}
	mdValid = true;
}

/**
 * @brief Record the HM0360 MD interrupt status for the next frame.
 *
 * Read at the start of the frame handling, before the image task clears the
 * HM0360 interrupt bits, and sent with that frame's line as "mdstat" so the
 * viewer can compare the MD interrupt with the motion cell count. Ignored
 * unless preview is on.
 *
 * @param status the registers read by hm0360_md_getMDStatus().
 */
void preview_setMdStatus(const HM0360_MD_STATUS_T *status) {
	if ((previewMode == PREVIEW_OFF) || (status == NULL)) {
		return;
	}

	mdStatus = *status;
	mdStatusValid = true;
}

/**
 * @brief Emit the JPEG of the capture just completed as one JSON line on the
 * console UART.
 *
 * Call from the image task once the frame is ready - after
 * img_correct_process() has run, so the streamed image is the same one
 * prepareJpegFile() would save (WB-corrected sw_jpeg output when the
 * correction ran, otherwise the hardware encoder output).
 *
 * Blocks the calling task for the duration of the UART write
 * (a VGA JPEG takes roughly 300-600 ms at 921600 baud).
 */
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
	frameSending = true;

	xprintf("\r{\"type\": 1, \"name\": \"INVOKE\", \"code\": 0, \"data\": {\"count\": %d, "
			"\"resolution\": [%d, %d], \"boxes\": [], ",
			(int)frameCount,
			(int)app_get_raw_width(), (int)app_get_raw_height());

	if (mdValid) {
		xprintf("\"md\": \"");
		for (uint8_t i = 0; i < PREVIEW_MD_BYTES; i++) {
			xprintf("%02x", mdBits[i]);
		}
		xprintf("\", ");
		mdValid = false;	// these bits belong to this frame only
	}

	if (mdStatusValid) {
		xprintf("\"mdstat\": {\"int\": %d, \"ctrl\": %d, \"ctrl1\": %d, \"thA\": %d, \"thB\": %d}, ",
				mdStatus.intIndic, mdStatus.mdCtrl, mdStatus.mdCtrl1,
				mdStatus.blockNumThA, mdStatus.blockNumThB);
		mdStatusValid = false;	// this status belongs to this frame only
	}

	xprintf("\"image\": \"");

	sendBase64((const uint8_t *)jpegBuffer, jpegLength);

	xprintf("\"}}\n");

	frameSending = false;
}
