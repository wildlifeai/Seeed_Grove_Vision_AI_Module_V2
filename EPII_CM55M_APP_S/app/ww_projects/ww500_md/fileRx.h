/*
 * fileRx.h
 *
 *  Created on: 21 Apr 2026
 *      Author: CGP / Claude
 *
 * Manages the HX6538 side of a generic file-receive session.
 * Files arrive from the nRF52832 over I2C in FILE_START / FILE_DATA / FILE_END
 * frames and are written to /MANIFEST/<filename> on the SD card.
 *
 * SD card I/O is performed asynchronously via fatfs_task using
 * APP_MSG_FATFSTASK_OPEN_FILE / APPEND_FILE / CLOSE_FILE messages.
 * This module manages protocol state and CRC accumulation only;
 * if_task.c drives the fatfs_task interactions.
 */

#ifndef APP_WW_PROJECTS_WW500_MD_FILERX_H_
#define APP_WW_PROJECTS_WW500_MD_FILERX_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>

/*********************************************** Global Type Declarations ************************************/

/*
 * Result codes returned by fileRx functions.
 * Codes 1-4 are generated on the nRF52832 side.
 * Codes 5-9 are generated here and reported to the nRF via "ftx err <n>".
 */
typedef enum {
    FILERX_OK               = 0,
    FILERX_ERR_BAD_FILENAME = 5,  // filename failed 8.3 validation
    FILERX_ERR_FILE_OPEN    = 6,  // fatfs_task failed to open/create the file
    FILERX_ERR_FILE_WRITE   = 7,  // fatfs_task reported a write error
    FILERX_ERR_SEQ_MISMATCH = 8,  // packet sequence number out of order
    FILERX_ERR_CRC_MISMATCH = 9,  // whole-file CRC verification failed
} fileRx_result_t;

/*********************************************** Global Function Declarations ********************************/

/*
 * Begin a new file transfer session.
 *
 * Validates the filename (8.3 format, uppercase letters/digits/'-'/'_'),
 * builds the full /MANIFEST/<filename> path, and initialises the CRC
 * accumulator and sequence counter.
 *
 * Returns FILERX_OK on success; FILERX_ERR_BAD_FILENAME if validation fails.
 * Does not perform any SD card I/O — the caller sends OPEN_FILE to fatfs_task.
 */
fileRx_result_t fileRx_start(const char *filename, uint32_t totalSize);

/*
 * Process one FILE_DATA chunk.
 *
 * Validates the packet sequence number (must increment by 1, wrapping 255 -> 1).
 * Updates the running CRC and byte counter.
 *
 * Returns FILERX_OK on success; FILERX_ERR_SEQ_MISMATCH on sequence error.
 * Does not perform any SD card I/O — the caller sends APPEND_FILE to fatfs_task.
 */
fileRx_result_t fileRx_data(const uint8_t *chunk, uint16_t len, uint8_t packetNum);

/*
 * Finalise the transfer after FILE_END is received.
 *
 * Computes the final CRC and compares it to receivedCrc.
 * Sets deleteOnClose if the CRC does not match.
 *
 * Returns FILERX_OK on CRC match; FILERX_ERR_CRC_MISMATCH otherwise.
 * The caller should always send CLOSE_FILE to fatfs_task regardless of the return value;
 * fileRx_shouldDelete() indicates whether the file should also be deleted.
 */
fileRx_result_t fileRx_end(uint16_t receivedCrc);

/*
 * Abort the current session (e.g. on BLE disconnect or unrecoverable error).
 * Sets the deleteOnClose flag so the caller will delete the partial file.
 * Does not reset the path — the caller may still need it for CLOSE_FILE.
 */
void fileRx_abort(void);

/*
 * Returns the bare filename for the current session (e.g. "FOO.BIN").
 * fatfs_task resolves the directory via f_chdir(dirManager.current_config_dir).
 * Valid from fileRx_start() until the next fileRx_start() call.
 */
const char *fileRx_getFileName(void);

/*
 * Returns true if the file should be deleted when closed (after error or abort).
 */
bool fileRx_shouldDelete(void);

#endif /* APP_WW_PROJECTS_WW500_MD_FILERX_H_ */
