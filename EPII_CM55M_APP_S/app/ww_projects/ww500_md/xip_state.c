/*
 * xip_state.c
 *
 *  Created on: 28 Oct 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  Saved state in the XIP flash: records that survive DPD and power cycles without the
 *  SD card. See xip_state.h for the layout.
 *
 *  All flash access goes through xip_state_area_read/write/erase() (xip_manager.c),
 *  which cannot reach outside the saved-state area. Reads use SPI commands, not
 *  the XIP window, so they never see stale cached data after a write.
 */

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "xprintf.h"

#include "crc16_ccitt.h"
#include "fatfs_task.h"		// op_parameter[] get/set
#include "cis_file.h"		// the staged extra camera registers
#include "ae.h"			// AE_GAIN_CODE_MAX
#include "camera_switch.h"	// cameraSwitch_thisVariant(): which image wrote the registers
#include "xip_manager.h"
#include "xip_state.h"

/*********************************************** Local Defines **********************************************/

#define SECTOR_SIZE			4096
#define SLOTS_PER_SECTOR	(SECTOR_SIZE / XIP_STATE_RECORD_SIZE)	// 16
#define RECORD_MAGIC		0x53585757u		// "WWXS" in flash (little-endian)
#define ERASED_WORD			0xFFFFFFFFu

#define FAST_START_NUM_OPS	23			// entries in fastStartOps[]
// The RP3 exposure counts as changed only when the total light (exposure x gain) moves more than
// this: in steady light the settled exposure wanders about 15 % (bench, 28 Oct 2026)
#define FAST_START_AE_TOLERANCE_PERCENT	25

/*********************************************** Local Types ************************************************/

typedef struct {
	uint32_t magic;					// RECORD_MAGIC
	uint8_t  version;				// XIP_STATE_FORMAT_VERSION
	uint8_t  type;					// XIP_STATE_TYPE_E
	uint16_t dataLen;				// payload bytes used
	uint32_t sequence;				// one more than the newest valid record when written
	uint8_t  data[XIP_STATE_DATA_MAX];
	uint16_t crc;					// CRC-16/CCITT of everything above
	uint16_t reserved;				// 0
} __attribute__((aligned(4))) XipStateRecord_t;

_Static_assert(sizeof(XipStateRecord_t) == XIP_STATE_RECORD_SIZE, "record must be one flash page");

// Working memory for one operation, from the heap (too big for a task stack)
typedef struct {
	XipStateRecord_t slot[SLOTS_PER_SECTOR];	// a copy of the whole sector
	XipStateRecord_t record;					// the record being written, then read back
} XipStateWork_t;

// The result of reading a sector
typedef struct {
	int8_t   newest;		// slot of the newest valid record, -1 if none
	uint32_t newestSeq;		// its sequence number
	uint8_t  nextFree;		// slot after the last used one (SLOTS_PER_SECTOR if full)
} SectorScan_t;

// One extra camera register write, as in the staged table (cis_file.c)
typedef struct {
	uint16_t addr;			// register address
	uint8_t  value;			// value to write
	uint8_t  action;		// HX_CIS_I2C_ActionType_e
} FastStartReg_t;

// The payload of a fast-start record. Change it only with XIP_STATE_FORMAT_VERSION.
typedef struct {
	uint16_t op[FAST_START_NUM_OPS];			// op_parameter[fastStartOps[i]]
	uint8_t  regCount;							// extra camera registers in reg[], 0 = none
	uint8_t  regCamera;							// XIP_SLOT_VARIANT_x of the image that wrote reg[]:
												// each image has its own register file and sensor
	FastStartReg_t reg[CIS_FILE_MAX_STAGED];	// unused entries are 0
} FastStartPayload_t;

_Static_assert(sizeof(FastStartPayload_t) <= XIP_STATE_DATA_MAX, "fast-start payload too big for a record");

// xip_state_test()'s buffers, from the heap too: the CLI task's stack is small
typedef struct {
	XipStateWork_t work;
	uint8_t saved[XIP_STATE_DATA_MAX];		// the record that was there before the test
	uint8_t expect[XIP_STATE_DATA_MAX];		// what the newest record should hold
	uint8_t got[XIP_STATE_DATA_MAX];		// what it does hold
} TestBuffers_t;

/*********************************************** Local Variables ********************************************/

