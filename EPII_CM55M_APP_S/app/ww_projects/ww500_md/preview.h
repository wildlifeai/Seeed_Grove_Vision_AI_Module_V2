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

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>

/*********************************************** Global Defines **********************************************/


/*********************************************** Global Types ************************************************/

typedef enum {
	PREVIEW_OFF = 0,			// no streaming (normal operation)
	PREVIEW_STREAM = 1,			// stream frames; skip SD card saves
	PREVIEW_STREAM_AND_SAVE = 2	// stream frames; save to SD as normal
} PREVIEW_MODE_E;

/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

/**
 * @brief Set the preview mode. Takes effect from the next captured frame.
 *
 * @param mode PREVIEW_MODE_E to switch to.
 */
void preview_setMode(PREVIEW_MODE_E mode);

/**
 * @brief Get the current preview mode.
 *
 * @return Current PREVIEW_MODE_E.
 */
PREVIEW_MODE_E preview_getMode(void);

/**
 * @brief True when frames should be streamed (mode 1 or 2).
 *
 * @return true if preview streaming is active.
 */
bool preview_isActive(void);

/**
 * @brief True when preview wants the SD file save skipped (mode 1).
 *
 * @return true if the SD card save should be skipped.
 */
bool preview_skipsFileSave(void);

/**
 * @brief True while a frame line is being written to the console UART.
 *
 * A print from another task during this time lands inside the frame line and
 * corrupts it, so tasks with chatty console output (e.g. the IF task) check
 * this and stay quiet.
 *
 * @return true if preview_sendFrame() is writing a frame.
 */
bool preview_isSending(void);

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
void preview_setMdBits(const uint8_t *roiOut, uint8_t length);

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
void preview_sendFrame(void);

#endif /* PREVIEW_H_ */
