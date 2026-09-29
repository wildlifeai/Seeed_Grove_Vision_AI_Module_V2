/*
 * printf_x.c
 *
 *  Created on: 29 Jul 2024
 *      Author: charl
 *
 *  Buffer dump utility. The colour macros are in printf_x.h.
 */

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <string.h>
#include "ctype.h"

#include "printf_x.h"
#include "xprintf.h"

/*********************************************** Local Defines ***********************************************/

#define IN_LINE_PRINT_CNT   (16u)   //!< Number of data bytes printed in a single line.

/********************************************** Local Variables **********************************************/

/**************************************** Local Function Declarations ****************************************/

/**************************************** Local Function Definitions *****************************************/

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Prints the contents of a buffer.
 *
 * Prints 16 bytes per line with a space after 8. Each line has the offset before the bytes
 * and the ASCII version afterwards.
 *
 * @param buff   Pointer to the buffer.
 * @param length Number of bytes to print.
 */
void printf_x_printBuffer(const void *buff, size_t length) {
	const uint8_t *src = (const uint8_t *)buff;
	uint8_t lineBuff[IN_LINE_PRINT_CNT + 1];   // +1 for '\0'
	size_t remaining;
	size_t bytesThisLine;

	for (size_t addr = 0; addr < length; addr += IN_LINE_PRINT_CNT)  {
		remaining = length - addr;
		bytesThisLine = (remaining < IN_LINE_PRINT_CNT) ? remaining : IN_LINE_PRINT_CNT;

		// Copy only the valid bytes
		memcpy(lineBuff, src + addr, bytesThisLine);

		// Pad the rest so the ASCII print section is well-defined
		if (bytesThisLine < IN_LINE_PRINT_CNT) {
			memset(lineBuff + bytesThisLine, ' ', IN_LINE_PRINT_CNT - bytesThisLine);
		}

		lineBuff[IN_LINE_PRINT_CNT] = '\0';

		xprintf("%03x: ", (unsigned)addr);

		for (uint8_t i = 0; i < IN_LINE_PRINT_CNT; i++) {
			if (i == IN_LINE_PRINT_CNT / 2) {
				xprintf(" ");
			}

			if (i < bytesThisLine)  {
				xprintf("%02x ", lineBuff[i]);

				if (!isprint((int)lineBuff[i])) {
					lineBuff[i] = '.';
				}
			}
			else  {
				xprintf("   ");
			}
		}

		xprintf("%s\n", lineBuff);
	}
}