// The op parameters in a fast-start record: those that affect the first photo, and the light
// state (25, 37, 38). Not the counters, GPS or deployment ID, which are only needed with the card.
static const uint8_t fastStartOps[FAST_START_NUM_OPS] = {
	OP_PARAMETER_NUM_PICTURES,				// 5
	OP_PARAMETER_PICTURE_INTERVAL,			// 6
	OP_PARAMETER_LED_BRIGHTNESS_PERCENT,	// 9
	OP_PARAMETER_CAMERA_ENABLED,			// 10
	OP_PARAMETER_MD_INTERVAL,				// 11
	OP_PARAMETER_FLASH_DURATION,			// 12
	OP_PARAMETER_FLASH_LED,					// 13
	OP_PARAMETER_MODEL_PROJECT,				// 14
	OP_PARAMETER_MODEL_VERSION,				// 15
	OP_PARAMETER_MODEL_THRESHOLD,			// 16
	OP_PARAMETER_MD_SENSITIVITY,			// 17
	OP_PARAMETER_TEST_MODE_BITS,			// 18
	OP_PARAMETER_AE_FLASH_STATE,			// 25
	OP_PARAMETER_WB_RED_GAIN,				// 27
	OP_PARAMETER_WB_BLUE_GAIN,				// 28
	OP_PARAMETER_CAM_AE_ENABLE,				// 29
	OP_PARAMETER_CAM_AE_TARGET,				// 30
	OP_PARAMETER_CAM_WB_MODE,				// 31
	OP_PARAMETER_FLASH_MODE,				// 34
	OP_PARAMETER_FLASH_TOD_START,			// 35
	OP_PARAMETER_FLASH_TOD_DURATION,		// 36
	OP_PARAMETER_CAM_AE_EXPOSURE,			// 37
	OP_PARAMETER_CAM_AE_GAIN,				// 38
};

// The fast-start record read at boot, kept to compare with CONFIG.TXT
static FastStartPayload_t bootPayload;
static bool bootPayloadValid = false;
static uint32_t bootSequence = 0;


/*********************************************** Local Function Declarations *********************************/

static uint32_t sectorAddr(XIP_STATE_TYPE_E type);
static bool slotIsBlank(const XipStateRecord_t *rec);
static uint16_t recordCrc(const XipStateRecord_t *rec);
static bool recordIsValid(const XipStateRecord_t *rec, XIP_STATE_TYPE_E type);
static int scanSector(XIP_STATE_TYPE_E type, XipStateWork_t *work, SectorScan_t *scan);
static int writeRecord(XIP_STATE_TYPE_E type, const uint8_t *data, uint16_t len, bool badCrc,
		XipStateWork_t *work, uint8_t *slotUsed, bool *erased);
static bool readNewest(XIP_STATE_TYPE_E type, XipStateWork_t *work, uint8_t *data, uint16_t *len, uint32_t *seq);
static void testPayload(uint16_t n, uint8_t *data, uint16_t *len);
static void buildPayload(FastStartPayload_t *payload);
static bool aeSimilar(uint16_t expA, uint16_t gainA, uint16_t expB, uint16_t gainB);
static bool payloadsMatch(const FastStartPayload_t *a, const FastStartPayload_t *b);

/*********************************************** Local Function Definitions *********************************/

/**
 * @brief Physical address of the sector that holds a record type.
 *
 * @param type  the record type
 * @return the sector's physical flash address
 */
static uint32_t sectorAddr(XIP_STATE_TYPE_E type) {
	return XIP_STATE_AREA_ADDR + ((type == XIP_STATE_TYPE_BOARD) ? SECTOR_SIZE : 0);
}

/**
 * @brief Whether every byte of a slot is erased.
 *
 * A slot with any programmed byte counts as used, even if its record is not valid, so a
 * part-written record is never written over.
 *
 * @param rec  the slot's copy in RAM
 * @return true if all 256 bytes are 0xFF
 */
static bool slotIsBlank(const XipStateRecord_t *rec) {
	const uint32_t *w = (const uint32_t *)rec;

	for (uint16_t i = 0; i < (XIP_STATE_RECORD_SIZE / 4); i++) {
		if (w[i] != ERASED_WORD) {
			return false;
		}
	}
	return true;
}

/**
 * @brief CRC-16/CCITT of a record, up to (not including) its crc field.
 *
 * @param rec  the record
 * @return the CRC
 */
static uint16_t recordCrc(const XipStateRecord_t *rec) {
	uint16_t crc;

	crc16_ccitt_generate((uint8_t *)rec, (uint16_t)offsetof(XipStateRecord_t, crc), &crc);
	return crc;
}

