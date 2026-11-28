/*
 * exif_photo.h
 *
 *  Created on: 27 Nov 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  What the EXIF of a WW500 photo says: camera model, software, photo time, flash, NN results and labels,
 *  capture settings (MakerNote), deployment ID. Fills an ExifInput_t and has exif_builder.c lay it out.
 *  Moved out of image_task.c's prepareJpegFile() (27 Nov 2026), which keeps the JPEG buffer, file name and
 *  file operation. exif_builder.c stays generic: it knows no camera or NN types.
 *
 *  The date tags are left out when the photo time is before EXIF_PHOTO_FIRST_VALID_YEAR (#295).
 */

#ifndef EXIF_PHOTO_H_
#define EXIF_PHOTO_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include "exif_utc.h"		// rtc_time
#include "hm0360_md.h"		// HM0360_GAIN_T

/*********************************************** Global Defines **********************************************/

// A photo time before this year is not plausible: no camera takes photos then, so the time came from an RTC
// restarted at 2024 after a reset, or from a wrong BLE processor clock. Its date tags are left out (#295)
#define EXIF_PHOTO_FIRST_VALID_YEAR		2026

/*********************************************** Global Types ************************************************/


/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

// Builds the EXIF for one photo into exif_builder.c's buffer (exif_get_buffer()). Returns its length, 0 on failure
uint32_t exif_photo_build(uint16_t width, uint16_t height, const rtc_time *photoTime, uint8_t flashFired,
		const int8_t *outCategories, uint8_t classCount, const HM0360_GAIN_T *gain);

#endif /* EXIF_PHOTO_H_ */
