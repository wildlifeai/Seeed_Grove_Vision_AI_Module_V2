/**
 * @file directory_manager.c
 *
 *  Created: 31 Jul 2025
 *      Author: TBP
 *
 * This file manages directories for the WW500 MD project.
 * It initializes directories, creates capture folders,
 * and handles directory-related operations.
 * It primiliarly communicates with the FatFS task.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "xprintf.h"
#include "fatfs_task.h"
#include "directory_manager.h"
#include "image_task.h"
#include "ffconf.h"
#include "fatfs_task.h"

#include "exif_utc.h"
#include "printf_x.h"

/*************************************** Definitions *******************************************/

#define MAXIMAGEDIRECTORIES 999

// low number just for testing
#define MAXIMAGESPERDIRECTORY 4
//#define MAXIMAGESPERDIRECTORY 100

// Note: CONFIG_DIR comes from directory_manager.h and is currently "/MANIFEST"
// TODO no need for both...
#define MANIFEST_DIR CONFIG_DIR

#ifdef UNZIPMANIFEST
// Locations on SD card
#define MANIFEST_ZIP_CANON "/MANIFEST.ZIP"
#endif // UNZIPMANIFEST

/*************************************** Local variables *******************************************/

// const char *folder_defaults[DIR_COUNT] = {
//     "config/",
//     "images/2025/08/06/",
//     "tflite_model/",
//     "logs/",
//     "models/"
// };

/*************************************** Global variables *******************************************/

directoryManager_t dirManager; // Added definition for dirManager

/********************************** Private Function Declarations *****************************/

/********************************** Private Function Definitions  *************************************/

#ifdef UNZIPMANIFEST

static const char *find_manifest_zip_path(void)
{
	// Be tolerant to case variants on the SD card, but treat /MANIFEST as canonical.
	// These are checked in priority order, preferring 8.3 uppercase.
	static const char *candidates[] = {
		MANIFEST_ZIP_CANON,
		"/MANIFEST.zip",
		"/Manifest.zip",
		"/manifest.zip",
		"/Manifest.ZIP",
		"/manifest.ZIP",
	};
	FILINFO fno;
	for (unsigned i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); i++)
	{
		if (f_stat(candidates[i], &fno) == FR_OK)
		{
			return candidates[i];
		}
	}
	return NULL;
}

static FRESULT ensure_directory_exists(const char *path)
{
	FILINFO fno;
	FRESULT res = f_stat(path, &fno);
	if (res == FR_OK)
	{
		return FR_OK;
	}

	xprintf("Creating directory '%s'\r\n", path);
	res = f_mkdir(path);
	if (res != FR_OK)
	{
		xprintf("f_mkdir('%s') failed (%d)\r\n", path, res);
	}
	return res;
}

static FRESULT ensure_default_config_file_exists(directoryManager_t *dirManager)
{
	// If config file exists already, do nothing.
	FILINFO fno;
	FRESULT res = f_stat(STATE_FILE, &fno);
	if (res == FR_OK)
	{
		xprintf("Config file '%s' exists\r\n", STATE_FILE);
		return FR_OK;
	}

	// Create a minimal default config file.
	// NOTE: The FatFS task will later call load_configuration(); if this file exists,
	// it will load any parameters it recognizes.
	FIL f;
	UINT bw = 0;
	res = f_open(&f, STATE_FILE, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK)
	{
		xprintf("Failed to create config file '%s' (%d)\r\n", STATE_FILE, res);
		return res;
	}

	// 	const char *header = "# Auto-created default config. See the wildlife-watcher-model-conversion streamlit app\n";
	// (void)f_write(&f, header, strlen(header), &bw);
	const char *header = "# Auto-created default configuration\n"
						 "# To download the latest manifest folder, visit: https://wildlifewatcher.streamlit.app/\n"
						 "#\n";
	res = f_write(&f, header, strlen(header), &bw);
	if (res != FR_OK)
	{
		xprintf("Failed to write header to config file '%s' (%d)\r\n", STATE_FILE, res);
		f_close(&f);
		return res;
	}

	res = f_close(&f);
	if (res != FR_OK)
	{
		xprintf("Failed to close config file '%s' (%d)\r\n", STATE_FILE, res);
		return res;
	}

	// TBP: I've removed save_configuration() func call from here.
	// It was generating a UsageFault_Handler error on an empty SD card emulation.
	// Described below as:
	// On an empty SD boot, directory manager initialization can still be in progress
	// and save_configuration() changes directories using dirManager fields (base_dir,
	// current_config_dir). If any of these are not valid yet, it can trigger faults.
	// The FATFS task will call load_configuration() shortly after init; if it wants
	// to normalize/write defaults, it should do so after init is complete.

	// Keep directory manager state consistent
	dirManager->configOpen = false;
	dirManager->configRes = FR_OK;
	xprintf("Created default config file '%s'\r\n", STATE_FILE);
	return FR_OK;
}
#endif // UNZIPMANIFEST

