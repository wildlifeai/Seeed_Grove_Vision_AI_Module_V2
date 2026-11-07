/*
 * selfTest.c
 *
 *  Created on: 18 Nov 2025
 *      Author: charl
 */


/***************************************** Includes ***************************************/

#include <stdint.h>
#include <string.h>

#include "selfTest.h"
#include "xprintf.h"

/***************************************** Defines *******************************************************/


/******************************** Private Function Declarations ***************************************************/

/******************************** Local Variables ****************************************************/

static uint16_t errorBits = 0;

// #290: a 'selftest' reply can go before the FatFS task has mounted the card (warm boot: "Wake" goes first), so
// bit 11 is not known yet. checksDone is set when the start-up checks are over (selfTest_setChecksDone());
// provisionalReplySent says a reply went before that, with bit 15 set, so a final one must follow
static volatile bool checksDone = false;
static volatile bool provisionalReplySent = false;

/******************************** Private Function Definitions ****************************************************/

/******************************** Public Function Definitions ************************************************************/

/**
 * initialises the bit mask
 *
 * Clears all error bits
 */
void selfTest_init(void) {
	errorBits = 0;
}

/**
 * Returns the error bits
 *
 * @return error bits
 */
uint16_t selfTest_getErrorBits(void) {
	return errorBits;
}

/**
 * Sets one or more error bits
 *
 * @param error bits from selfTest_type_t
 */
void selfTest_setErrorBits(uint16_t errBits) {
	errorBits |= errBits;
}

/**
 * Clears one or more error bits
 *
 * @param error bit from selfTest_type_t
 */
void selfTest_clearErrorBits(uint16_t errBits) {
	errorBits &= ~errBits;
}

/**
 * Run a few tests to give confidence
 */
void selfTest_test(void) {

	xprintf("Testing selfTest\n");

	selfTest_setErrorBits(1 << SELF_TEST_LOW_BATTERY);
	xprintf("Error bits = 0x%04x (expect 0x0001)\n", selfTest_getErrorBits());

	selfTest_setErrorBits((1 << SELF_TEST_AI_PDM_ERROR) | (1 << SELF_TEST_AI_NO_SD_CARD)) ;
	xprintf("Error bits = 0x%04x (expect 0x1801)\n", selfTest_getErrorBits());

	selfTest_clearErrorBits(1 << SELF_TEST_LOW_BATTERY);
	xprintf("Error bits = 0x%04x (expect 0x1800)\n", selfTest_getErrorBits());

	selfTest_clearErrorBits(1 << SELF_TEST_AI_PDM_ERROR);
	xprintf("Error bits = 0x%04x (expect 0x0800)\n", selfTest_getErrorBits());

	selfTest_clearErrorBits(1 << SELF_TEST_AI_NO_SD_CARD);
	xprintf("Error bits = 0x%04x (expect 0x0000)\n", selfTest_getErrorBits());
}

/**
 * The bits to send in a 'selfTest' reply, solicited or not
 *
 * Before the start-up checks are done the reply is provisional: bit 15 (SELF_TEST_AI_PROVISIONAL) is set, and
 * selfTest_setChecksDone() will ask for a final one. Bit 14 (SELF_TEST_AI_NO_BLE) is cleared only once a final
 * reply has carried it (#246), as the BLE processor replaces all its AI bits with each reply.
 *
 * provisionalReplySent is set before checksDone is read, and selfTest_setChecksDone() sets checksDone before
 * reading provisionalReplySent, so whichever runs second sees the other: a provisional reply is never left
 * without a final one. At worst two final replies go.
 *
 * @return the bits for the reply
 */
uint16_t selfTest_getReplyBits(void) {
	uint16_t bits;

	provisionalReplySent = true;

	if (checksDone) {
		provisionalReplySent = false;
		bits = errorBits;
		errorBits &= ~(1 << SELF_TEST_AI_NO_BLE);
	}
	else {
		bits = errorBits | (1 << SELF_TEST_AI_PROVISIONAL);
	}

	return bits;
}

/**
 * Records that the FatFS task's start-up checks are over (the SD card is mounted, or the mount failed)
 *
 * Call once, from the FatFS task, after setting any bit those checks found.
 *
 * @return true if a provisional reply has gone out, so the caller must have a final 'selfTest' sent
 */
bool selfTest_setChecksDone(void) {
	checksDone = true;
	return provisionalReplySent;
}

