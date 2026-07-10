/*
 * preview.h
 *
 * Live image preview over the console UART, for bench tuning of camera
 * settings (white balance, exposure, focus, flash) while watching the
 * effect on the picture. See _Documentation/live_preview.md
 *
 * Enable with the 'preview' CLI command, then start frames flowing with
 * e.g. 'capture 1000 0'. Every captured frame is emitted on the console
 * UART as one JSON line with the JPEG as base64. View with
 * _Tools/live_view.py (recommended - keeps the CLI usable for
 * camreg/vcm/setop while watching) or the Himax AI web toolkit.
 */

#ifndef PREVIEW_H_
#define PREVIEW_H_

#include <stdint.h>
#include <stdbool.h>

typedef enum {
	PREVIEW_OFF = 0,			// no streaming (normal operation)
	PREVIEW_STREAM = 1,			// stream frames; skip SD card saves
	PREVIEW_STREAM_AND_SAVE = 2	// stream frames; save to SD as normal
} PREVIEW_MODE_E;

/**
 * Set the preview mode. Takes effect from the next captured frame.
 */
void preview_setMode(PREVIEW_MODE_E mode);

PREVIEW_MODE_E preview_getMode(void);

/**
 * True when frames should be streamed (mode 1 or 2).
 */
bool preview_isActive(void);

/**
 * True when preview wants the SD file save skipped (mode 1).
 */
bool preview_skipsFileSave(void);

/**
 * Emit the JPEG of the capture just completed as one JSON line on the
 * console UART. Call from the image task once the frame is ready - after
 * img_correct_process() has run, so the streamed image is the same one
 * prepareJpegFile() would save (WB-corrected sw_jpeg output when the
 * correction ran, otherwise the hardware encoder output).
 *
 * Blocks the calling task for the duration of the UART write
 * (a VGA JPEG takes roughly 300-600 ms at 921600 baud).
 */
void preview_sendFrame(void);

#endif /* PREVIEW_H_ */
