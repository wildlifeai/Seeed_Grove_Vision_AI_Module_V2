/*
 * exif_photo.c
 *
 *  Created on: 27 Nov 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  What the EXIF of a WW500 photo says: camera model, software, photo time, flash, NN results and labels,
 *  capture settings (MakerNote), deployment ID. Fills an ExifInput_t and has exif_builder.c lay it out.
 *
 *  Moved out of image_task.c's prepareJpegFile() on 27 Nov 2026, unchanged apart from the date rule
 *  (#295): the date tags are left out when the photo time is before EXIF_PHOTO_FIRST_VALID_YEAR, whatever it
 *  came from (the RTC, 2024 after a reset, or a setutc from a BLE processor whose clock is wrong).
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>
#include <string.h>

#include "xprintf.h"
#include "exif_photo.h"
#include "exif_builder.h"
#include "exif_utc.h"
#include "fatfs_task.h"		// op parameters, deployment ID
#include "ww500_md.h"		// board name and version strings
#include "cvapp.h"			// NN labels and confidence (USE_PERCENTAGE)

/*********************************************** Local Defines **********************************************/

// Per-class confidence/label text in the UserComment. Follows USE_PERCENTAGE in cvapp.h
#ifdef USE_PERCENTAGE
#define ENABLE_EXIF_CONFIDENCE
#endif // USE_PERCENTAGE

// Add the MakerNote: the capture settings (AE gain values, white balance, flash)
#define EXIF_MAKER_NOTES

// Length of the MakerNote string
#define MAKERDATALEN 64

/*********************************************** External Variables *****************************************/


/*********************************************** Local Variables ********************************************/

// Static, not on the image task's stack: their addresses go into the EXIF input
static char softwareString[64];					// EXIF Software: board name and firmware version
static uint8_t nnData[MAX_CLASSES + 2];			// [total_bytes][count][score...]
static char userComment[EXIF_COMMENT_LENGTH];	// NN label summary
#ifdef ENABLE_EXIF_CONFIDENCE
static ClassConfidenceData confidenceData;
#endif // ENABLE_EXIF_CONFIDENCE
#ifdef EXIF_MAKER_NOTES
static char makerNote[MAKERDATALEN];
#endif // EXIF_MAKER_NOTES

/*********************************************** Local Function Declarations *********************************/

static const char *userCommentFromNN(const int8_t *outCategories, uint8_t classCount);

/*********************************************** Local Function Definitions *********************************/

/**
 * @brief Builds the UserComment: the NN labels and their scores.
 *
 * With ENABLE_EXIF_CONFIDENCE, from cvapp's confidence data ("label: N%; ..."); otherwise from the output
 * tensor and the model's labels ("label: N; ...").
 *
 * @param outCategories  the NN output, one value per class
 * @param classCount     the number of classes
 * @return the comment, or NULL if there is none
 */
static const char *userCommentFromNN(const int8_t *outCategories, uint8_t classCount) {
#ifdef ENABLE_EXIF_CONFIDENCE
	bool hasConfidenceData;

	(void)outCategories;
	(void)classCount;

	memset(&confidenceData, 0, sizeof(confidenceData));
	memset(userComment, 0, sizeof(userComment));
	cv_get_confidence_data(&confidenceData);
	hasConfidenceData = ((confidenceData.class_count > 0) && (confidenceData.class_count <= MAX_CLASSES));
	if (hasConfidenceData) {
		char *ucPtr = userComment;
		int ucRem = sizeof(userComment) - 1;

		for (uint8_t i = 0; (i < confidenceData.class_count) && (i < EXIF_MAX_DYNAMIC_CLASSES); i++) {
			const char *label = confidenceData.labels[i] ? confidenceData.labels[i] : "Unknown";
			int written = snprintf(ucPtr, ucRem, "%s: %d%%; ", label, confidenceData.confidence_percent[i]);

			if ((written > 0) && (written < ucRem)) {
				ucPtr += written;
				ucRem -= written;
			}
			else {
				break;
			}
		}
	}
	return (hasConfidenceData && (userComment[0] != '\0')) ? userComment : NULL;
#else
	userComment[0] = '\0';
	if (cv_modelLoaded()) {
		size_t ucOffset = 0;

		for (uint8_t i = 0; i < classCount; i++) {
			int written = snprintf(userComment + ucOffset, EXIF_COMMENT_LENGTH - ucOffset,
					"%s: %d; ", cv_getLabel(i), outCategories[i]);
			if (written < 0) {
				break;
			}
			if ((size_t)written >= EXIF_COMMENT_LENGTH - ucOffset) {
				break;
			}
			ucOffset += (size_t)written;
		}
	}
	return (userComment[0] != '\0') ? userComment : NULL;
#endif // ENABLE_EXIF_CONFIDENCE
}

/*********************************************** Global Function Definitions *********************************/

