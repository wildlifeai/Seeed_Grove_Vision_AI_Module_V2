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
#include "hx_drv_scu.h"
#include "hx_drv_uart.h"

#include "printf_x.h"
#include "xprintf.h"

#include "app_msg.h"
#include "barrier.h"
#include "blinky_task.h"
#include "CLI-commands.h"
#include "fatfs_task.h"
#ifndef WW500_MINIMAL_NO_CAMERA
#include "image_task.h"
#endif // WW500_MINIMAL_NO_CAMERA
#include "inactivity.h"
#include "power_diag.h"
#include "sleep_mode.h"
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

// The longest name of a clock group for 'clkoff'
#define CLK_GROUP_NAME_LEN			8

// The longest wake time for 'sleep', in seconds
#define MAX_SLEEP_PERIOD_S			3600

// Time for the blinky task to stop and switch the LEDs off before 'sleep' enters Power-down
#define SLEEP_SETTLE_MS				50

// How long the 'idle' command measures the idle loop
#define IDLE_MEASURE_MS				2000

// The longest wait for the FatFS task to finish an SD card operation before doing something that stops it
#define FATFS_WAIT_MS				2000

// The size of the buffer for 'sdwrite' and 'sdread' (the text of a command line is shorter than this)
#define CLI_FILE_BUFFER_SIZE		128

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

#ifndef WW500_MINIMAL_NO_FATFS
// The file operation for the 'sdwrite' and 'sdread' commands. The FatFS task replies with APP_MSG_CLITASK_FILE_DONE.
static fileOperation_t cliFileOp;
static char cliFileName[FATFS_TASK_FILENAME_LENGTH];
static char cliFileBuffer[CLI_FILE_BUFFER_SIZE];
static volatile bool cliFileOpBusy = false;
static bool cliFileOpIsRead = false;
#endif // WW500_MINIMAL_NO_FATFS

// How long to stay awake after a character is typed. Changed by the 'inactivity' command.
static uint32_t cliInactivityMs = WW500_MINIMAL_INACTIVITY_CLI_MS;

/**************************************** Local Function Declarations ****************************************/

