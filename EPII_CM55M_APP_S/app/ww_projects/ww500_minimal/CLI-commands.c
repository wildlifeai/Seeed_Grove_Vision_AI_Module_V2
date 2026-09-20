/*
 * CLI-commands.c
 *
 *  Created on: 25 Jul 2022
 *      Author: CGP
 *
 *  The CLI task and its commands. See CLI-commands.h.
 *
 *  How it works:
 *   (1) The task opens the console UART and enables its receive interrupt, one character at a time.
 *   (2) The UART callback vCmdLineTask_cb() sends a message to the task's queue for each character.
 *   (3) The task accumulates characters and, on a newline, calls FreeRTOS_CLIProcessCommand().
 *
 *  Any character typed extends the inactivity period, so that the device does not enter DPD
 *  while you are using the console.
 */

/*********************************************** Includes ****************************************************/

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* FreeRTOS+CLI includes. */
#include "FreeRTOS_CLI.h"

// Himax board UART driver
#include "hx_drv_uart.h"

#include "printf_x.h"
#include "xprintf.h"

#include "app_msg.h"
#include "barrier.h"
#include "blinky_task.h"
#include "CLI-commands.h"
#include "inactivity.h"
#include "rtc_util.h"
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

#define CLI_TASK_QUEUE_LEN			10

// Number of characters received by each UART interrupt
#define NUMRXCHARACTERS				1

// Limits for the parameters of the commands
#define MAX_WAKE_PERIOD_S			65535
#define MAX_AWAKE_PERIOD_S			86400
#define MIN_BLINK_PERIOD_MS			50
#define MAX_BLINK_PERIOD_MS			10000
#define MAX_TIME_PRINT_PERIOD_S		3600
#define MAX_INACTIVITY_PERIOD_S		86400

// The shortest inactivity period, used to force DPD. inactivity_setPeriod() raises it as necessary.
#define FORCE_DPD_INACTIVITY_MS		1

// The period for the watchdog that resets the processor
#define WATCHDOG_RESET_MS			100

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

extern Barrier_t startupBarrier;  // Object that calls a function when all tasks are ready

// The internal states of the tasks, and how many of them app_main() actually populated
extern internal_state_t internalStates[WW500_MINIMAL_NUMBER_OF_TASKS];
extern uint8_t numTasksRegistered;

/********************************************** Local Variables **********************************************/

// This is the handle of the task
static TaskHandle_t cli_task_id;

// The queue that the UART callback uses to send characters to the task
static QueueHandle_t xCliTaskQueue;

// The character received by the UART, put here by the interrupt
static char rxChar;

static char cliInBuffer[CLI_CMD_LINE_BUF_SIZE];		/* Buffer for input */
static char cliOutBuffer[CLI_OUTPUT_BUF_SIZE];		/* Buffer for output */

// How long to stay awake after a character is typed. Changed by the 'inactivity' command.
static uint32_t cliInactivityMs = WW500_MINIMAL_INACTIVITY_CLI_MS;

/**************************************** Local Function Declarations ****************************************/

static void vCmdLineTask(void *pvParameters);
static void vCmdLineTask_cb(void);
static void registerCommands(void);
static void processSingleCharacter(char c);
static bool parseUint(const char *param, BaseType_t length, uint32_t *value);