///*************************************** Global Function Definitions *****************************/

/**
 * Looks for a directory to save images.
 * If it exists, use it. Otherwise, create it.
 *
 * TBP - I've set FF_FS_LOCK to 2. This means that the directory
 * is locked when it is opened, and unlocked when it is closed.
 * This is to prevent other tasks from writing to the directory
 * while it is being used. 2 also means that it enables two directories
 * to be used simultaneously.
 *
 * TODO - this should probably be the responsibility of the image task?
 * Or else: move generateImageFileName() to the fatfs task as well.
 */
FRESULT dir_mgr_init_directories(directoryManager_t *dirManager) {
	FRESULT res;
	char path_buf[IMAGEFILENAMELEN];
	FILINFO fno;

	// Make sure all fields start from a known state.
	// (This is especially important for path buffers used by f_chdir()).
	memset(dirManager, 0, sizeof(*dirManager));

	int len = sizeof(dirManager->base_dir);
	res = f_getcwd(dirManager->base_dir, len); /* Get current directory */
	if (res != FR_OK) {
		// Use root as base directory.
		strcpy(dirManager->base_dir, "/");
	}
	dirManager->imagesDirIdx = 0;

#ifdef UNZIPMANIFEST

	// === MANIFEST / CONFIG DIRECTORY + UNZIP LOGIC ===
	// Required behavior:
	// 1) manifest.zip exists but /MANIFEST doesn't => unzip manifest.zip
	// 2) /MANIFEST exists => do nothing
	// 3) neither manifest.zip nor /MANIFEST exists => create config directory and config file

	bool manifest_dir_exists = (f_stat(MANIFEST_DIR, &fno) == FR_OK);
	const char *manifest_zip_path = find_manifest_zip_path();
	bool manifest_zip_exists = (manifest_zip_path != NULL);
	xprintf("MANIFEST_DIR: %s\r\n", MANIFEST_DIR);
	xprintf("MANIFEST_ZIP (selected): %s\r\n", manifest_zip_path ? manifest_zip_path : "<none>");

	if (manifest_dir_exists) {
		xprintf("Directory '%s' exists\r\n", MANIFEST_DIR);
	}
	else if (manifest_zip_exists) {
		xprintf("'%s' exists but '%s' missing; unzipping...\r\n", manifest_zip_path, MANIFEST_DIR);
		int uz = fatfs_unzip_manifest();
		xprintf("Unzip result: %d\r\n", uz);
		// Re-check after unzip
		manifest_dir_exists = (f_stat(MANIFEST_DIR, &fno) == FR_OK);
		xprintf("Post-unzip stat('%s') = %s\r\n", MANIFEST_DIR, manifest_dir_exists ? "OK" : "MISSING");
		// Also check that CONFIG.TXT exists (this should be present for config-only zips)
		FRESULT cfg_stat = f_stat(MANIFEST_DIR "/" STATE_FILE, &fno);
		xprintf("Post-unzip stat('%s/%s') = %d\r\n", MANIFEST_DIR, STATE_FILE, cfg_stat);
	}
	else {
		xprintf("Neither '%s' nor '%s' exists; creating default config\r\n", MANIFEST_ZIP_CANON, MANIFEST_DIR);
		// Neither manifest.zip nor manifest dir exists: create config dir and default config file.
		res = ensure_directory_exists(MANIFEST_DIR);
		if (res != FR_OK) {
			dirManager->configRes = res;
			return res;
		}
		// Initialize current_config_dir BEFORE calling ensure_default_config_file_exists
		strcpy(dirManager->current_config_dir, MANIFEST_DIR);

		res = f_chdir(MANIFEST_DIR);
		if (res != FR_OK) {
			dirManager->configRes = res;
			return res;
		}
		res = ensure_default_config_file_exists(dirManager);
		// Always restore original dir
		(void)f_chdir(dirManager->base_dir);
		if (res != FR_OK) {
			dirManager->configRes = res;
			return res;
		}
		manifest_dir_exists = true;
	}

	// IMPORTANT: if the zip existed but unzip did not create /MANIFEST, do NOT silently
	// create /MANIFEST here. That masks unzip failures and produces the "only CONFIG.TXT exists"
	// behavior you observed.
	// Only case (3) is allowed to create a default /MANIFEST.
	if (!manifest_dir_exists && manifest_zip_exists)
	{
		xprintf("Manifest zip exists but '%s' was not created by unzip; leaving as-is.\r\n", MANIFEST_DIR);
		dirManager->configRes = FR_NO_PATH;
		// Continue init so the rest of SD (images dir) still works, but config will fail.
	}
	else if (!manifest_dir_exists)
	{
		// Case (3) should have made it exist; if not, try once.
		res = ensure_directory_exists(MANIFEST_DIR);
		if (res != FR_OK)
		{
			dirManager->configRes = res;
			return res;
		}
	}

	strcpy(dirManager->current_config_dir, MANIFEST_DIR);
#else
	// Original code
	xsprintf(path_buf, CONFIG_DIR);
	res = f_stat(path_buf, &fno);
	if (res == FR_OK) {
		xprintf("Directory '%s' exists\r\n", path_buf);
	}
	else {
		xprintf("Creating directory '%s'\r\n", path_buf);
		res = f_mkdir(path_buf);
		if (res != FR_OK) {
			xprintf("f_mkdir(config) failed (%d)\r\n", res);
			dirManager->configRes = res;
			return dirManager->configRes;
		}
	}
	strcpy(dirManager->current_config_dir, path_buf); // Set initial result for config operations

#endif //UNZIPMANIFEST

	// === IMAGES DIRECTORY ===

	// CGP 31/3/26
	// This needs to be re-factored.
#if 0
	// Use 8.3 file name format
	snprintf(path_buf, sizeof(path_buf), "%s.%03d", CAPTURE_DIR, dirManager->imagesDirIdx);

	res = f_stat(path_buf, &fno);
	if (res == FR_OK) {
		xprintf("Directory '%s' exists\r\n", path_buf);
	}
	else {
		xprintf("Creating directory '%s'\r\n", path_buf);
		res = f_mkdir(path_buf);
		if (res != FR_OK) {
			xprintf("f_mkdir(images) failed (%d)\r\n", res);
			dirManager->imagesRes = res;
			return dirManager->imagesRes;
		}
		// TODO - what to do here?
		strcpy(dirManager->current_capture_dir, path_buf); // Set initial result for image operations

		dirManager->imagesDirIdx = 1;
	}
#else
	// TODO - temporary only as the file may change after we load the CONFIG.TXT values
	dir_mgr_generateImageDirName(path_buf, sizeof(path_buf));
	dir_mgr_createImageDir(path_buf);	// Having created the name, create the folder if necessary

	dirManager->imagesDirIdx = 1;
#endif

	dirManager->configOpen = false;
	dirManager->imagesOpen = false;

	return res;
}

