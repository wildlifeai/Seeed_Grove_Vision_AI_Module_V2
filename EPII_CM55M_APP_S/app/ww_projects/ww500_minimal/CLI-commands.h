/*
 * CLI-commands.h
 *
 *  Created on: 25 Jul 2022
 *      Author: CGP
 *
 *  A command line interface (CLI) on the console UART, for use in debugging and measurement.
 *  Uses FreeRTOS+CLI. See:
 *    https://www.freertos.org/Documentation/03-Libraries/02-FreeRTOS-plus/03-FreeRTOS-plus-CLI/01-FreeRTOS-plus-CLI
 *
 *  Collects together the CLI FreeRTOS task, the UART receive callback and the commands.
 *  Type 'help' at the console for a list of the commands.
 *
 *  Derived from ww500_md/CLI-commands.c, keeping only the commands that make sense in a build
 *  that has no camera, SD card or BLE processor.
 */

#ifndef CLI_COMMANDS_H_
#define CLI_COMMANDS_H_

/*********************************************** Includes ****************************************************/

#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ww500_minimal.h"

/********************************************** Global Defines ***********************************************/

#define CLI_CMD_LINE_BUF_SIZE       80
#define CLI_OUTPUT_BUF_SIZE         configCOMMAND_INT_MAX_OUTPUT_SIZE

/*********************************************** Global Types ************************************************/

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates the CLI task and its queue. Call before the scheduler is started.
 */
TaskHandle_t cli_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason);

/**
 * @brief Returns the internal state as a number (this task has no states).
 */
uint16_t cli_getState(void);

/**
 * @brief Returns the internal state as a string (this task has no states).
 */
const char * cli_getStateString(void);

/**
 * @brief Safely appends formatted text to a CLI output buffer.
 */
BaseType_t cli_append(char **buf, size_t *len, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* CLI_COMMANDS_H_ */