/**
 * @brief Whether a record is valid: magic, format version, type, length and CRC all correct.
 *
 * @param rec   the record
 * @param type  the record type it should be
 * @return true if valid
 */
static bool recordIsValid(const XipStateRecord_t *rec, XIP_STATE_TYPE_E type) {
	return ((rec->magic == RECORD_MAGIC)
			&& (rec->version == XIP_STATE_FORMAT_VERSION)
			&& (rec->type == (uint8_t)type)
			&& (rec->dataLen <= XIP_STATE_DATA_MAX)
			&& (rec->crc == recordCrc(rec)));
}

/**
 * @brief Read a whole sector and find the newest valid record and the next free slot.
 *
 * @param type  the record type, which selects the sector
 * @param work  working memory: on return work->slot[] holds a copy of the sector
 * @param scan  output: the newest valid record and the next free slot
 * @return 0 on success, -1 if the flash could not be read
 */
static int scanSector(XIP_STATE_TYPE_E type, XipStateWork_t *work, SectorScan_t *scan) {
	scan->newest = -1;
	scan->newestSeq = 0;
	scan->nextFree = 0;

	if (xip_state_area_read(sectorAddr(type), (uint32_t *)work->slot, SECTOR_SIZE) != 0) {
		xprintf("xipstate: flash read failed\n");
		return -1;
	}

	for (uint8_t i = 0; i < SLOTS_PER_SECTOR; i++) {
		const XipStateRecord_t *rec = &work->slot[i];

		if (slotIsBlank(rec)) {
			continue;
		}
		scan->nextFree = (uint8_t)(i + 1);
		if (recordIsValid(rec, type) && ((scan->newest < 0) || (rec->sequence > scan->newestSeq))) {
			scan->newest = (int8_t)i;
			scan->newestSeq = rec->sequence;
		}
	}
	return 0;
}

/**
 * @brief Append one record, erasing the sector first if it is full, then read it back.
 *
 * The sequence number is one more than the newest valid record's (1 if there is none).
 *
 * @param type      the record type, which selects the sector
 * @param data      the payload
 * @param len       payload bytes, at most XIP_STATE_DATA_MAX
 * @param badCrc    true only in the test: write a record whose CRC is wrong
 * @param work      working memory
 * @param slotUsed  output: the slot written
 * @param erased    output: true if the sector was erased first
 * @return 0 on success; -1 too long, -2 read failed, -3 erase failed, -4 write failed,
 *         -5 read-back did not match
 */
static int writeRecord(XIP_STATE_TYPE_E type, const uint8_t *data, uint16_t len, bool badCrc,
		XipStateWork_t *work, uint8_t *slotUsed, bool *erased) {
	SectorScan_t scan;
	uint8_t slot;

	*erased = false;

	if (len > XIP_STATE_DATA_MAX) {
		return -1;
	}
	if (scanSector(type, work, &scan) != 0) {
		return -2;
	}

	slot = scan.nextFree;
	if (slot >= SLOTS_PER_SECTOR) {
		if (xip_state_area_erase(sectorAddr(type)) != 0) {
			xprintf("xipstate: sector erase failed\n");
			return -3;
		}
		*erased = true;
		slot = 0;
	}
	*slotUsed = slot;

	XipStateRecord_t *rec = &work->record;
	memset(rec, 0, sizeof(*rec));
	rec->magic = RECORD_MAGIC;
	rec->version = XIP_STATE_FORMAT_VERSION;
	rec->type = (uint8_t)type;
	rec->dataLen = len;
	rec->sequence = (scan.newest < 0) ? 1 : (scan.newestSeq + 1);
	memcpy(rec->data, data, len);
	rec->crc = recordCrc(rec);
	if (badCrc) {
		rec->crc ^= 0xFFFF;
	}

	uint32_t addr = sectorAddr(type) + ((uint32_t)slot * XIP_STATE_RECORD_SIZE);

	if (xip_state_area_write(addr, (uint32_t *)rec, XIP_STATE_RECORD_SIZE) != 0) {
		xprintf("xipstate: write failed at 0x%08x\n", (unsigned)addr);
		return -4;
	}

	// Read back into the slot copy and compare
	if ((xip_state_area_read(addr, (uint32_t *)&work->slot[slot], XIP_STATE_RECORD_SIZE) != 0)
			|| (memcmp(&work->slot[slot], rec, XIP_STATE_RECORD_SIZE) != 0)) {
		xprintf("xipstate: read-back failed at 0x%08x\n", (unsigned)addr);
		return -5;
	}
	return 0;
}