// FRESULT init_directories(DirectoryManager *manager) {
//     for (int i = 0; i < DIR_COUNT; i++) {
//         strncpy(manager->dirs[i].path, folder_defaults[i], MAX_DIR_NAME_LEN);
//         manager->dirs[i].res = create_path(manager->dirs[i].path);
//         if (manager->dirs[i].res != FR_OK && manager->dirs[i].res != FR_EXIST) {
//             printf("Error creating %s: %d\n", manager->dirs[i].path, manager->dirs[i].res);
//             return manager->dirs[i].res;
//         }
//     }
//     return FR_OK;
// }
//
///**
// * Creates a new folder for image captures when threshold met.
// *
// * Realistic use case will be to check the number of captures within a folder,
// * when it reaches the threshold (fatfs_getImageSequenceNumber),
// * this function gets called to create a new folder
// * and points to this directory for new captures to be stored.
// *
// * @param dirManager Pointer to the directory manager structure to initialize.
// * @return FRESULT indicating the success or failure of the operation.
// */
//FRESULT dir_mgr_add_capture_folder(directoryManager_t *dirManager) {
//	char path_buf[IMAGEFILENAMELEN];
//
//	// 999 is an arbitrary number
//	// TODO, what we set the limits to and when to shift to a new capture folder
//	if (dirManager->imagesDirIdx < MAXIMAGEDIRECTORIES) {
//		dirManager->imagesDirIdx++;
//
//#if 0
//		// TODO Compiler says 'warning: 'snprintf' output may be truncated before the last format character'
//		snprintf(path_buf, sizeof(path_buf), "%s.%03d", CAPTURE_DIR, dirManager->imagesDirIdx);
//#else
//		dir_mgr_generateImageDirName(path_buf, sizeof(path_buf));
//#endif
//		// Creates the new folder
//		FRESULT res = f_mkdir(path_buf);
//		if (res != FR_OK)
//		{
//			xprintf("Failed to create folder: %s, error: %d\n", path_buf, res);
//			return res;
//		}
//		dirManager->imagesRes = FR_OK;
//	}
//	else {
//		xprintf("Folder index too high: %d (max %d)\n", dirManager->imagesDirIdx, MAXIMAGEDIRECTORIES);
//		dirManager->imagesRes = FR_INVALID_NAME; // Set error in directory manager
//		return dirManager->imagesRes;
//	}
//	strcpy(dirManager->current_capture_dir, path_buf); // Update current directory
//	return dirManager->imagesRes;
//}
//
///**
// * Deletes a folder for image captures.
// * @param path_buf Name of the folder to be deleted.
// * @param dirManager Pointer to the directory manager structure.
// * @return FRESULT indicating the success or failure of the operation.
// */
//FRESULT dir_mgr_delete_capture_folder(const char *path_buf, directoryManager_t *dirManager) {
//	FRESULT res = f_rmdir(path_buf);
//	if (res != FR_OK)
//	{
//		xprintf("Failed to delete folder: %s, error: %d\n", path_buf, res);
//		dirManager->imagesRes = res; // Update directory manager with error
//	}
//	else
//	{
//		xprintf("Successfully deleted folder: %s\n", path_buf);
//		dirManager->imagesRes = FR_OK; // Update directory manager on success
//		dirManager->imagesDirIdx--;	   // Decrease folder count
//	}
//	return res;
//}
//

