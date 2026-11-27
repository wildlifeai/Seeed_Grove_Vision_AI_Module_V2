/*
 * xip_state.h
 *
 *  Created on: 28 Oct 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  Saved state in the XIP flash: small records that survive DPD and power cycles
 *  without the SD card. See _Documentation/development reports/2026-10-27_XIP_save_state/.
 *
 *  Three 4 KB sectors of the saved-state area (XIP_STATE_AREA_ADDR, xip_manager.h),
 *  one per record type, so the frequent erases of one can never touch the other:
 *    0x00F00000  fast-start records (the settings the first photo needs)
 *    0x00F01000  board record: values that belong to this board (the RTC calibration, rtc_cal.c)
 *    0x00F02000  deployment record: the deployment ID and GPS position, a copy of CONFIG.TXT's 'I' and
 *                'G' lines, used for whichever of them CONFIG.TXT didn't give: no card, no file,
 *                damaged, or the line missing (#298, 27 Nov 2026)
 *
 *  Each sector holds 16 records of 256 bytes (one flash page each). A new record
 *  goes into the slot after the last used one; when the sector is full it is
 *  erased and the record goes into slot 0. A reader takes the valid record
 *  (magic, format version, type, CRC) with the highest sequence number. If there
 *  is none (blank sector, new format, bad CRC, power cut during an erase) the
 *  caller uses its C defaults.
 *
 *  The fast-start record is a copy of every op parameter except the counters (32 of them)
 *  and the staged extra camera registers. CONFIG.TXT stays the master copy for settings; for the
 *  state ops (25, 37, 38) the values in RAM, which start from the record, are kept. It is read at
 *  the start of the FatFS task, so the camera can start before the card is ready, and written at
 *  sleep when something has changed.
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
// Raise this whenever the record or the fast-start payload changes: older fast-start records are then ignored
#define XIP_STATE_FORMAT_VERSION	2		// 2: 32 ops (28 Oct 2026); 1 had 23
// The board record's own version, so a new fast-start format never loses a board's calibration.
// Raise it only when the board payload changes, and keep reading the old one if possible
#define XIP_STATE_BOARD_VERSION		2		// 2: the RTC calibration (31 Oct 2026); 1 had the RC32K trim (no effect in DPD)
// The deployment record's own version. Raise it only when its payload changes
#define XIP_STATE_DEPLOYMENT_VERSION	1		// 1: deployment ID and GPS position (27 Nov 2026, #298)

/*********************************************** Global Types ************************************************/

typedef enum {
	XIP_STATE_TYPE_FAST_START	= 1,	// sector 0x00F00000
	XIP_STATE_TYPE_BOARD		= 2,	// sector 0x00F01000: this board's values (the RTC calibration)
	XIP_STATE_TYPE_DEPLOYMENT	= 3,	// sector 0x00F02000: the deployment ID and GPS position (#298)
} XIP_STATE_TYPE_E;

// The payload of a board record: values that belong to this board. Change it only with XIP_STATE_BOARD_VERSION
typedef struct {
	uint8_t  rtcCalValid;		// 1: rtcPpm and rtcMeasuredAt hold a measurement
	uint8_t  rtcCalOff;			// 1: no correction and no measuring ('rtccal off')
	uint16_t reserved;			// 0
	int32_t  rtcPpm;			// the RC32K clock's error, measured awake (rtc_cal.c): negative means fast (#245)
	uint32_t rtcMeasuredAt;		// RTC seconds when it was measured
} XipStateBoard_t;

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
 * @brief Print every slot of every sector on the console (the 'xipstate' command).
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

/**
 * @brief While CONFIG.TXT loads: whether to keep RAM's value of a state op (25, 37, 38).
 */
bool xip_state_keepStateOp(uint8_t op, uint16_t cardValue);

/**
 * @brief Read the board record (all zero if there is none).
 */
bool xip_state_readBoard(XipStateBoard_t *board);

/**
 * @brief Write the board record, if it differs from the one in flash.
 */
int xip_state_writeBoard(const XipStateBoard_t *board);

/**
 * @brief At boot, with the fast-start record: read the deployment record into RAM (#298).
 */
bool xip_state_loadDeployment(void);

/**
 * @brief When CONFIG.TXT didn't give them: set the deployment ID and/or GPS position from the record read at boot.
 */
bool xip_state_applyDeployment(bool id, bool gps);

/**
 * @brief At sleep: write the deployment record if the deployment ID or GPS position has changed.
 */
int xip_state_saveDeployment(void);

#endif /* XIP_STATE_H_ */