/**
 * @brief Read the newest valid record of a type.
 *
 * @param type  the record type, which selects the sector
 * @param work  working memory
 * @param data  output: the payload, XIP_STATE_DATA_MAX bytes
 * @param len   output: payload bytes
 * @param seq   output: the record's sequence number
 * @return true if there is a valid record; the outputs are unchanged if not
 */
static bool readNewest(XIP_STATE_TYPE_E type, XipStateWork_t *work, uint8_t *data, uint16_t *len, uint32_t *seq) {
	SectorScan_t scan;

	if ((scanSector(type, work, &scan) != 0) || (scan.newest < 0)) {
		return false;
	}

	const XipStateRecord_t *rec = &work->slot[scan.newest];

	memcpy(data, rec->data, rec->dataLen);
	*len = rec->dataLen;
	*seq = rec->sequence;
	return true;
}

/**
 * @brief Make a payload with a different length and pattern for each test write.
 *
 * @param n     the write number
 * @param data  output: the payload, XIP_STATE_DATA_MAX bytes
 * @param len   output: payload bytes, 1 to XIP_STATE_DATA_MAX
 */
static void testPayload(uint16_t n, uint8_t *data, uint16_t *len) {
	*len = (uint16_t)(1 + ((n * 37u) % XIP_STATE_DATA_MAX));
	for (uint16_t i = 0; i < *len; i++) {
		data[i] = (uint8_t)((n * 31u) + i);
	}
}

/**
 * @brief Fill a fast-start payload from op_parameter[] and the staged extra camera registers.
 *
 * The registers are labelled with this image's camera, since each image has its own.
 *
 * @param payload  output: the payload, unused bytes 0
 */
static void buildPayload(FastStartPayload_t *payload) {
	uint16_t count = 0;

	memset(payload, 0, sizeof(*payload));

	for (uint8_t i = 0; i < FAST_START_NUM_OPS; i++) {
		payload->op[i] = fatfs_getOperationalParameter((OP_PARAMETERS_E)fastStartOps[i]);
	}

	if (cis_file_isStagedLoaded()) {
		const HX_CIS_SensorSetting_t *table = cis_file_getStagedTable();

		count = cis_file_getStagedCount();
		if (count > CIS_FILE_MAX_STAGED) {
			count = CIS_FILE_MAX_STAGED;
		}
		for (uint16_t j = 0; j < count; j++) {
			payload->reg[j].addr = table[j].RegAddree;
			payload->reg[j].value = table[j].Value;
			payload->reg[j].action = (uint8_t)table[j].I2C_ActionType;
		}
	}
	payload->regCount = (uint8_t)count;
	payload->regCamera = cameraSwitch_thisVariant();
}

/**
 * @brief Whether two RP3 exposure and gain pairs give about the same total light.
 *
 * Compares exposure x analog gain (gain = 1024 / (1024 - code), as in ae.c), within
 * FAST_START_AE_TOLERANCE_PERCENT of the second pair. An exposure of 0 (none saved) matches
 * only another 0.
 *
 * @param expA   first exposure (lines)
 * @param gainA  first gain code
 * @param expB   second exposure (lines)
 * @param gainB  second gain code
 * @return true if within the tolerance
 */
static bool aeSimilar(uint16_t expA, uint16_t gainA, uint16_t expB, uint16_t gainB) {
	if ((expA == 0) || (expB == 0)) {
		return (expA == expB);
	}
	if ((gainA > AE_GAIN_CODE_MAX) || (gainB > AE_GAIN_CODE_MAX)) {
		return ((expA == expB) && (gainA == gainB));
	}

	uint32_t lightA = ((uint32_t)expA * 1024u) / (1024u - gainA);
	uint32_t lightB = ((uint32_t)expB * 1024u) / (1024u - gainB);
	uint32_t diff = (lightA > lightB) ? (lightA - lightB) : (lightB - lightA);

	return ((diff * 100u) <= (lightB * FAST_START_AE_TOLERANCE_PERCENT));
}

/**
 * @brief Whether two fast-start payloads match: every op and register equal, except the RP3
 * exposure and gain, which need only give about the same light (aeSimilar()).
 *
 * @param a  one payload
 * @param b  the other
 * @return true if they match
 */