/**
 * Generate a filename for the image (jpeg) file.
 *
 * Must use 8.3 file name: upper case alphanumeric
 *
 * The name is 8 hex characters (which can be generated by a 32-bit integer).
 * The names is generated as follows:
 * - find the Unix epoch timestamp (seconds since 1/1/1970)
 * - shitft is left 4 bits (i.e.lose the first nibble)
 * - make the LS nibble '0', but use successive values (up to 'F')
 * 		if there are more than 1 file generated within this same second.
 *
 * 	This gives file names with these characteristics:
	1    When sorted in alphanumeric order that is also chronological order.
	2    Up to 16 files per second
	3    The file name can be converted to Unix time (1s resolution) by shifting 4 bits right and pre-pending '6' (or whatever).
 *
 * @param imageFileName character array to contain the name
 * @param filenameLen - length of that array
 * @param type character array to contain the extension (JPG or BMP)
 */
void dir_mgr_generateImageFilename(char *imageFileName, uint8_t filenameLen, char * type) {

	uint32_t seconds;
	static uint32_t old = 1;
	static uint8_t subSecond = 0;	// increment by 1 if this is called several times in teh same second

	exif_utc_get_rtc_as_seconds(&seconds);
	// check this is linux epoch
	//xprintf("Linux epoch 0x%08x ", seconds);

	if (seconds == old) {
		// we have called this function more than once this second, so increment LS digit
		if (subSecond < 15) {
			subSecond++;
		}
		else {
			// Unlikely - here if we get >= 16 images in teh same second.
			// tough: we have to overwrite the previous file
		}
	}
	else {
		subSecond = 0;
	}

	old = seconds;

	// Now create a value which is seconds * 16 + subSeconds
	seconds = (seconds << 4) + subSecond;
	snprintf(imageFileName, filenameLen, "%08X.%s", (int) seconds, type);
}


