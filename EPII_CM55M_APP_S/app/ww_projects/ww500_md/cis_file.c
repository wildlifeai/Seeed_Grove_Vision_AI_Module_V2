/*
 * cis_file.c
 *
 * Functions to program cis sensors with data from files containing binary data.
 *
 * These files are written by the python script 'scan_cis_settings.py'
 * and contain binary data.
 *
 * Thanks to ChatGPT!
 *
 *  Created on: 16 Mar 2025
 *      Author: charl
 */
#include "cis_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"

#include "xprintf.h"
#include "fatfs_task.h"
#include "image_task.h"	// for CAMERA_EXTRA_FILE

// Register settings staged for the camera "extra settings" file. Loaded from the file
// by cis_file_process() at sensor init, and edited by the 'camreg' CLI command, which
// re-saves the table to the same file. Keeping the table here (rather than in the CLI)
// means the CLI's view stays consistent with the SD card across DPD cycles and reboots.
static HX_CIS_SensorSetting_t stagedSettings[CIS_FILE_MAX_STAGED];
static uint16_t stagedCount = 0;

// True once the staged table reflects CAMERA_EXTRA_FILE (or its absence).
// Guards against a fresh boot where the CLI is used before the camera has
// initialised (or with the camera disabled): without loading first, 'camreg
// list' would wrongly report an empty table and the first 'camreg' write
// would save a 1-entry file, erasing the registers already on the SD card.
static bool stagedLoaded = false;

/**
 * Load the staged table from CAMERA_EXTRA_FILE if that has not happened yet.
 *
 * FatFs here is NOT re-entrant (FF_FS_REENTRANT = 0), so this must only run
 * where no other task is using the disk: the fatfs_task calls it once at boot
 * (after mounting, before other tasks start disk activity), and
 * cis_file_process() covers it from the image task at sensor init.
 * A missing file counts as loaded (genuinely no staged registers).
 */
void cis_file_loadStagedFromFile(void) {
	FIL file;
	FRESULT res;
	UINT bytesRead;
	uint16_t numEntries;
	HX_CIS_SensorSetting_t fileSettings[CIS_FILE_MAX_STAGED];

	if (stagedLoaded) {
		return;
	}

	if (!fatfs_mounted()) {
		return;	// try again on the next call
	}

	res = f_open(&file, CAMERA_EXTRA_FILE, FA_READ);

	if ((res == FR_NO_FILE) || (res == FR_NO_PATH)) {
		// No file - there really are no staged registers
		stagedLoaded = true;
		return;
	}
	if (res != FR_OK) {
		xprintf("cis_file_loadStagedFromFile: error %d opening '%s'\n", res, CAMERA_EXTRA_FILE);
		return;	// try again on the next call
	}

	numEntries = f_size(&file) / sizeof(HX_CIS_SensorSetting_t);
	if (numEntries > CIS_FILE_MAX_STAGED) {
		xprintf("Warning: '%s' has %d entries; only the first %d can be edited by 'camreg'\n",
				CAMERA_EXTRA_FILE, numEntries, CIS_FILE_MAX_STAGED);
		numEntries = CIS_FILE_MAX_STAGED;
	}

	if (numEntries == 0) {
		f_close(&file);
		stagedLoaded = true;
		return;
	}

	res = f_read(&file, fileSettings, numEntries * sizeof(HX_CIS_SensorSetting_t), &bytesRead);
	f_close(&file);

	if ((res != FR_OK) || (bytesRead != numEntries * sizeof(HX_CIS_SensorSetting_t))) {
		xprintf("cis_file_loadStagedFromFile: error %d reading '%s'\n", res, CAMERA_EXTRA_FILE);
		return;	// try again on the next call
	}

	taskENTER_CRITICAL();
	memcpy(stagedSettings, fileSettings, numEntries * sizeof(HX_CIS_SensorSetting_t));
	stagedCount = numEntries;
	stagedLoaded = true;
	taskEXIT_CRITICAL();

	xprintf("Loaded %d staged register(s) from '%s'\n", numEntries, CAMERA_EXTRA_FILE);
}