static bool payloadsMatch(const FastStartPayload_t *a, const FastStartPayload_t *b) {
	uint16_t expA = 0;
	uint16_t gainA = 0;
	uint16_t expB = 0;
	uint16_t gainB = 0;

	for (uint8_t i = 0; i < FAST_START_NUM_OPS; i++) {
		if (fastStartOps[i] == OP_PARAMETER_CAM_AE_EXPOSURE) {
			expA = a->op[i];
			expB = b->op[i];
		}
		else if (fastStartOps[i] == OP_PARAMETER_CAM_AE_GAIN) {
			gainA = a->op[i];
			gainB = b->op[i];
		}
		else if (a->op[i] != b->op[i]) {
			return false;
		}
	}
	if ((a->regCamera != b->regCamera) || (a->regCount != b->regCount)
			|| (memcmp(a->reg, b->reg, (size_t)a->regCount * sizeof(FastStartReg_t)) != 0)) {
		return false;
	}
	return aeSimilar(expA, gainA, expB, gainB);
}

/*********************************************** Global Function Definitions *********************************/

/**
 * @brief Read the newest valid record of a type.
 *
 * @param type  the record type
 * @param data  output: the payload, XIP_STATE_DATA_MAX bytes
 * @param len   output: payload bytes
 * @return true if a valid record was found; false means use the C defaults
 */
bool xip_state_read(XIP_STATE_TYPE_E type, uint8_t *data, uint16_t *len) {
	uint32_t seq;
	bool found;

	XipStateWork_t *work = (XipStateWork_t *)pvPortMalloc(sizeof(XipStateWork_t));
	if (work == NULL) {
		xprintf("xipstate: out of memory\n");
		return false;
	}
	found = readNewest(type, work, data, len, &seq);
	vPortFree(work);
	return found;
}

/**
 * @brief Append a record, erasing the sector first if it is full, and check it by reading it back.
 *
 * Only XIP_STATE_TYPE_FAST_START is written for now: the board record is reserved for the
 * clock calibration work.
 *
 * @param type  the record type
 * @param data  the payload
 * @param len   payload bytes, at most XIP_STATE_DATA_MAX
 * @return 0 on success, negative on failure
 */
int xip_state_write(XIP_STATE_TYPE_E type, const uint8_t *data, uint16_t len) {
	uint8_t slot;
	bool erased;
	int ret;

	if (type != XIP_STATE_TYPE_FAST_START) {
		return -1;
	}

	XipStateWork_t *work = (XipStateWork_t *)pvPortMalloc(sizeof(XipStateWork_t));
	if (work == NULL) {
		xprintf("xipstate: out of memory\n");
		return -1;
	}
	ret = writeRecord(type, data, len, false, work, &slot, &erased);
	vPortFree(work);
	return ret;
}

/**
 * @brief Print every slot of both sectors on the console (the 'xipstate' command).
 *
 * One line per used slot (valid or not, sequence, type, length, first data bytes), one line
 * for each run of blank slots, and a note when a sector has no valid record. For the
 * fast-start sector, also the op values in the newest record.
 */
void xip_state_dump(void) {
	static const XIP_STATE_TYPE_E types[] = { XIP_STATE_TYPE_FAST_START, XIP_STATE_TYPE_BOARD };
	SectorScan_t scan;

	XipStateWork_t *work = (XipStateWork_t *)pvPortMalloc(sizeof(XipStateWork_t));
	if (work == NULL) {
		xprintf("xipstate: out of memory\n");
		return;
	}

	for (uint8_t t = 0; t < (sizeof(types) / sizeof(types[0])); t++) {
		XIP_STATE_TYPE_E type = types[t];

		if (scanSector(type, work, &scan) != 0) {
			break;
		}
		xprintf("%s sector 0x%08x (format version %d):\n",
				(type == XIP_STATE_TYPE_BOARD) ? "Board" : "Fast-start",
				(unsigned)sectorAddr(type), XIP_STATE_FORMAT_VERSION);

		for (uint8_t i = 0; i < SLOTS_PER_SECTOR; i++) {
			const XipStateRecord_t *rec = &work->slot[i];

			if (slotIsBlank(rec)) {
				// One line for the rest: once a slot is blank, every later slot normally is too
				uint8_t j = i;
				while ((j + 1 < SLOTS_PER_SECTOR) && slotIsBlank(&work->slot[j + 1])) {
					j++;
				}
				if (j == i) {
					xprintf("  slot %2d: blank\n", i);
				}
				else {
					xprintf("  slots %d-%d: blank\n", i, j);
				}
				i = j;
				continue;
			}
			xprintf("  slot %2d: %s seq %lu version %d type %d len %d data %02x %02x %02x %02x%s\n",
					i,
					recordIsValid(rec, type) ? "valid" : "NOT VALID",
					(unsigned long)rec->sequence, rec->version, rec->type, rec->dataLen,
					rec->data[0], rec->data[1], rec->data[2], rec->data[3],
					(i == scan.newest) ? "  <- newest" : "");
		}
		if (scan.newest < 0) {
			xprintf("  No valid record: the C defaults are used\n");
		}
		else if ((type == XIP_STATE_TYPE_FAST_START)
				&& (work->slot[scan.newest].dataLen == sizeof(FastStartPayload_t))) {
			const FastStartPayload_t *payload = (const FastStartPayload_t *)work->slot[scan.newest].data;

			// The newest record's values, as "op=value", for comparing with 'getop'
			xprintf("  Newest:");
			for (uint8_t k = 0; k < FAST_START_NUM_OPS; k++) {
				xprintf(" %d=%d", fastStartOps[k], payload->op[k]);
			}
			xprintf(", %d extra camera registers (camera %d)\n", payload->regCount, payload->regCamera);
		}
	}
	vPortFree(work);
}