static BaseType_t prvVer(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvTaskStats(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvTaskState(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvGetUtc(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvSetUtc(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvWake(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvAwake(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvBlink(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvTimePrint(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvLed(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvInactivity(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvDpd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvReset(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Converts a command parameter to an unsigned number.
 *
 * FreeRTOS_CLIGetParameter() returns a pointer into the command line, and the parameter is
 * not '\0' terminated, so it is copied first.
 *
 * @param param  Pointer to the parameter, from FreeRTOS_CLIGetParameter().
 * @param length The length of the parameter.
 * @param value  Pointer to receive the number.
 * @return true if the parameter was a valid decimal number.
 */
static bool parseUint(const char *param, BaseType_t length, uint32_t *value) {
	char text[12];
	char *end;

	if ((param == NULL) || (length <= 0) || (length >= (BaseType_t) sizeof(text))) {
		return false;
	}

	memcpy(text, param, length);
	text[length] = '\0';

	*value = strtoul(text, &end, 10);

	return (*end == '\0');
}

/**
 * @brief Reports the board name and build time. Command: ver
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvVer(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "%s %s",
			ww500_minimal_getBoardNameString(), ww500_minimal_getVersionString());

	return pdFALSE;
}

/**
 * @brief Displays a table showing the state of each FreeRTOS task. Command: ps
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvTaskStats(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *const pcHeader = "Task          State  Priority  Stack	#\r\n************************************************\r\n";

	(void)pcCommandString;
	(void)xWriteBufferLen;
	configASSERT(pcWriteBuffer);

	strcpy(pcWriteBuffer, pcHeader);
	vTaskList(pcWriteBuffer + strlen(pcHeader));

	return pdFALSE;
}

/**
 * @brief Displays a table showing the internal states of the tasks. Command: states
 *
 * Returns pdTRUE until every task has been listed, one task per call.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdTRUE if there is more output, otherwise pdFALSE.
 */
static BaseType_t prvTaskState(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *const pcHeader = "Task  State #    State Name	Priority\r\n*************************************";
	static bool listing = false;
	static uint8_t i = 0;

	(void)pcCommandString;

	if (!listing) { // Do this the first time through
		listing = true;
		strcpy(pcWriteBuffer, pcHeader);
		// Return for the task details one at a time
		return pdTRUE;
	}

	if (i < numTasksRegistered) {
		snprintf(pcWriteBuffer, xWriteBufferLen, "%s\t%d\t%s\t%d",
								  pcTaskGetName(internalStates[i].task_id),
								  (int)internalStates[i].getState(),
								  internalStates[i].stateString(),
								  internalStates[i].priority);
		i++;
	}

	if (i == numTasksRegistered) {
		// Done. reset static variables
		listing = false;
		i = 0;
		return pdFALSE;
	}

	// Return for more
	return pdTRUE;
}

/**
 * @brief Prints the RTC time as an ISO string. Command: getutc
 *
 * The UTC format is "YYYY-MM-DDTHH:MM:SSZ", e.g. 2025-03-05T21:52:04Z
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvGetUtc(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	RTC_ERROR_E ret;
	char timeString[RTC_UTIL_UTC_STRING_LENGTH];

	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	ret = rtc_util_getString(timeString, sizeof(timeString));

	if (ret == RTC_NO_ERROR) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "%s", timeString);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error %d", ret);
	}

	return pdFALSE;
}

/**
 * @brief Sets the RTC from an ISO 8601 string. Command: setutc <YYYY-MM-DDTHH:MM:SSZ>
 *
 * Example: setutc 2025-03-21T09:05:00Z
 *
 * This takes 1-2 s and suppresses interrupts for about 1.4 s.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvSetUtc(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char timeString[RTC_UTIL_UTC_STRING_LENGTH];
	RTC_ERROR_E ret;
	rtc_time tm;
	TickType_t startTime;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if ((pcParameter == NULL) || (lParameterStringLength >= (BaseType_t) sizeof(timeString))) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected YYYY-MM-DDTHH:MM:SSZ");
		return pdFALSE;
	}

	// The parameter is not '\0' terminated
	memcpy(timeString, pcParameter, lParameterStringLength);
	timeString[lParameterStringLength] = '\0';

	ret = rtc_util_stringToTime(timeString, &tm);
	if (ret != RTC_NO_ERROR) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error %d. Expected YYYY-MM-DDTHH:MM:SSZ", ret);
		return pdFALSE;
	}

	startTime = xTaskGetTickCount();
	ret = rtc_util_setTime(&tm);

	if (ret == RTC_NO_ERROR) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "RTC set to %s (this took %dms)",
				timeString, (int) ww500_minimal_getElapsedMs(startTime));
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Error %d setting RTC", ret);
	}

	return pdFALSE;
}

/**
 * @brief Sets the RTC alarm period used to wake from DPD. Command: wake <seconds>
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvWake(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t seconds;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (parseUint(pcParameter, lParameterStringLength, &seconds) && (seconds >= 1) && (seconds <= MAX_WAKE_PERIOD_S)) {
		ww500_minimal_setAlarmPeriod((uint16_t) seconds);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Will wake %d s after entering DPD (until the next reset or wake)", (int) seconds);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 1 to %d seconds", MAX_WAKE_PERIOD_S);
	}

	return pdFALSE;
}

/**
 * @brief Sets how long the blinky task runs before it stops. Command: awake <seconds>
 *
 * Measured from when blinking started, so it applies immediately if still blinking. Use
 * 'blink <ms>' to restart the run time.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvAwake(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t seconds;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (parseUint(pcParameter, lParameterStringLength, &seconds) && (seconds >= 1) && (seconds <= MAX_AWAKE_PERIOD_S)) {
		blinky_task_setRunTime(seconds * 1000);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Run time is %d s (from when blinking started)", (int) seconds);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 1 to %d seconds", MAX_AWAKE_PERIOD_S);
	}

	return pdFALSE;
}

/**
 * @brief Starts or stops the blinking. Command: blink <ms|off>
 *
 * 'blink off' stops the blinky task and switches the LEDs off, so the tasks become idle and
 * DPD follows after the inactivity period. 'blink <ms>' (re)starts it with the given period.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvBlink(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t periodMs;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if ((pcParameter != NULL) && (lParameterStringLength == 3) && (strncmp(pcParameter, "off", 3) == 0)) {
		blinky_task_setPeriod(0);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Blinking is off");
	}
	else if (parseUint(pcParameter, lParameterStringLength, &periodMs) &&
			(periodMs >= MIN_BLINK_PERIOD_MS) && (periodMs <= MAX_BLINK_PERIOD_MS)) {
		blinky_task_setPeriod(periodMs);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Blinking every %d ms", (int) periodMs);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 'off' or %d to %d ms", MIN_BLINK_PERIOD_MS, MAX_BLINK_PERIOD_MS);
	}

	return pdFALSE;
}

/**
 * @brief Sets the period at which the time is printed while blinking. Command: timeprint <seconds>
 *
 * 0 turns the printing off. The time is printed only while the blinky task is running, so that
 * the printing does not stop the tasks becoming inactive.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvTimePrint(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t seconds;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (parseUint(pcParameter, lParameterStringLength, &seconds) && (seconds <= MAX_TIME_PRINT_PERIOD_S)) {
		blinky_task_setTimePrintPeriod(seconds * 1000);
		if (seconds == 0) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Time printing is off");
		}
		else {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Printing the time every %d s while blinking", (int) seconds);
		}
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 0 (off) to %d seconds", MAX_TIME_PRINT_PERIOD_S);
	}

	return pdFALSE;
}

/**
 * @brief Sets an LED directly. Command: led <9|10> <0|1>
 *
 * The blinky task will overwrite the LEDs while it is blinking, so use 'blink off' first.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvLed(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t pin;
	uint32_t value;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &pin) || ((pin != 9) && (pin != 10))) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected pin 9 or 10, then 0 or 1");
		return pdFALSE;
	}

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 2, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &value) || (value > 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected pin 9 or 10, then 0 or 1");
		return pdFALSE;
	}

	if (pin == 9) {
		ww500_minimal_ledPb9(value == 1);
	}
	else {
		ww500_minimal_ledPb10(value == 1);
	}

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "PB%d = %d", (int) pin, (int) value);

	return pdFALSE;
}

/**
 * @brief Sets how long the device stays awake after a character is typed. Command: inactivity <seconds>
 *
 * Together with 'blink off' this holds the device awake and idle, for measuring operating current.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvInactivity(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t seconds;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (parseUint(pcParameter, lParameterStringLength, &seconds) && (seconds >= 1) && (seconds <= MAX_INACTIVITY_PERIOD_S)) {
		cliInactivityMs = seconds * 1000;
		inactivity_setPeriod(cliInactivityMs);
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "DPD follows %d s after the tasks become inactive", (int) seconds);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 1 to %d seconds", MAX_INACTIVITY_PERIOD_S);
	}

	return pdFALSE;
}

/**
 * @brief Enters deep power down (DPD) as soon as possible. Command: dpd
 *
 * Stops the blinking so all tasks are idle, and shortens the inactivity period.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvDpd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Forcing DPD by stopping the blinking and shortening the inactivity period");

	blinky_task_setPeriod(0);
	inactivity_setPeriod(FORCE_DPD_INACTIVITY_MS);

	return pdFALSE;
}

/**
 * @brief Resets the processor using the watchdog. Command: reset
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvReset(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Resetting by watchdog in %d ms", WATCHDOG_RESET_MS);

	ww500_minimal_reset(WATCHDOG_RESET_MS);

	return pdFALSE;
}

/**
 * @brief Registers the CLI commands. The 'help' command is built in.
 *
 * The definitions are static so they still exist after this function exits: FreeRTOS+CLI
 * keeps pointers to them.
 */
static void registerCommands(void) {
	static const CLI_Command_Definition_t commands[] = {
		{ "ver", "ver:\r\n Reports the board name and build time\r\n", prvVer, 0 },
		{ "ps", "ps:\r\n Displays a table showing the state of each FreeRTOS task\r\n", prvTaskStats, 0 },
		{ "states", "states:\r\n Displays a table showing the internal states of the tasks\r\n", prvTaskState, 0 },
		{ "getutc", "getutc:\r\n Prints the time as an ISO 8601 string\r\n", prvGetUtc, 0 },
		{ "setutc", "setutc <YYYY-MM-DDTHH:MM:SSZ>:\r\n Sets the RTC, e.g. setutc 2025-03-21T09:05:00Z (takes 1-2 s)\r\n", prvSetUtc, 1 },
		{ "wake", "wake <seconds>:\r\n Sets the RTC alarm period for waking from DPD (reverts after DPD)\r\n", prvWake, 1 },
		{ "awake", "awake <seconds>:\r\n Sets how long the blinking runs before it stops (reverts after DPD)\r\n", prvAwake, 1 },
		{ "blink", "blink <ms|off>:\r\n Starts blinking with a period in ms, or stops it. Once stopped DPD follows\r\n", prvBlink, 1 },
		{ "timeprint", "timeprint <seconds>:\r\n Prints the time this often while blinking. 0 = off\r\n", prvTimePrint, 1 },
		{ "led", "led <9|10> <0|1>:\r\n Sets the LED on PB9 or PB10 (use 'blink off' first)\r\n", prvLed, 2 },
		{ "inactivity", "inactivity <seconds>:\r\n Sets how long to stay awake once the tasks are idle\r\n", prvInactivity, 1 },
		{ "dpd", "dpd:\r\n Enters deep power down as soon as possible\r\n", prvDpd, 0 },
		{ "reset", "reset:\r\n Resets the processor using the watchdog\r\n", prvReset, 0 },
	};

	for (uint8_t i = 0; i < (sizeof(commands) / sizeof(commands[0])); i++) {
		FreeRTOS_CLIRegisterCommand(&commands[i]);
	}
}

/**
 * @brief UART interrupt callback.
 *
 * Called after a character is received and placed in rxChar. Sends a message to the CLI task.
 */
static void vCmdLineTask_cb(void) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	APP_MSG_T send_msg;

	send_msg.msg_data = 0;
	send_msg.msg_parameter = 0;
	send_msg.msg_event = APP_MSG_CLITASK_RXCHAR;

	xQueueSendFromISR(xCliTaskQueue, &send_msg, &xHigherPriorityTaskWoken);

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * @brief Processes a single character that has arrived from the console UART.
 *
 * The characters are accumulated in cliInBuffer[]. When a \n arrives
 * FreeRTOS_CLIProcessCommand() processes cliInBuffer[] and places the result in cliOutBuffer[],
 * which is then printed to the console.
 *
 * @param c The character received.
 */
static void processSingleCharacter(char c) {
	static uint16_t index = 0; /* Index into cliInBuffer */
	BaseType_t xMore;

	switch (c) {

	case (0x08):
		// Backspace
		if (index > 0) {
			index--;
			xprintf("\x08 \x08");
		}
		fflush(stdout);
		break;

	case (0x03):
		//^C abort
		index = 0;
		xprintf("^C");
		XP_YELLOW;
		xprintf("\ncmd> ");
		XP_WHITE;
		break;

	case '\r':
		// Ignore. Take action on '\n' only
		break;

	case '\n':
		xprintf("\r\n");
		// Null terminate the string in the receive buffer for safety
		cliInBuffer[index] = '\0';

		// Evaluate the command - loop while the registered command returns true.
		// e.g. the 'states' command loops through for every task
		do {
			memset(cliOutBuffer, 0, CLI_OUTPUT_BUF_SIZE);
			xMore = FreeRTOS_CLIProcessCommand(cliInBuffer, cliOutBuffer, CLI_OUTPUT_BUF_SIZE);
			xprintf("%s\n", cliOutBuffer);
		} while (xMore != pdFALSE);

		// New prompt
		index = 0;
		XP_YELLOW;
		xprintf("cmd> ");
		XP_WHITE;
		fflush(stdout);
		break;

	default:
		// 'normal' characters
		if (index < CLI_CMD_LINE_BUF_SIZE) {
			putchar(c);
			cliInBuffer[index++] = c;
			fflush(stdout);
		}
		else {
			// Throw away data and beep terminal
			putchar(0x07);
			fflush(stdout);
		}
		break;
	}
}

/**
 * @brief The command line task.
 *
 * Provides a prompt on the serial interface and takes input from the user to evaluate via the
 * FreeRTOS+CLI parser. NOTE: FreeRTOS+CLI is part of FreeRTOS+ and has different licensing
 * requirements. See http://www.freertos.org/FreeRTOS-Plus for more information.
 *
 * @param pvParameters Not used.
 */
static void vCmdLineTask(void *pvParameters) {
	DEV_UART_PTR dev_uart_ptr;
	DEV_BUFFER rx_buffer;
	APP_MSG_T rxMessage;

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting CLI Task\n");
	XP_WHITE;

	/* Register available CLI commands */
	registerCommands();

	xprintf("\nEnter 'help' to view a list of available commands.\n");
	XP_YELLOW;
	xprintf("cmd> ");
	XP_WHITE;
	fflush(stdout);

	// Prepare the console UART for interrupt-driven receive of characters into rxChar
	dev_uart_ptr = hx_drv_uart_get_dev(USE_DW_UART_0);
	dev_uart_ptr->uart_open(UART_BAUDRATE_921600);
	dev_uart_ptr->uart_control(UART_CMD_SET_RXCB, (UART_CTRL_PARAM)vCmdLineTask_cb);

	rx_buffer.buf = (void *)&rxChar;
	rx_buffer.len = NUMRXCHARACTERS;

	// Enable console UART to receive characters, interrupt-driven
	dev_uart_ptr->uart_control(UART_CMD_SET_RXINT_BUF, (UART_CTRL_PARAM)&rx_buffer);
	dev_uart_ptr->uart_control(UART_CMD_SET_RXINT, (UART_CTRL_PARAM)1);

	barrier_ready(&startupBarrier);		// Call a function when every task reaches this point

	for (;;) {
		if (xQueueReceive(xCliTaskQueue, &rxMessage, portMAX_DELAY) == pdTRUE) {

			switch (rxMessage.msg_event) {

			case APP_MSG_CLITASK_RXCHAR:
				// A character has arrived from the UART (user types at console).
				// Extend the inactivity period so the device does not enter DPD while debugging
				inactivity_setPeriod(cliInactivityMs);

				processSingleCharacter(rxChar);

				// Re-enable the interrupts
				rx_buffer.buf = (void *)&rxChar;
				rx_buffer.len = NUMRXCHARACTERS;

				dev_uart_ptr->uart_control(UART_CMD_SET_RXINT_BUF, (UART_CTRL_PARAM)&rx_buffer);
				dev_uart_ptr->uart_control(UART_CMD_SET_RXINT, (UART_CTRL_PARAM)1);
				break;

			default:
				xprintf("CLI Task: unexpected event 0x%04x\n", rxMessage.msg_event);
				break;
			}
		}
	}
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Creates the CLI task and its queue. Call before the scheduler is started.
 *
 * @param priority   The FreeRTOS priority for the task.
 * @param wakeReason The reason for this wakeup (not used).
 * @return The handle of the task.
 */
TaskHandle_t cli_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason) {
	(void)wakeReason;

	if (priority < 0) {
		priority = 0;
	}

	xCliTaskQueue = xQueueCreate(CLI_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xCliTaskQueue == 0) {
		xprintf("Failed to create xCliTaskQueue\n");
		configASSERT(0);
	}

	if (xTaskCreate(vCmdLineTask, (const char *)"CLI",
			configMINIMAL_STACK_SIZE * 4,
			NULL, priority,
			&cli_task_id) != pdPASS) {
		xprintf("Failed to create vCmdLineTask\n");
		configASSERT(0);
	}

	return cli_task_id;
}

/**
 * @brief Returns the internal state as a number. This task has no states.
 *
 * @return 0.
 */
uint16_t cli_getState(void) {
	return 0;
}

/**
 * @brief Returns the internal state as a string. This task has no states.
 *
 * @return A pointer to a constant string.
 */
const char * cli_getStateString(void) {
	return "-";
}

/**
 * @brief Safely appends formatted text into a FreeRTOS CLI output buffer.
 *
 * Advances *buf and reduces *len by the amount written, so calls can be chained.
 *
 * @param buf Pointer to the current write position. Updated on return.
 * @param len Pointer to the space remaining. Updated on return.
 * @param fmt printf-style format string.
 * @return pdTRUE if the formatted text was fully written into the buffer.
 * @return pdFALSE if the output was truncated or an encoding error occurred.
 */
BaseType_t cli_append(char **buf, size_t *len, const char *fmt, ...) {
    if (*len == 0) {
        return pdFALSE; // no space left
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(*buf, *len, fmt, args);
    va_end(args);

    if (written < 0 || written >= *len) {
        // Truncated or error — stop writing
        (*buf)[*len - 1] = '\0';  // ensure null termination
        *len = 0;
        return pdFALSE;
    }

    *buf += written;
    *len -= written;
    return pdTRUE;
}