bool cis_file_isStagedLoaded(void) {
	return stagedLoaded;
}

uint16_t cis_file_getStagedCount(void) {
	return stagedCount;
}

HX_CIS_SensorSetting_t * cis_file_getStagedTable(void) {
	return stagedSettings;
}

bool cis_file_stageReg(uint16_t addr, uint8_t val) {
	uint16_t i;
	bool success = true;

	// Refuse until the table reflects the SD card, otherwise the first save
	// could overwrite registers that are already staged in the file
	if (!stagedLoaded) {
		return false;
	}

	// The staged table is written by the CLI task (here) and by the image task
	// (cis_file_process() at sensor init) - guard against concurrent updates
	taskENTER_CRITICAL();

	for (i = 0; i < stagedCount; i++) {
		if (stagedSettings[i].RegAddree == addr) {
			break;
		}
	}

	if (i == stagedCount) {
		if (stagedCount == CIS_FILE_MAX_STAGED) {
			success = false;
		}
		else {
			stagedCount++;
		}
	}

	if (success) {
		stagedSettings[i].I2C_ActionType = HX_CIS_I2C_Action_W;
		stagedSettings[i].RegAddree = addr;
		stagedSettings[i].Value = val;
	}

	taskEXIT_CRITICAL();

	return success;
}

void cis_file_clearStaged(void) {
	taskENTER_CRITICAL();
	stagedCount = 0;
	stagedLoaded = true;	// an explicit clear is authoritative - do not reload the file
	taskEXIT_CRITICAL();
}

/**
 * Read CIS register settings from a file and process them
 *
 * @param filename - name of file containing binary data
 * @return error code
 */
HX_CIS_ERROR_E cis_file_process(const char *filename) {
    FIL file;
    FRESULT res;
    UINT bytes_read;
    HX_CIS_ERROR_E result;
    DWORD file_size;
    uint16_t num_entries ;

    if (!fatfs_mounted()) {
        xprintf("SD card not mounted.\n");
    	return FR_NO_FILESYSTEM;
    }

    // DEBUG - find out where we are!

    char cur_dir[128]; //8.3? or full path?
    UINT len = 128;

    res = f_getcwd(cur_dir, len);      /* Get current directory */
    xprintf("CWD is '%s'\n", cur_dir);

    // Open file
    res = f_open(&file, filename, FA_READ);
    if (res != FR_OK) {
        xprintf("Error opening '%s': %d\n", filename, res);
        return HX_CIS_ERROR_INVALID_PARAMETERS;
    }

    // Get file size
    file_size = f_size(&file);

    if (file_size == 0) {
    	//This can happen if teh python script fails to process the .txt file
        xprintf("Error: File has no contents\n");
        f_close(&file);
        return HX_CIS_ERROR_INVALID_PARAMETERS;
    }

    if (file_size % sizeof(HX_CIS_SensorSetting_t) != 0) {
        xprintf("Error: Invalid file size\n");
        f_close(&file);
        return HX_CIS_ERROR_INVALID_PARAMETERS;
    }

    num_entries = file_size / sizeof(HX_CIS_SensorSetting_t);

    // Allocate memory
    HX_CIS_SensorSetting_t *sensor_settings = pvPortMalloc(file_size);

    if (!sensor_settings) {
        xprintf("Memory allocation of %d bytes failed\n", file_size);
        f_close(&file);
        return HX_CIS_UNKNOWN_ERROR;
    }

    // Read the binary data
    res = f_read(&file, sensor_settings, file_size, &bytes_read);
    f_close(&file);

    if (res != FR_OK || bytes_read != file_size) {
        xprintf("Error reading file: %d\n", res);
        vPortFree(sensor_settings);
        return HX_CIS_UNKNOWN_ERROR;
    }

    // Mirror the file contents into the staged table so the 'camreg' CLI command
    // edits what is actually on the SD card. Only CAMERA_EXTRA_FILE feeds the
    // staged table, and only if it has not been loaded yet - once loaded, the
    // in-memory table is authoritative (it may hold edits whose save to the SD
    // card is still in flight). Entries beyond CIS_FILE_MAX_STAGED are still
    // applied below but cannot be edited by 'camreg'.
    if (strcmp(filename, CAMERA_EXTRA_FILE) == 0) {
        if (num_entries > CIS_FILE_MAX_STAGED) {
            xprintf("Warning: '%s' has %d entries; only the first %d can be edited by 'camreg' (extras are lost if re-saved)\n",
                    filename, num_entries, CIS_FILE_MAX_STAGED);
        }
        taskENTER_CRITICAL();
        if (!stagedLoaded) {
            stagedCount = (num_entries <= CIS_FILE_MAX_STAGED) ? num_entries : CIS_FILE_MAX_STAGED;
            memcpy(stagedSettings, sensor_settings, stagedCount * sizeof(HX_CIS_SensorSetting_t));
            stagedLoaded = true;
        }
        taskEXIT_CRITICAL();
    }

    // Apply the settings
    result = hx_drv_cis_setRegTable(sensor_settings, num_entries);
    if (result == HX_CIS_NO_ERROR) {
        xprintf("Processed %d settings from '%s'\n", num_entries, filename);
    }
    else {
        xprintf("Error: hx_drv_cis_setRegTable failed with code %d\n", result);
    }

    vPortFree(sensor_settings);

    return result;
}

