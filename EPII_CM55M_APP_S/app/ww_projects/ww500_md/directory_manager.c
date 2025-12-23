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

/*************************************** Definitions *******************************************/

#define MAXIMAGEDIRECTORIES 999

// Locations on SD card
// Note: CONFIG_DIR comes from directory_manager.h and is currently "/MANIFEST"
#define MANIFEST_DIR CONFIG_DIR
#define MANIFEST_ZIP_CANON "/MANIFEST.ZIP"

static const char *find_manifest_zip_path(void)
{
	// Be tolerant to case variants on the SD card, but treat /MANIFEST as canonical.
	// These are checked in priority order, preferring 8.3 uppercase.
	static const char *candidates[] = {
		MANIFEST_ZIP_CANON,
		"/MANIFEST.zip",
		"/Manifest.zip",
		"/manifest.zip",
		"/MANIFEST.ZIP",
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

	const char *header = "# Auto-created default configuration\n";
	(void)f_write(&f, header, strlen(header), &bw);

	res = f_close(&f);
	if (res != FR_OK)
	{
		xprintf("Failed to close config file '%s' (%d)\r\n", STATE_FILE, res);
		return res;
	}

	// Keep directory manager state consistent
	dirManager->configOpen = false;
	dirManager->configRes = FR_OK;
	xprintf("Created default config file '%s'\r\n", STATE_FILE);
	return FR_OK;
}

///*************************************** Global Function Definitions *****************************/

/**
 * Looks for a directory to save images.
 * If it exists, use it. Otherwise, create it.
 *
 * TBP - I've set FF_FS_LOCK to 2. This means that the directory
 * is locked when it is opened, and unlocked when it is closed.
 * This is to prevent other tasks from writing to the directory
 * while it is being used. 2 also means that it enables two directories
 * to be used simulantaneously.
 *
 * TODO - this should probably be the responsibility of the image task?
 * Or else: move generateImageFileName() to the fatfs task as well.
 */
FRESULT dir_mgr_init_directories(directoryManager_t *dirManager)
{
	FRESULT res;
	char path_buf[IMAGEFILENAMELEN];
	FILINFO fno;
	int len = sizeof(dirManager->base_dir);
	res = f_getcwd(dirManager->base_dir, len); /* Get current directory */
	dirManager->imagesDirIdx = 0;

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

	if (manifest_dir_exists)
	{
		xprintf("Directory '%s' exists\r\n", MANIFEST_DIR);
	}
	else if (manifest_zip_exists)
	{
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
	else
	{
		xprintf("Neither '%s' nor '%s' exists; creating default config\r\n", MANIFEST_ZIP_CANON, MANIFEST_DIR);
		// Neither manifest.zip nor manifest dir exists: create config dir and default config file.
		res = ensure_directory_exists(MANIFEST_DIR);
		if (res != FR_OK)
		{
			dirManager->configRes = res;
			return res;
		}
		res = f_chdir(MANIFEST_DIR);
		if (res != FR_OK)
		{
			dirManager->configRes = res;
			return res;
		}
		res = ensure_default_config_file_exists(dirManager);
		// Always restore original dir
		(void)f_chdir(dirManager->base_dir);
		if (res != FR_OK)
		{
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

	// === IMAGES DIRECTORY ===
#if FF_USE_LFN
	snprintf(path_buf, sizeof(path_buf), "%s_%04d", CAPTURE_DIR, dirManager->imagesDirIdx);
#else
	// Use 8.3 file name format
	snprintf(path_buf, sizeof(path_buf), "%s.%03d", CAPTURE_DIR, dirManager->imagesDirIdx);
#endif // FF_USE_LFN

	res = f_stat(path_buf, &fno);
	if (res == FR_OK)
	{
		xprintf("Directory '%s' exists\r\n", path_buf);
	}
	else
	{
		xprintf("Creating directory '%s'\r\n", path_buf);
		res = f_mkdir(path_buf);
		if (res != FR_OK)
		{
			xprintf("f_mkdir(images) failed (%d)\r\n", res);
			dirManager->imagesRes = res;
			return dirManager->imagesRes;
		}
		// TODO - what to do here?
		dirManager->imagesDirIdx = 1;
	}
	strcpy(dirManager->current_capture_dir, path_buf); // Set initial result for image operations
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

/**
 * Creates a new folder for image captures when threshold met.
 *
 * Realistic use case will be to check the number of captures within a folder,
 * when it reaches the threshold (fatfs_getImageSequenceNumber),
 * this function gets called to create a new folder
 * and points to this directory for new captures to be stored.
 *
 * @param dirManager Pointer to the directory manager structure to initialize.
 * @return FRESULT indicating the success or failure of the operation.
 */
FRESULT dir_mgr_add_capture_folder(directoryManager_t *dirManager)
{
	char path_buf[IMAGEFILENAMELEN];

	// 999 is an arbitrary number
	// TODO, what we set the limits to and when to shift to a new capture folder
	if (dirManager->imagesDirIdx < MAXIMAGEDIRECTORIES)
	{
		dirManager->imagesDirIdx++;
		uint16_t idx = dirManager->imagesDirIdx;

#if FF_USE_LFN
		snprintf(path_buf, sizeof(path_buf), "%s_%04d", CAPTURE_DIR, idx);
#else
		snprintf(path_buf, sizeof(path_buf), "%s.%03d", CAPTURE_DIR, idx);
#endif

		// Creates the new folder
		FRESULT res = f_mkdir(path_buf);
		if (res != FR_OK)
		{
			xprintf("Failed to create folder: %s, error: %d\n", path_buf, res);
			return res;
		}
		dirManager->imagesRes = FR_OK;
	}
	else
	{
		xprintf("Folder index too high: %d (max %d)\n", dirManager->imagesDirIdx, MAXIMAGEDIRECTORIES);
		dirManager->imagesRes = FR_INVALID_NAME; // Set error in directory manager
		return dirManager->imagesRes;
	}
	strcpy(dirManager->current_capture_dir, path_buf); // Update current directory
	return dirManager->imagesRes;
}

/**
 * Deletes a folder for image captures.
 * @param path_buf Name of the folder to be deleted.
 * @param dirManager Pointer to the directory manager structure.
 * @return FRESULT indicating the success or failure of the operation.
 */
FRESULT dir_mgr_delete_capture_folder(const char *path_buf, directoryManager_t *dirManager)
{
	FRESULT res = f_rmdir(path_buf);
	if (res != FR_OK)
	{
		xprintf("Failed to delete folder: %s, error: %d\n", path_buf, res);
		dirManager->imagesRes = res; // Update directory manager with error
	}
	else
	{
		xprintf("Successfully deleted folder: %s\n", path_buf);
		dirManager->imagesRes = FR_OK; // Update directory manager on success
		dirManager->imagesDirIdx--;	   // Decrease folder count
	}
	return res;
}