/**
 * @brief Bench test of the fast-start sector (the 'xipstate test' command).
 *
 * Fills the sector twice over (33 writes, 2 erases), checking every write by reading it back
 * and that each erase comes at the right count; then checks that a record with a bad CRC is
 * skipped and that an erased sector reads as "no record". Finally writes back the record that
 * was there before the test, if any. Prints one line per erase and PASS or FAIL.
 *
 * Call only while the NN is not running (see xip_state.h).
 *
 * @return true if every check passed
 */
bool xip_state_test(void) {
	const XIP_STATE_TYPE_E type = XIP_STATE_TYPE_FAST_START;
	const uint16_t numWrites = (2 * SLOTS_PER_SECTOR) + 1;	// fill, erase, fill, erase, one more

	uint16_t savedLen = 0;
	uint16_t expectLen;
	uint16_t gotLen;
	uint32_t gotSeq;
	uint32_t lastSeq;
	uint8_t slot;
	bool erased;
	bool haveSaved;
	bool pass = true;
	uint16_t writes = 0;
	uint16_t erases = 0;

	TestBuffers_t *b = (TestBuffers_t *)pvPortMalloc(sizeof(TestBuffers_t));
	if (b == NULL) {
		xprintf("xipstate test: out of memory\n");
		return false;
	}

	// Keep the real record, to put back at the end
	haveSaved = readNewest(type, &b->work, b->saved, &savedLen, &gotSeq);
	xprintf("xipstate test: %s\n", haveSaved ? "saved the current record" : "no record before the test");

	// Start from an erased sector, so the erases come at known counts
	if (xip_state_area_erase(sectorAddr(type)) != 0) {
		xprintf("xipstate test: FAIL, first erase\n");
		vPortFree(b);
		return false;
	}
	erases++;
	xprintf("Erase %d: at the start\n", erases);
	lastSeq = 0;

	// Fill the sector twice over, checking every write
	for (uint16_t n = 1; (n <= numWrites) && pass; n++) {
		bool expectErase = (n > 1) && (((n - 1) % SLOTS_PER_SECTOR) == 0);
		uint8_t expectSlot = (uint8_t)((n - 1) % SLOTS_PER_SECTOR);

		testPayload(n, b->expect, &expectLen);
		if (writeRecord(type, b->expect, expectLen, false, &b->work, &slot, &erased) != 0) {
			xprintf("xipstate test: FAIL, write %d\n", n);
			pass = false;
			break;
		}
		writes++;
		if (erased) {
			erases++;
			xprintf("Erase %d: before write %d (sector full)\n", erases, n);
		}
		if ((erased != expectErase) || (slot != expectSlot)) {
			xprintf("xipstate test: FAIL, write %d went to slot %d (expected %d), erased %d (expected %d)\n",
					n, slot, expectSlot, erased, expectErase);
			pass = false;
		}
		else if (!readNewest(type, &b->work, b->got, &gotLen, &gotSeq)
				|| (gotLen != expectLen) || (memcmp(b->got, b->expect, expectLen) != 0)
				|| (gotSeq != lastSeq + 1)) {
			xprintf("xipstate test: FAIL, write %d not read back (len %d, seq %lu)\n",
					n, gotLen, (unsigned long)gotSeq);
			pass = false;
		}
		lastSeq = gotSeq;
	}

	// A record with a bad CRC must be skipped: the reader returns the one before it
	if (pass) {
		testPayload(numWrites + 1, b->got, &gotLen);
		if (writeRecord(type, b->got, gotLen, true, &b->work, &slot, &erased) != 0) {
			xprintf("xipstate test: FAIL, bad-CRC write\n");
			pass = false;
		}
		else {
			writes++;
			if (!readNewest(type, &b->work, b->got, &gotLen, &gotSeq)
					|| (gotLen != expectLen) || (memcmp(b->got, b->expect, expectLen) != 0)) {
				xprintf("xipstate test: FAIL, bad-CRC record not skipped\n");
				pass = false;
			}
			else {
				xprintf("Bad-CRC record in slot %d skipped: OK\n", slot);
			}
		}
	}

	// An erased sector must read as "no record", so the C defaults are used
	if (pass) {
		if (xip_state_area_erase(sectorAddr(type)) != 0) {
			xprintf("xipstate test: FAIL, erase\n");
			pass = false;
		}
		else {
			erases++;
			xprintf("Erase %d: to check the fall-back\n", erases);
			if (readNewest(type, &b->work, b->got, &gotLen, &gotSeq)) {
				xprintf("xipstate test: FAIL, erased sector still has a record\n");
				pass = false;
			}
			else {
				xprintf("Erased sector: no record, the C defaults would be used: OK\n");
			}
		}
	}

	// Put back the record that was there before. A failed test may have left the sector part
	// written: the restored record is newest anyway, since it is written last
	if (haveSaved) {
		if ((writeRecord(type, b->saved, savedLen, false, &b->work, &slot, &erased) != 0)
				|| !readNewest(type, &b->work, b->got, &gotLen, &gotSeq)
				|| (gotLen != savedLen) || (memcmp(b->got, b->saved, savedLen) != 0)) {
			xprintf("xipstate test: FAIL, the saved record was not written back\n");
			pass = false;
		}
		else {
			writes++;
			if (erased) {
				erases++;
			}
			xprintf("Saved record written back\n");
		}
	}

	xprintf("xipstate test: %s (%d writes, %d erases)\n", pass ? "PASS" : "FAIL", writes, erases);
	vPortFree(b);
	return pass;
}

