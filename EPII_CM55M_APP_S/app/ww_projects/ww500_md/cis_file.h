/*
 * cis_file.h
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

#ifndef CIS_FILE_H_
#define CIS_FILE_H_

#include <stdint.h>
#include <stdbool.h>
#include "hx_drv_CIS_common.h"
#include "ff.h"  // FatFs library

// Maximum number of register settings that can be staged by the 'camreg' CLI command.
// Files larger than this still have all their settings applied by cis_file_process(),
// but only the first CIS_FILE_MAX_STAGED entries can be edited/re-saved by 'camreg'.
#define CIS_FILE_MAX_STAGED 24

/**
 * @brief Processes a binary file and applies the sensor register settings.
 *
 * Also loads the settings into the staged table (see cis_file_getStagedTable())
 * so the 'camreg' CLI command can edit and re-save them.
 *
 * @param filename The path to the binary file.
 * @return HX_CIS_ERROR_E Returns HX_CIS_NO_ERROR on success, otherwise an error code.
 */
HX_CIS_ERROR_E cis_file_process(const char *filename);

/**
 * @brief Load the staged register table from CAMERA_EXTRA_FILE.
 *
 * MUST be called from a context where no other task is using FatFs
 * (FF_FS_REENTRANT is 0). The fatfs_task calls this once at boot, after the
 * SD card is mounted and before other tasks start disk activity.
 * A missing file counts as loaded (there really are no staged registers).
 */
void cis_file_loadStagedFromFile(void);

/**
 * @brief Whether the staged table has been loaded from (or reconciled with)
 * the SD card. When false, staging and clearing are refused so an unloaded
 * table can never overwrite the registers already saved on the card.
 */
bool cis_file_isStagedLoaded(void);

/**
 * @brief Number of register settings currently staged.
 */
uint16_t cis_file_getStagedCount(void);

/**
 * @brief The staged register settings table (CIS_FILE_MAX_STAGED entries allocated).
 */
HX_CIS_SensorSetting_t * cis_file_getStagedTable(void);

/**
 * @brief Add a register write to the staged table, or update it if the address is
 * already present.
 *
 * @return false if the table is full.
 */
bool cis_file_stageReg(uint16_t addr, uint8_t val);

/**
 * @brief Empty the staged table.
 */
void cis_file_clearStaged(void);

/**
 * @brief Test function to either apply settings or print parsed data.
 *
 * @param filename The path to the binary file.
 * @param apply_settings If true, calls hx_drv_cis_setRegTable(); otherwise, prints data.
 */
void cis_file_test(const char *filename, bool apply_settings);

#endif /* CIS_FILE_H_ */
