/*
 * fileRx.c
 *
 *  Created on: 21 Apr 2026
 *      Author: CGP / Claude
 *
 * Manages the HX6538 side of a generic file-receive session.
 */

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "xprintf.h"

#include "fileRx.h"
#include "crc16_ccitt.h"
#include "printf_x.h"

/*********************************************** Local Defines ***********************************************/

#define FILERX_MAX_FILENAME     12      // 8.3: up to 8 base + '.' + up to 3 ext (= IMAGEFILENAMELEN - 1)

/*********************************************** Local Type Declarations *************************************/

typedef struct {
    char     fileName[FILERX_MAX_FILENAME + 1];
    uint32_t totalSize;
    uint32_t bytesReceived;
    uint8_t  lastPacketNum;     // 0 means no packet received yet
    uint16_t runningCrc;
    bool     deleteOnClose;
    bool     active;
} fileRxSession_t;

/*********************************************** Local Variables *********************************************/

static fileRxSession_t session;

/*********************************************** Local Function Definitions **********************************/

/*
 * Validate an 8.3 filename: uppercase A-Z, digits 0-9, '-', '_'.
 * Exactly one dot separating 1-8 char base from 1-3 char extension.
 * Returns true if valid.
 */
static bool validate_83_filename(const char *name)
{
    if (name == NULL) {
        return false;
    }

    const char *dot = strchr(name, '.');
    if (dot == NULL) {
        return false;
    }

    /* Check no second dot */
    if (strchr(dot + 1, '.') != NULL) {
        return false;
    }

    size_t base_len = (size_t)(dot - name);
    size_t ext_len  = strlen(dot + 1);

    if (base_len < 1 || base_len > 8) {
        return false;
    }
    if (ext_len < 1 || ext_len > 3) {
        return false;
    }

    static const char allowed[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";

    for (size_t i = 0; i < base_len; i++) {
        if (strchr(allowed, name[i]) == NULL) {
            return false;
        }
    }
    for (size_t i = 0; i < ext_len; i++) {
        if (strchr(allowed, dot[1 + i]) == NULL) {
            return false;
        }
    }

    return true;
}

/*********************************************** Global Function Definitions *********************************/

/**
 * Begin a new file transfer session.
 *
 * Validates the filename (8.3 format, uppercase letters/digits/'-'/'_'),
 * stores the bare filename, and initialises the CRC accumulator and
 * sequence counter.
 *
 * Does not perform any SD card I/O — the caller sends OPEN_FILE to fatfs_task,
 * which will chdir to current_config_dir before opening the file.
 *
 * @param filename   Null-terminated 8.3 filename (e.g. "HX6538V2.IMG")
 * @param totalSize  Total file size in bytes (informational; not validated here)
 * @return FILERX_OK on success; FILERX_ERR_BAD_FILENAME if validation fails
 */
fileRx_result_t fileRx_start(const char *filename, uint32_t totalSize) {

    if (!validate_83_filename(filename)) {
        return FILERX_ERR_BAD_FILENAME;
    }

    if (totalSize > FILERX_MAX_FILE_SIZE) {
        return FILERX_ERR_BAD_FILENAME;
    }

    memset(&session, 0, sizeof(session));

    strncpy(session.fileName, filename, FILERX_MAX_FILENAME);
    session.fileName[FILERX_MAX_FILENAME] = '\0';

    session.totalSize      = totalSize;
    session.bytesReceived  = 0;
    session.lastPacketNum  = 0;
    session.runningCrc     = crc16_ccitt_stream_init();
    session.deleteOnClose  = false;
    session.active         = true;

    XP_LT_BLUE;	// colour for File TX operations
    xprintf("FileTX: Receiving '%s' (%d bytes)\n", session.fileName, session.totalSize);
    XP_WHITE;

    return FILERX_OK;
}

/**
 * Process one FILE_DATA chunk.
 *
 * Validates the packet sequence number (must increment by 1, wrapping 255 -> 1).
 * Updates the running CRC and byte counter.
 *
 * Does not perform any SD card I/O — the caller sends APPEND_FILE to fatfs_task.
 *
 * @param chunk      Pointer to the raw chunk bytes
 * @param len        Number of bytes in this chunk
 * @param packetNum  Packet sequence number from the FILE_DATA frame (1–255)
 * @return FILERX_OK on success; FILERX_ERR_SEQ_MISMATCH if sequence is out of order
 */
fileRx_result_t fileRx_data(const uint8_t *chunk, uint16_t len, uint8_t packetNum) {
    uint8_t expected;

    /*
     * Sequence check: first packet must be 1; subsequent packets increment by 1
     * wrapping from 255 back to 1 (0 is reserved as "none received yet").
     */
    if (session.lastPacketNum == 0) {
        expected = 1;
    }
    else if (session.lastPacketNum == 255) {
        expected = 1;
    }
    else {
        expected = session.lastPacketNum + 1;
    }

    if (packetNum != expected) {
        return FILERX_ERR_SEQ_MISMATCH;
    }

    if (session.bytesReceived + len > FILERX_MAX_FILE_SIZE) {
        return FILERX_ERR_BAD_PAYLOAD;
    }

    session.runningCrc    = crc16_ccitt_stream_update(chunk, len, session.runningCrc);
    session.bytesReceived += len;
    session.lastPacketNum  = packetNum;

    XP_LT_BLUE;	// colour for File TX operations
    xprintf("FileTX: Received packet %d (%d bytes)\n", packetNum, len);
    XP_WHITE;

    return FILERX_OK;
}

/**
 * Finalise the transfer after FILE_END is received.
 *
 * Computes the final CRC and compares it to receivedCrc. Sets deleteOnClose
 * if the CRC does not match.
 *
 * The caller should always send CLOSE_FILE to fatfs_task regardless of the
 * return value; fileRx_shouldDelete() indicates whether to also delete the file.
 *
 * @param receivedCrc  CRC16-CCITT of the entire file as reported by the sender (LE)
 * @return FILERX_OK on CRC match; FILERX_ERR_CRC_MISMATCH otherwise
 */
fileRx_result_t fileRx_end(uint16_t receivedCrc) {
    uint16_t finalCrc = crc16_ccitt_stream_final(session.runningCrc);


    if (finalCrc != receivedCrc) {
        session.deleteOnClose = true;

        XP_LT_BLUE;	// colour for File TX operations
        xprintf("CRC error receiving '%s')\n", session.fileName);
        XP_WHITE;

        return FILERX_ERR_CRC_MISMATCH;
    }

    XP_LT_BLUE;
    xprintf("FileTX: Received '%s' OK (%d packets, %d bytes, CRC 0x%04x)\n",
    		session.fileName, session.lastPacketNum, session.totalSize, receivedCrc);
    XP_WHITE;

    return FILERX_OK;
}

/**
 * Abort the current session (e.g. on BLE disconnect or unrecoverable error).
 *
 * Sets the deleteOnClose flag so the caller will delete the partial file when
 * it sends CLOSE_FILE to fatfs_task. Does not reset the filename — the caller
 * may still need it for CLOSE_FILE.
 */
void fileRx_abort(void) {
    session.deleteOnClose = true;
}

/**
 * Returns the bare filename for the current session (e.g. "FOO.BIN").
 *
 * The caller passes this to fatfs_task via fileOperation_t.fileName; fatfs_task
 * resolves the directory via f_chdir(dirManager->current_config_dir).
 *
 * @return Pointer to the null-terminated filename; valid from fileRx_start()
 *         until the next fileRx_start() call
 */
const char *fileRx_getFileName(void) {
    return session.fileName;
}

/**
 * Returns true if the file should be deleted when closed.
 *
 * Set by fileRx_end() on CRC mismatch, or by fileRx_abort() on error or disconnect.
 *
 * @return true if the caller should delete the file after closing it
 */
bool fileRx_shouldDelete(void) {
    return session.deleteOnClose;
}