/**
 * @brief At boot: fill op_parameter[] from the fast-start record, if there is one.
 *
 * Call at the start of the FatFS task, before CONFIG.TXT is loaded (which then overrides
 * these values, as before). Keeps a copy for xip_state_compareFastStart(). Values that are
 * not valid for their op (fatfs_isValidOperationalParameter()) are skipped. The extra camera
 * registers are kept for the comparison only: the staged table is still loaded from the card.
 *
 * @return true if a record was found and used; false means the C defaults stay
 */
bool xip_state_loadFastStart(void) {
	uint16_t len = 0;
	uint32_t seq = 0;
	bool found;

	XipStateWork_t *work = (XipStateWork_t *)pvPortMalloc(sizeof(XipStateWork_t));
	if (work == NULL) {
		xprintf("xipstate: out of memory\n");
		return false;
	}

	// work->record is free here, so its data field holds the payload
	found = readNewest(XIP_STATE_TYPE_FAST_START, work, work->record.data, &len, &seq);
	if (found && (len == sizeof(FastStartPayload_t))) {
		memcpy(&bootPayload, work->record.data, sizeof(bootPayload));
		bootPayloadValid = true;
		bootSequence = seq;
	}
	vPortFree(work);

	if (!bootPayloadValid) {
		xprintf("Fast-start record: none, using the C defaults\n");
		return false;
	}

	for (uint8_t i = 0; i < FAST_START_NUM_OPS; i++) {
		OP_PARAMETERS_E op = (OP_PARAMETERS_E)fastStartOps[i];

		if (fatfs_isValidOperationalParameter(op, bootPayload.op[i])) {
			fatfs_setOperationalParameter(op, (int16_t)bootPayload.op[i]);
		}
	}
	xprintf("Fast-start record: seq %lu loaded (%d ops, %d extra camera registers)\n",
			(unsigned long)bootSequence, FAST_START_NUM_OPS, bootPayload.regCount);
	return true;
}