static void vCmdLineTask(void *pvParameters);
static void vCmdLineTask_cb(void);
static void registerCommands(void);
static void processSingleCharacter(char c);
static bool parseUint(const char *param, BaseType_t length, uint32_t *value);
static void waitForFatFs(void);
#ifndef WW500_MINIMAL_NO_FATFS
static void reportFileOp(void);
#endif // WW500_MINIMAL_NO_FATFS

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
static BaseType_t prvIdle(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClocks(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkOff(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkOn(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkSlow(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkPll(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkDiv(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkUart(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvClkFast(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvSleep(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvXtal(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
#ifndef WW500_MINIMAL_NO_FATFS
static BaseType_t prvSd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvBootCount(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvSdWrite(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvSdRead(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
#endif // WW500_MINIMAL_NO_FATFS
#ifndef WW500_MINIMAL_NO_CAMERA
static BaseType_t prvCapture(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvCam(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
#endif // WW500_MINIMAL_NO_CAMERA
static BaseType_t prvRc32kTrim(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvDpd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvReset(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Waits for the FatFS task to finish what it is doing, for at most FATFS_WAIT_MS.
 *
 * Used before doing something that suppresses interrupts for a while (setting the RTC) or that stops the
 * processor (Power-down), so that an SD card write is not interrupted.
 */
static void waitForFatFs(void) {
	TickType_t startTime = xTaskGetTickCount();

	while (fatfs_task_isBusy() && (ww500_minimal_getElapsedMs(startTime) < FATFS_WAIT_MS)) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
}

#ifndef WW500_MINIMAL_NO_FATFS
/**
 * @brief Prints the result of the 'sdwrite' or 'sdread' operation that has just finished.
 */
static void reportFileOp(void) {
	cliFileOpBusy = false;

	if (cliFileOp.res != FR_OK) {
		XP_RED;
		xprintf("SD card operation on '%s' failed: FatFS error %d%s\n", cliFileName, (int) cliFileOp.res,
				(cliFileOp.res == FR_NOT_READY) ? " (no card)" : "");
		XP_WHITE;
	}
	else if (cliFileOpIsRead) {
		cliFileBuffer[cliFileOp.length] = '\0';	// The buffer was one byte bigger than the length asked for
		xprintf("Read %u bytes from '%s':\n%s\n", (unsigned) cliFileOp.length, cliFileName, cliFileBuffer);
	}
	else {
		xprintf("Wrote %u bytes to '%s'\n", (unsigned) cliFileOp.length, cliFileName);
	}
}

#endif // WW500_MINIMAL_NO_FATFS

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

	// Setting the RTC suppresses interrupts for about 1.4 s, which must not happen in the middle of an SD card write
	waitForFatFs();

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
 * @brief Checks whether tickless idle is sleeping the CPU. Command: idle
 *
 * Blocks this task for IDLE_MEASURE_MS while every task is idle, counting how often the idle
 * hook runs. See power_diag_measureIdle().
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvIdle(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	power_diag_measureIdle(IDLE_MEASURE_MS);

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Done");

	return pdFALSE;
}

/**
 * @brief Prints the clock frequencies and clock enables. Command: clocks
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClocks(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	power_diag_printClocks();

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Done");

	return pdFALSE;
}

/**
 * @brief Switches off a group of unused clocks, to see what they cost. Command: clkoff <group>
 *
 * EXPERIMENT. Groups: image, hsc, u55, i3c, puf, dma, sdio, flash, lsc, sb, all. See power_diag_gateClocks().
 * 'clkon' restores them; so does a reset or a DPD wake.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkOff(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char group[CLK_GROUP_NAME_LEN];

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if ((pcParameter != NULL) && (lParameterStringLength > 0) && (lParameterStringLength < (BaseType_t) sizeof(group))) {
		memcpy(group, pcParameter, lParameterStringLength);
		group[lParameterStringLength] = '\0';

		if (power_diag_gateClocks(group)) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Clock group '%s' is off. 'clocks' shows what is left, 'clkon' restores", group);
			return pdFALSE;
		}
	}

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected image, hsc, u55, i3c, puf, dma, sdio, flash, lsc, sb or all");

	return pdFALSE;
}

/**
 * @brief Restores the clocks switched off by 'clkoff'. Command: clkon
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkOn(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	power_diag_restoreClocks();

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Clocks restored");

	return pdFALSE;
}

/**
 * @brief Switches the CPU and bus clocks to a 24 MHz oscillator. Command: clkslow <rc|xtal>
 *
 * EXPERIMENT. See power_diag_slowClock(). The tick is retuned so FreeRTOS time stays correct.
 * Use "rc". "xtal" needs a 24 MHz crystal to be fitted, otherwise the CPU clock stops.
 * 'clkfast' restores the clocks, as does entering DPD or Power-down.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkSlow(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char source[CLK_GROUP_NAME_LEN];

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if ((pcParameter != NULL) && (lParameterStringLength > 0) && (lParameterStringLength < (BaseType_t) sizeof(source))) {
		memcpy(source, pcParameter, lParameterStringLength);
		source[lParameterStringLength] = '\0';

		if (power_diag_slowClock(source)) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "CPU and bus clocks are now the 24 MHz %s. 'clocks' shows the result, 'clkpll 0' switches the PLL off, 'clkfast' restores", source);
			return pdFALSE;
		}
	}

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected rc or xtal");

	return pdFALSE;
}

/**
 * @brief Moves the console UART's reference clock to the RC oscillator or the crystal. Command: clkuart <rc|xtal>
 *
 * EXPERIMENT. The UART clock comes from the 24 MHz crystal, so switching the crystal off (xtal 24 0) stops the
 * console unless this is used first. The RC oscillator is less accurate, so the console may be garbled: power-cycle
 * if it is. 'clkfast' puts the original source back.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkUart(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	char source[CLK_GROUP_NAME_LEN];

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if ((pcParameter != NULL) && (lParameterStringLength > 0) && (lParameterStringLength < (BaseType_t) sizeof(source))) {
		memcpy(source, pcParameter, lParameterStringLength);
		source[lParameterStringLength] = '\0';

		if (power_diag_uartClock(source)) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "The UART reference clock is now the %s. If this text is garbled, power-cycle", source);
			return pdFALSE;
		}
	}

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected rc or xtal");

	return pdFALSE;
}

/**
 * @brief Divides the slow CPU and bus clock further. Command: clkdiv <1-16>
 *
 * EXPERIMENT. Only after 'clkslow'. With the 24 MHz oscillator a divider of 16 gives 1.5 MHz.
 * 'clkfast' restores the normal clocks.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkDiv(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t divider;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (!parseUint(pcParameter, lParameterStringLength, &divider) || (divider < 1) || (divider > 16)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected a divider from 1 to 16");
	}
	else if (power_diag_divideClock(divider)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Clock divider is %d. 'clocks' shows the frequencies, 'clkfast' restores", (int) divider);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Refused: use 'clkslow rc' first, so the clocks are not taken from the PLL");
	}

	return pdFALSE;
}

/**
 * @brief Switches the PLL off or on. Command: clkpll <0|1>
 *
 * EXPERIMENT. The PLL is only switched off when the CPU and bus clocks are not using it, so use
 * 'clkslow' first.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkPll(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t enable;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);

	if (!parseUint(pcParameter, lParameterStringLength, &enable) || (enable > 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 0 (off) or 1 (on)");
	}
	else if (power_diag_pll(enable == 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "PLL is %s", (enable == 1) ? "on" : "off");
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Refused: the CPU or bus clock is still using the PLL. Use 'clkslow' first");
	}

	return pdFALSE;
}

/**
 * @brief Returns the CPU and bus clocks to normal, switching the PLL back on if needed. Command: clkfast
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvClkFast(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	power_diag_fastClock();

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Clock speed, PLL, crystal and UART clock restored. Clock enables switched off by clkoff stay off: use clkon");

	return pdFALSE;
}

/**
 * @brief Switches a crystal oscillator off or on. Command: xtal <24|32> <0|1>
 *
 * EXPERIMENT. Both oscillators are enabled by default. The WW500 has no 32.768 kHz crystal, so
 * 'xtal 32 0' should only remove the current the idle oscillator draws. The 24 MHz oscillator is refused
 * while the PLL, the CPU clock or the console UART uses it (use clkslow rc, clkpll 0 and clkuart rc first).
 * See power_diag_crystal().
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvXtal(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t which;
	uint32_t enable;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &which) || ((which != 24) && (which != 32))) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 24 or 32, then 0 (off) or 1 (on)");
		return pdFALSE;
	}

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 2, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &enable) || (enable > 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 24 or 32, then 0 (off) or 1 (on)");
		return pdFALSE;
	}

	if (power_diag_crystal(which, enable == 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "The %d %s crystal oscillator is %s. 'clocks' shows the state", (int) which,
				(which == 24) ? "MHz" : "kHz", (enable == 1) ? "enabled" : "disabled");
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Refused: the 24 MHz crystal is in use by the PLL, the CPU clock or the console UART. Use 'clkslow rc', 'clkpll 0' and 'clkuart rc' first");
	}

	return pdFALSE;
}

/**
 * @brief Enters Power-down mode, optionally keeping the memories. Command: sleep <seconds> <retention 0|1>
 *
 * EXPERIMENT. This is the datasheet's Power-down mode: the wake is a warm boot (a restart at the
 * application entry). With retention the memories are kept and the bootloader does not reload the
 * application from flash, so the wake should be much faster than from DPD. The wake sources are
 * the timer and the WAKE pin (PA0, level high). Does not return.
 *
 * Any clocks changed by the clock experiments are restored first, and the LEDs are switched off.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvSleep(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParameter;
	BaseType_t lParameterStringLength;
	uint32_t seconds;
	uint32_t retention;

	configASSERT(pcWriteBuffer);

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 1, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &seconds) || (seconds < 1) || (seconds > MAX_SLEEP_PERIOD_S)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 1 to %d seconds, then retention 0 or 1", MAX_SLEEP_PERIOD_S);
		return pdFALSE;
	}

	pcParameter = FreeRTOS_CLIGetParameter(pcCommandString, 2, &lParameterStringLength);
	if (!parseUint(pcParameter, lParameterStringLength, &retention) || (retention > 1)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected 1 to %d seconds, then retention 0 or 1", MAX_SLEEP_PERIOD_S);
		return pdFALSE;
	}

	XP_LT_RED;
	xprintf(">>> Entering Power-down for %d s, retention %d\n\n", (int) seconds, (int) retention);
	XP_LT_GREY;

	// Let the blinky task stop and switch the LEDs off, then put everything back to normal
	blinky_task_setPeriod(0);
	vTaskDelay(pdMS_TO_TICKS(SLEEP_SETTLE_MS));
	waitForFatFs();		// Do not stop the processor in the middle of an SD card write
	ww500_minimal_ledPb9(false);
	ww500_minimal_ledPb10(false);
	power_diag_restoreClocks();

	sleep_mode_enter_sleep(seconds * 1000, 0, retention);	// does not return

	return pdFALSE;
}

#ifndef WW500_MINIMAL_NO_FATFS
/**
 * @brief Prints the state of the SD card and the boot count. Command: sd
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvSd(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	fatfs_task_printStatus();

	cli_append(&pcWriteBuffer, &xWriteBufferLen, "Done");

	return pdFALSE;
}

/**
 * @brief Reports the boot count. Command: bootcount
 *
 * The count is in BOOTS.TXT on the SD card and is incremented once at every boot. To change it, write the file:
 * for example 'sdwrite BOOTS.TXT 0'.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvBootCount(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	if (fatfs_task_bootCountValid()) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Boot count: %u", (unsigned) fatfs_task_getBootCount());
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Boot count is not available (no SD card, or BOOTS.TXT could not be updated)");
	}

	return pdFALSE;
}

/**
 * @brief Writes text to a file on the SD card. Command: sdwrite <name> <text>
 *
 * The name is an 8.3 name in the root directory. The text is the rest of the command line, spaces included.
 * The file is replaced if it exists. The result is printed when the FatFS task replies.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvSdWrite(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcName;
	const char *pcText;
	BaseType_t nameLength;
	BaseType_t textLength;
	size_t length;

	configASSERT(pcWriteBuffer);

	pcName = FreeRTOS_CLIGetParameter(pcCommandString, 1, &nameLength);
	pcText = FreeRTOS_CLIGetParameter(pcCommandString, 2, &textLength);

	if ((pcName == NULL) || (pcText == NULL) || (nameLength >= FATFS_TASK_FILENAME_LENGTH)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected an 8.3 file name, then the text to write");
	}
	else if (cliFileOpBusy) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "The previous SD card operation has not finished");
	}
	else {
		memcpy(cliFileName, pcName, nameLength);
		cliFileName[nameLength] = '\0';

		// The text is the rest of the command line
		length = strlen(pcText);
		if (length > sizeof(cliFileBuffer)) {
			length = sizeof(cliFileBuffer);
		}
		memcpy(cliFileBuffer, pcText, length);

		cliFileOp.fileName = cliFileName;
		cliFileOp.buffer = (uint8_t *) cliFileBuffer;
		cliFileOp.length = length;
		cliFileOp.doneEvent = APP_MSG_CLITASK_FILE_DONE;
		cliFileOp.senderQueue = xCliTaskQueue;
		cliFileOpIsRead = false;
		cliFileOpBusy = true;

		if (fatfs_task_sendFileOp(APP_MSG_FATFSTASK_WRITE_FILE, &cliFileOp)) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Writing '%s'...", cliFileName);
		}
		else {
			cliFileOpBusy = false;
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "The FatFS task did not accept the request");
		}
	}

	return pdFALSE;
}

/**
 * @brief Reads a file on the SD card and prints it as text. Command: sdread <name>
 *
 * The name is an 8.3 name in the root directory. At most CLI_FILE_BUFFER_SIZE bytes are read. The result is
 * printed when the FatFS task replies.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvSdRead(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcName;
	BaseType_t nameLength;

	configASSERT(pcWriteBuffer);

	pcName = FreeRTOS_CLIGetParameter(pcCommandString, 1, &nameLength);

	if ((pcName == NULL) || (nameLength >= FATFS_TASK_FILENAME_LENGTH)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected an 8.3 file name");
	}
	else if (cliFileOpBusy) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "The previous SD card operation has not finished");
	}
	else {
		memcpy(cliFileName, pcName, nameLength);
		cliFileName[nameLength] = '\0';

		cliFileOp.fileName = cliFileName;
		cliFileOp.buffer = (uint8_t *) cliFileBuffer;
		cliFileOp.length = sizeof(cliFileBuffer) - 1;	// leave room for the '\0' when it is printed
		cliFileOp.doneEvent = APP_MSG_CLITASK_FILE_DONE;
		cliFileOp.senderQueue = xCliTaskQueue;
		cliFileOpIsRead = true;
		cliFileOpBusy = true;

		if (fatfs_task_sendFileOp(APP_MSG_FATFSTASK_READ_FILE, &cliFileOp)) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Reading '%s'...", cliFileName);
		}
		else {
			cliFileOpBusy = false;
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "The FatFS task did not accept the request");
		}
	}

	return pdFALSE;
}

#endif // WW500_MINIMAL_NO_FATFS

#ifndef WW500_MINIMAL_NO_CAMERA
/**
 * @brief Takes a picture and saves it as a JPEG. Command: capture
 *
 * The image task does the work, and prints the result when the file has been written.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvCapture(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	(void)pcCommandString;
	configASSERT(pcWriteBuffer);

	if (image_task_requestCapture()) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Asked the image task for a picture");
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "The image task did not accept the request");
	}

	return pdFALSE;
}

/**
 * @brief Shows or changes the HM0360 mode. Command: cam [mode|init]
 *
 * With no parameter the image task prints the mode the sensor is in. With a number it sets the resting mode
 * (the mode the sensor is in when not taking a picture, and left in for DPD): 0 sleep, 1 continuous, 2 N frames
 * then sleep, 3 N frames then standby, 4 hardware trigger, 6 or 7 hardware trigger N frames. With 'init' the
 * image task writes the long register table again.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvCam(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParam;
	BaseType_t paramLength;
	uint32_t mode;
	bool queued;

	configASSERT(pcWriteBuffer);

	pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &paramLength);

	if (pcParam == NULL) {
		queued = image_task_requestMode(IMAGE_TASK_MODE_REPORT);
	}
	else if ((paramLength == 4) && (strncmp(pcParam, "init", 4) == 0)) {
		queued = image_task_requestReinit();
	}
	else if (parseUint(pcParam, paramLength, &mode) && (mode <= 7)) {
		queued = image_task_requestMode((uint8_t) mode);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected nothing, a mode from 0 to 7, or 'init'");
		return pdFALSE;
	}

	if (queued) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Asked the image task");
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "The image task did not accept the request");
	}

	return pdFALSE;
}

#endif // WW500_MINIMAL_NO_CAMERA

/**
 * @brief Reads or sets the RC32K1K trim register. Command: rc32ktrim [value]
 *
 * EXPERIMENT: the RTC and the sleep/wake timers are clocked from this oscillator (no 32.768 kHz crystal is
 * fitted - see doc/README.md, "RTC accuracy and the 32.768 kHz crystal"), and it measured about 4 % fast. The
 * trim register might correct that, but its direction and step size are not documented, so this is trial and
 * error: measure the RTC rate (see the doc for the procedure), change the trim, then measure again.
 *
 * @param pcWriteBuffer   Buffer for the response.
 * @param xWriteBufferLen Length of the buffer.
 * @param pcCommandString The command line.
 * @return pdFALSE as there is no more output.
 */
static BaseType_t prvRc32kTrim(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
	const char *pcParam;
	BaseType_t paramLength;
	uint32_t value;
	uint8_t trim;

	configASSERT(pcWriteBuffer);

	pcParam = FreeRTOS_CLIGetParameter(pcCommandString, 1, &paramLength);

	if (pcParam == NULL) {
		if (hx_drv_scu_get_RC32K1K_trim(&trim) == SCU_NO_ERROR) {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "RC32K1K trim is %u (0-255)", (unsigned) trim);
		}
		else {
			cli_append(&pcWriteBuffer, &xWriteBufferLen, "Could not read the trim register");
		}
	}
	else if (!parseUint(pcParam, paramLength, &value) || (value > 255)) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Expected nothing, or a value from 0 to 255");
	}
	else if (hx_drv_scu_set_RC32K1K_trim((uint8_t) value) == SCU_NO_ERROR) {
		cli_append(&pcWriteBuffer, &xWriteBufferLen,
				"RC32K1K trim set to %u. Re-measure the RTC rate to see whether this helped", (unsigned) value);
	}
	else {
		cli_append(&pcWriteBuffer, &xWriteBufferLen, "Could not set the trim register");
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
		{ "idle", "idle:\r\n Measures the idle loop for 2 s to show whether tickless idle is sleeping the CPU (use after blink off)\r\n", prvIdle, 0 },
		{ "clocks", "clocks:\r\n Prints the clock frequencies and which clock enables are set\r\n", prvClocks, 0 },
		{ "clkoff", "clkoff <image|hsc|u55|i3c|puf|dma|sdio|flash|lsc|sb|all>:\r\n EXPERIMENT: switches off a group of unused clocks to see what they cost (u55...sdio are the parts of hsc)\r\n", prvClkOff, 1 },
		{ "clkon", "clkon:\r\n Restores the clocks switched off by clkoff\r\n", prvClkOn, 0 },
		{ "clkslow", "clkslow <rc|xtal>:\r\n EXPERIMENT: runs the CPU and buses from a 24 MHz oscillator instead of the 400 MHz PLL. Use rc: xtal stops the CPU clock if no 24 MHz crystal is fitted\r\n", prvClkSlow, 1 },
		{ "clkpll", "clkpll <0|1>:\r\n EXPERIMENT: switches the PLL off (after clkslow) or on\r\n", prvClkPll, 1 },
		{ "clkuart", "clkuart <rc|xtal>:\r\n EXPERIMENT: moves the console UART's reference clock to the RC oscillator or the crystal\r\n", prvClkUart, 1 },
		{ "clkdiv", "clkdiv <1-16>:\r\n EXPERIMENT: divides the slow CPU and bus clock further (use after clkslow rc)\r\n", prvClkDiv, 1 },
		{ "clkfast", "clkfast:\r\n Restores the clock speed, PLL, crystals and UART clock after clkslow. Does not restore clkoff (use clkon)\r\n", prvClkFast, 0 },
		{ "xtal", "xtal <24|32> <0|1>:\r\n EXPERIMENT: switches the 24 MHz or 32.768 kHz crystal oscillator off (0) or on (1)\r\n", prvXtal, 2 },
		{ "sleep", "sleep <seconds> <retention 0|1>:\r\n EXPERIMENT: Power-down mode with timer and WAKE pin wake. Retention keeps RAM so the wake avoids the flash reload\r\n", prvSleep, 2 },
#ifndef WW500_MINIMAL_NO_FATFS
		{ "sd", "sd:\r\n Prints the state of the SD card and the boot count\r\n", prvSd, 0 },
		{ "bootcount", "bootcount:\r\n Prints the boot count, kept in BOOTS.TXT on the SD card\r\n", prvBootCount, 0 },
		{ "sdwrite", "sdwrite <name> <text>:\r\n Writes the text to an 8.3 file in the root of the SD card, replacing it\r\n", prvSdWrite, -1 },
		{ "sdread", "sdread <name>:\r\n Reads an 8.3 file from the root of the SD card and prints it as text\r\n", prvSdRead, 1 },
#endif // WW500_MINIMAL_NO_FATFS
#ifndef WW500_MINIMAL_NO_CAMERA
		{ "capture", "capture:\r\n Takes a picture with the HM0360 (mode 2, one frame) and saves it as Bnnnnnnn.JPG on the SD card\r\n", prvCapture, 0 },
		{ "cam", "cam [mode|init]:\r\n Prints the HM0360 mode, or sets its resting mode (0-4, 6, 7; the default is 2), or 'init' writes its registers again\r\n", prvCam, -1 },
#endif // WW500_MINIMAL_NO_CAMERA
		{ "rc32ktrim", "rc32ktrim [0-255]:\r\n EXPERIMENT: reads or sets the RC32K1K trim register that clocks the RTC and sleep timers (no 32.768 kHz crystal is fitted). Re-measure RTC accuracy after changing it\r\n", prvRc32kTrim, -1 },
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

#ifndef WW500_MINIMAL_NO_FATFS
			case APP_MSG_CLITASK_FILE_DONE:
				// The FatFS task has finished the 'sdwrite' or 'sdread' operation
				reportFileOp();
				break;
#endif // WW500_MINIMAL_NO_FATFS

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