/**
 * @brief Builds the EXIF for one photo into exif_builder.c's buffer.
 *
 * The date tags (DateTimeOriginal, CreateDate) are left out when the photo time is before
 * EXIF_PHOTO_FIRST_VALID_YEAR, with a console line (#295). Everything else is as before the move from
 * image_task.c.
 *
 * @param width          image width in pixels
 * @param height         image height in pixels
 * @param photoTime      the photo's time, as used for its file name
 * @param flashFired     0 if no LED lit this photo, else the LED (EXIF Flash)
 * @param outCategories  the NN output, one value per class
 * @param classCount     the number of classes
 * @param gain           the HM0360 AE registers for the MakerNote, or NULL for no MakerNote
 * @return the EXIF length (exif_get_buffer() holds it), 0 on failure
 */
uint32_t exif_photo_build(uint16_t width, uint16_t height, const rtc_time *photoTime, uint8_t flashFired,
		const int8_t *outCategories, uint8_t classCount, const HM0360_GAIN_T *gain) {
	ExifInput_t exifInput;
	char deploymentId[UUIDLENGTH];
	rtc_time timeCopy = *photoTime;	// exif_utc_time_to_exif_string() takes a non-const pointer

	memset(&exifInput, 0, sizeof(exifInput));
	exifInput.width = width;
	exifInput.height = height;

	// The date tags, only if the time is plausible (#295)
	exif_utc_time_to_exif_string(&timeCopy, exifInput.timestamp, sizeof(exifInput.timestamp));
	exifInput.timestamp_known = (photoTime->tm_year >= EXIF_PHOTO_FIRST_VALID_YEAR);
	if (!exifInput.timestamp_known) {
		xprintf("Photo time %s is before %d: no EXIF date\n", exifInput.timestamp, EXIF_PHOTO_FIRST_VALID_YEAR);
	}

	/* Camera identity, build provenance and flash state. The Model string names
	 * the camera variant so a photo is self-describing (colour RP3 vs mono/IR
	 * HM0360) - the website maps this to its camera_variant. Software matches
	 * the firmware version recorded in the firmware database. */
#if defined(USE_RP3)
	// Test the RP cameras first: their builds define USE_HM0360_MD as well, for motion detection. Testing that first
	// labelled every RP3 photo "WW500 HM0360" (issue #153, fixed 2 Oct 2026)
	exifInput.camera_model = "WW500 RP3";
#elif defined(USE_RP2)
	exifInput.camera_model = "WW500 RP2";
#elif defined(USE_HM0360)
	exifInput.camera_model = "WW500 HM0360";
#else
	exifInput.camera_model = NULL;	/* exif_builder falls back to "WW500" */
#endif

	snprintf(softwareString, sizeof(softwareString), "%s %s", app_get_board_name_string(), app_get_version_string());
	exifInput.software = softwareString;

	// Which LED lit this photo: the caller passes the state at the capture, not a live ledFlashIsActive()
	// (by now the post-capture light check may have set it for the next capture)
	exifInput.flash_fired = flashFired;

	/* NN data: [total_bytes][count][score...] */
	if (classCount > MAX_CLASSES) {
		nnData[0] = 1;
		nnData[1] = 0;
	}
	else {
		nnData[0] = classCount + 1u;
		nnData[1] = classCount;
		for (uint8_t i = 0; i < classCount; i++) {
			nnData[i + 2] = (uint8_t)outCategories[i];
		}
	}
	exifInput.nn_data = nnData;

	/* UserComment: NN label summary */
	exifInput.user_comment = userCommentFromNN(outCategories, classCount);

	/* MakerNote: capture-settings CSV (formatted here; exif_builder.c has no HM0360 types).
	 * Fields: integration, analogGain, digitalGain, aeMean, aeConverged,
	 *         wbRedGain, wbBlueGain, flashFired
	 * The last three are appended (backward-compatible): the WB gains (Q8.8,
	 * 0 = correction off) that produced this image, and whether the flash/IR
	 * illumination fired - so field A/B tuning can be traced from the photo. */
#ifdef EXIF_MAKER_NOTES
#if defined(USE_RP2) || defined(USE_RP3)
	uint16_t wbRedNote  = (uint16_t)fatfs_getOperationalParameter(OP_PARAMETER_WB_RED_GAIN);
	uint16_t wbBlueNote = (uint16_t)fatfs_getOperationalParameter(OP_PARAMETER_WB_BLUE_GAIN);
#else
	uint16_t wbRedNote  = 0u;	/* no software WB on the mono/IR camera */
	uint16_t wbBlueNote = 0u;
#endif

	if (gain != NULL) {
		snprintf(makerNote, sizeof(makerNote), "%d, %d, %d, %d, %c, %u, %u, %u",
		         gain->integration, gain->analogGain, gain->digitalGain, gain->aeMean,
		         (gain->aeConverged == 1) ? 'Y' : 'N',
		         wbRedNote, wbBlueNote, (unsigned)exifInput.flash_fired);
		exifInput.maker_note = makerNote;
	}
	else {
		exifInput.maker_note = NULL;
	}
#else
	(void)gain;
	exifInput.maker_note = NULL;
#endif // EXIF_MAKER_NOTES

	/* Deployment ID: none if it is the zero UUID */
	fatfs_getDeploymentId(deploymentId, sizeof(deploymentId));
	exifInput.deployment_id = (strcmp(deploymentId, DEPLOYMENT_ID_ZERO_UUID) != 0) ? deploymentId : NULL;

	/* Sector-alignment COM padding is applied inside exif_build_segment(). */
	return exif_build_segment(&exifInput);
}