/**
 * @brief After CONFIG.TXT has loaded: print any difference from the record read at boot.
 *
 * Call once the card's settings and the staged extra camera registers have loaded. Prints
 * one line if they match, otherwise one line per differing op (the RP3 exposure and gain only
 * if outside the tolerance) and one for the extra registers. Changes nothing: CONFIG.TXT has
 * already overridden the record.
 */
void xip_state_compareFastStart(void) {
	static FastStartPayload_t card;		// static: too big for the FatFS task's stack

	if (!bootPayloadValid) {
		xprintf("Fast-start record: none at boot, nothing to compare\n");
		return;
	}

	buildPayload(&card);
	if (payloadsMatch(&bootPayload, &card)) {
		xprintf("Fast-start record: seq %lu matches CONFIG.TXT\n", (unsigned long)bootSequence);
		return;
	}

	xprintf("Fast-start record: seq %lu differs from CONFIG.TXT:\n", (unsigned long)bootSequence);

	// The RP3 exposure and gain are listed only if outside the tolerance: the record keeps the
	// last value that moved more than that, so small differences from the card are expected
	uint16_t recExp = 0;
	uint16_t recGain = 0;
	uint16_t cardExp = 0;
	uint16_t cardGain = 0;

	for (uint8_t i = 0; i < FAST_START_NUM_OPS; i++) {
		if (fastStartOps[i] == OP_PARAMETER_CAM_AE_EXPOSURE) {
			recExp = bootPayload.op[i];
			cardExp = card.op[i];
		}
		else if (fastStartOps[i] == OP_PARAMETER_CAM_AE_GAIN) {
			recGain = bootPayload.op[i];
			cardGain = card.op[i];
		}
	}
	bool aeListed = !aeSimilar(recExp, recGain, cardExp, cardGain);

	for (uint8_t i = 0; i < FAST_START_NUM_OPS; i++) {
		bool isAe = (fastStartOps[i] == OP_PARAMETER_CAM_AE_EXPOSURE) || (fastStartOps[i] == OP_PARAMETER_CAM_AE_GAIN);

		if ((bootPayload.op[i] != card.op[i]) && (!isAe || aeListed)) {
			xprintf("  op %d: record %d, CONFIG.TXT %d\n", fastStartOps[i], bootPayload.op[i], card.op[i]);
		}
	}
	if (bootPayload.regCamera != card.regCamera) {
		xprintf("  extra camera registers: the record's are from the other camera image\n");
	}
	else if ((bootPayload.regCount != card.regCount)
			|| (memcmp(bootPayload.reg, card.reg, (size_t)card.regCount * sizeof(FastStartReg_t)) != 0)) {
		xprintf("  extra camera registers: record %d, card %d (or their values differ)\n",
				bootPayload.regCount, card.regCount);
	}
}

/**
 * @brief At sleep: write a new fast-start record if anything has changed.
 *
 * Call from the FatFS task's save-state handler, when the NN is not running. Compares the
 * current settings with the newest record (payloadsMatch()) and writes only if they differ,
 * so a steady camera writes rarely.
 *
 * @return 0 if written or unchanged, negative on failure
 */
int xip_state_saveFastStart(void) {
	static FastStartPayload_t now;		// static: too big for the FatFS task's stack
	uint16_t len = 0;
	uint32_t seq = 0;
	uint8_t slot = 0;
	bool erased = false;
	bool found;
	int ret;

	buildPayload(&now);

	XipStateWork_t *work = (XipStateWork_t *)pvPortMalloc(sizeof(XipStateWork_t));
	if (work == NULL) {
		xprintf("xipstate: out of memory\n");
		return -1;
	}

	// work->record is free until writeRecord(), so its data field holds the newest payload
	found = readNewest(XIP_STATE_TYPE_FAST_START, work, work->record.data, &len, &seq);
	if (found && (len == sizeof(FastStartPayload_t))
			&& payloadsMatch(&now, (const FastStartPayload_t *)work->record.data)) {
		vPortFree(work);
		xprintf("Fast-start record: unchanged (seq %lu)\n", (unsigned long)seq);
		return 0;
	}

	ret = writeRecord(XIP_STATE_TYPE_FAST_START, (const uint8_t *)&now, sizeof(now), false, work, &slot, &erased);
	if (ret == 0) {
		xprintf("Fast-start record: written, seq %lu slot %d%s\n",
				(unsigned long)work->slot[slot].sequence, slot, erased ? " (sector erased first)" : "");
	}
	else {
		xprintf("Fast-start record: write failed (%d)\n", ret);
	}
	vPortFree(work);
	return ret;
}