/**
 * Generate a directory name for images
 *
 * Must use 8.3 file name: upper case alphanumeric
 *
 * @param imageDirName character array to contain the name
 * @param dirNameLen - length of that array
 */
void dir_mgr_generateImageDirName(char * imageDirName, uint8_t dirNameLen) {
	uint16_t imagesCount;
	uint16_t imagesIndex;

	imagesCount = fatfs_getOperationalParameter(OP_PARAMETER_IMAGES_COUNT);
	imagesIndex = fatfs_getOperationalParameter(OP_PARAMETER_IMAGES_FILE_INDEX);

	XP_GREEN;
	// At every warm boot, check if the current images folder is full. If so, make a new one.
	if ((imagesCount > MAXIMAGESPERDIRECTORY) && (imagesIndex < MAXIMAGEDIRECTORIES)) {
		xprintf("DEBUG: created a new images directory (%d > %d)\n", imagesCount, MAXIMAGESPERDIRECTORY);

		imagesIndex++;
		fatfs_setOperationalParameter(OP_PARAMETER_IMAGES_FILE_INDEX, imagesIndex);
		fatfs_setOperationalParameter(OP_PARAMETER_IMAGES_COUNT, 0);
	}
	else {
		xprintf("DEBUG: retained old images directory (%d <= %d)\n", imagesCount, MAXIMAGESPERDIRECTORY);
	}
	XP_WHITE;

	snprintf(imageDirName, dirNameLen, "%s.%03d", CAPTURE_DIR, imagesIndex);
}

/**
 * Create a directory to accept the images
 */
void dir_mgr_createImageDir(char * path_buf) {
	FRESULT res;
	FILINFO fno;

	res = f_stat(path_buf, &fno);

	if (res == FR_OK) {
		xprintf("Directory '%s' exists\r\n", path_buf);
	}
	else {
		xprintf("Creating directory '%s'\r\n", path_buf);
		res = f_mkdir(path_buf);
		if (res != FR_OK) {
			xprintf("f_mkdir(images) failed (%d)\r\n", res);
			dirManager.imagesRes = res;
			//return dirManager->imagesRes;
		}
	}

	strcpy(dirManager.current_capture_dir, path_buf); // Set initial result for image operations
}
