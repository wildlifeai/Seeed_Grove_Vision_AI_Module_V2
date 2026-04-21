/*
 * fileRx.c
 *
 *  Created on: 21 Apr 2026
 *      Author: CGP / Claude
 *
 * Manages the HX6538 side of a generic file-receive session.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "fileRx.h"
#include "crc16_ccitt.h"

#define FILERX_PATH_PREFIX      "/MANIFEST/"
#define FILERX_PATH_PREFIX_LEN  10      // strlen("/MANIFEST/")
#define FILERX_MAX_FILENAME     12      // 8 + '.' + 3
#define FILERX_MAX_PATH         (FILERX_PATH_PREFIX_LEN + FILERX_MAX_FILENAME + 1)

typedef struct {
    char     filePath[FILERX_MAX_PATH];
    uint32_t totalSize;
    uint32_t bytesReceived;
    uint8_t  lastPacketNum;     // 0 means no packet received yet
    uint16_t runningCrc;
    bool     deleteOnClose;
    bool     active;
} fileRxSession_t;

static fileRxSession_t session;

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

fileRx_result_t fileRx_start(const char *filename, uint32_t totalSize)
{
    if (!validate_83_filename(filename)) {
        return FILERX_ERR_BAD_FILENAME;
    }

    memset(&session, 0, sizeof(session));

    /* Build "/MANIFEST/<filename>" */
    memcpy(session.filePath, FILERX_PATH_PREFIX, FILERX_PATH_PREFIX_LEN);
    strncpy(session.filePath + FILERX_PATH_PREFIX_LEN,
            filename,
            FILERX_MAX_FILENAME);
    session.filePath[FILERX_MAX_PATH - 1] = '\0';

    session.totalSize      = totalSize;
    session.bytesReceived  = 0;
    session.lastPacketNum  = 0;
    session.runningCrc     = crc16_ccitt_stream_init();
    session.deleteOnClose  = false;
    session.active         = true;

    return FILERX_OK;
}

fileRx_result_t fileRx_data(const uint8_t *chunk, uint16_t len, uint8_t packetNum)
{
    /*
     * Sequence check: first packet must be 1; subsequent packets increment by 1
     * wrapping from 255 back to 1 (0 is reserved as "none received yet").
     */
    uint8_t expected;
    if (session.lastPacketNum == 0) {
        expected = 1;
    } else if (session.lastPacketNum == 255) {
        expected = 1;
    } else {
        expected = session.lastPacketNum + 1;
    }

    if (packetNum != expected) {
        return FILERX_ERR_SEQ_MISMATCH;
    }

    session.runningCrc    = crc16_ccitt_stream_update(chunk, len, session.runningCrc);
    session.bytesReceived += len;
    session.lastPacketNum  = packetNum;

    return FILERX_OK;
}

fileRx_result_t fileRx_end(uint16_t receivedCrc)
{
    uint16_t finalCrc = crc16_ccitt_stream_final(session.runningCrc);

    if (finalCrc != receivedCrc) {
        session.deleteOnClose = true;
        return FILERX_ERR_CRC_MISMATCH;
    }

    return FILERX_OK;
}

void fileRx_abort(void)
{
    session.deleteOnClose = true;
}

const char *fileRx_getFilePath(void)
{
    return session.filePath;
}

bool fileRx_shouldDelete(void)
{
    return session.deleteOnClose;
}
