/*
 * xip_state.h
 *
 *  Created on: 28 Oct 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  Saved state in the XIP flash: small records that survive DPD and power cycles
 *  without the SD card. See _Documentation/development reports/2026-10-27_XIP_save_state/.
 *
 *  Two 4 KB sectors of the saved-state area (XIP_STATE_AREA_ADDR, xip_manager.h),
 *  one per record type, so the frequent erases of one can never touch the other:
 *    0x00F00000  fast-start records (the settings the first photo needs)
 *    0x00F01000  board record (clock calibration) - reserved, not written yet
 *
 *  Each sector holds 16 records of 256 bytes (one flash page each). A new record
 *  goes into the slot after the last used one; when the sector is full it is
 *  erased and the record goes into slot 0. A reader takes the valid record
 *  (magic, format version, type, CRC) with the highest sequence number. If there
 *  is none (blank sector, new format, bad CRC, power cut during an erase) the
 *  caller uses its C defaults.
 *
 *  The fast-start record is a copy of the settings the first photo needs (23 op parameters)
 *  and the staged extra camera registers. CONFIG.TXT stays the master copy. It is read at the
 *  start of the FatFS task and written at sleep when something has changed.
 *
 *  Not for use while the NN is running: every flash access switches XIP off.
 */

#ifndef XIP_STATE_H_
#define XIP_STATE_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>

/*********************************************** Global Defines **********************************************/

#define XIP_STATE_RECORD_SIZE		256		// one flash page: a write never crosses a page boundary
#define XIP_STATE_DATA_MAX			240		// payload bytes in one record
// Raise this whenever the record or a payload layout changes: older records are then ignored
#define XIP_STATE_FORMAT_VERSION	1

/*********************************************** Global Types ************************************************/

typedef enum {
	XIP_STATE_TYPE_FAST_START	= 1,	// sector 0x00F00000
	XIP_STATE_TYPE_BOARD		= 2,	// sector 0x00F01000: reserved for the clock calibration, not written yet
} XIP_STATE_TYPE_E;

/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

/**
 * @brief Read the newest valid record of a type (false means use the C defaults).
 */
bool xip_state_read(XIP_STATE_TYPE_E type, uint8_t *data, uint16_t *len);

/**
 * @brief Append a record, erasing the sector first if it is full, and check it by reading it back.
 */
int xip_state_write(XIP_STATE_TYPE_E type, const uint8_t *data, uint16_t len);

/**
 * @brief Print every slot of both sectors on the console (the 'xipstate' command).
 */
void xip_state_dump(void);

/**
 * @brief Bench test of the fast-start sector (the 'xipstate test' command).
 */
bool xip_state_test(void);

/**
 * @brief At boot: fill op_parameter[] from the fast-start record, if there is one.
 */
bool xip_state_loadFastStart(void);

/**
 * @brief After CONFIG.TXT has loaded: print any difference from the record read at boot.
 */
void xip_state_compareFastStart(void);

/**
 * @brief At sleep: write a new fast-start record if anything has changed.
 */
int xip_state_saveFastStart(void);

#endif /* XIP_STATE_H_ */