/**
 * Allows testing
 *
 * @param filename -
 * @param apply_settings - if true then write actual values. If false then print info
 */
void cis_file_test(const char *filename, bool apply_settings) {
	FIL file;
	FRESULT res;
	UINT bytes_read;
	HX_CIS_ERROR_E result;

    if (!fatfs_mounted()) {
        xprintf("SD card not mounted.\n");
    	return;
    }

	// Open file
	res = f_open(&file, filename, FA_READ);
	if (res != FR_OK) {
		xprintf("Error opening file: %d\n", res);
		return;
	}

	// Get file size
	DWORD file_size = f_size(&file);

	if (file_size % sizeof(HX_CIS_SensorSetting_t) != 0) {
		xprintf("Error: Invalid file size\n");
		f_close(&file);
		return;
	}

	uint16_t num_entries = file_size / sizeof(HX_CIS_SensorSetting_t);

	// Allocate memory
	HX_CIS_SensorSetting_t *sensor_settings = pvPortMalloc(file_size);

	if (!sensor_settings) {
		xprintf("Memory allocation failed\n");
		f_close(&file);
		return;
	}

	// Read the binary data
	res = f_read(&file, sensor_settings, file_size, &bytes_read);
	f_close(&file);

	if (res != FR_OK || bytes_read != file_size) {
		xprintf("Error reading file: %d\n", res);
		vPortFree(sensor_settings);
		return;
	}

	if (apply_settings) {
		// Apply the settings using hx_drv_cis_setRegTable
		result = hx_drv_cis_setRegTable(sensor_settings, num_entries);
	    if (result == HX_CIS_NO_ERROR) {
	        xprintf("Processed %d settings from '%s'\n", num_entries, filename);
	    }
	    else {
	        xprintf("Error: hx_drv_cis_setRegTable failed with code %d\n", result);
	    }
	}
	else {
		// Print the parsed data
	    xprintf("Parsing %d settings from '%s':\n", num_entries, filename);

		for (uint16_t i = 0; i < num_entries; i++) {
			xprintf("Action: 0x%02X, Register: 0x%04X, Value: 0x%02X\n",
					sensor_settings[i].I2C_ActionType, sensor_settings[i].RegAddree, sensor_settings[i].Value);
		}
	}

	vPortFree(sensor_settings);
}

