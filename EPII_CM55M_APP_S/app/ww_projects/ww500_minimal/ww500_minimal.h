/*
 * ww500_minimal.h
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  Main header for the 'ww500_minimal' app: a minimal FreeRTOS image for a WW500_C00 board
 *  that carries the HX6538 (AI processor) and almost nothing else, so that DPD (sleep) and
 *  operating current can be measured on their own.
 *
 *  It excludes the camera, FatFS, BLE interface and neural network processing.
 *  See _Documentation/development reports/2026-09-20_Minimal__FreeRTOS/
 *
 *  Derived from ww500_md.h.
 */

#ifndef WW500_MINIMAL_H_
#define WW500_MINIMAL_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "WE2_device.h"
#include "WE2_core.h"
#include "board.h"

/********************************************** Global Defines ***********************************************/

// The default RTC alarm period, in seconds, used to wake from DPD. Settable by the CLI.
#define WW500_MINIMAL_ALARM_PERIOD_S			30

// How long the blinky task runs, in ms, before it stops (so that inactivity is detected).
// Type a character at the console to hold the device awake (see WW500_MINIMAL_INACTIVITY_CLI_MS).
#define WW500_MINIMAL_RUN_TIME_COLD_MS			3000
#define WW500_MINIMAL_RUN_TIME_WARM_MS			10000

// If 1, the first read of the RTC after waking from DPD waits about 1 s for the counter to
// synchronise, so the "Woke at" time is right, but the processor is awake for 1 s longer.
// If 0 that time is the time DPD was entered (30 s old, with the default alarm period).
#define WW500_MINIMAL_SYNC_RTC_AFTER_DPD		0

// How often the time is printed while blinking, in ms (0 = off). Settable by the CLI.
#define WW500_MINIMAL_TIME_PRINT_PERIOD_MS		1000

// The period of inactivity, in ms, before DPD is entered
#define WW500_MINIMAL_INACTIVITY_MS				5000

// Use an extended period if the user starts to use the console, for debugging
#define WW500_MINIMAL_INACTIVITY_CLI_MS			60000

// The time set at cold boot. A date prior to 2025 flags "not set".
#define WW500_MINIMAL_DEFAULT_TIME				"2024-01-01T00:00:00Z"

// The size of the buffer for CLI output. FreeRTOS_CLI.c uses this name.
#define configCOMMAND_INT_MAX_OUTPUT_SIZE		512

// Ticks to wait when sending to a queue
#define WW500_MINIMAL_QUEUE_SEND_TICKS			pdMS_TO_TICKS(1000)

/*********************************************** Global Types ************************************************/

// Number of tasks (the size of the internalStates[] array)
// CLI and Blinky always run; FatFS and the image task are each optional (see WW500_MINIMAL_NO_FATFS,
// WW500_MINIMAL_NO_CAMERA in ww500_minimal.mk - build-time experiments to isolate their effect on DPD current)
#if defined(WW500_MINIMAL_NO_CAMERA) && defined(WW500_MINIMAL_NO_FATFS)
#define WW500_MINIMAL_NUMBER_OF_TASKS			2
#elif defined(WW500_MINIMAL_NO_CAMERA) || defined(WW500_MINIMAL_NO_FATFS)
#define WW500_MINIMAL_NUMBER_OF_TASKS			3
#else
#define WW500_MINIMAL_NUMBER_OF_TASKS			4
#endif

// Function pointer types, to get the internal state of a task
typedef uint16_t (*int_func_ptr)(void);
typedef const char* (*str_func_ptr)(void);

// Structure to allow retrieval of the internal state of each task
typedef struct {
	TaskHandle_t	task_id;
	int_func_ptr	getState;
	str_func_ptr 	stateString;
	uint16_t		priority;
} internal_state_t;

// Possible wakeup reasons
typedef enum {
	WW500_MINIMAL_WAKE_REASON_UNKNOWN,
	WW500_MINIMAL_WAKE_REASON_COLD,			// Cold boot
	WW500_MINIMAL_WAKE_REASON_WAKE_PIN,		// WAKE signal on PA0
	WW500_MINIMAL_WAKE_REASON_TIMER,		// RTC
} WW500_MINIMAL_WAKE_REASON_E;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Main entry point. Called from main.c. Does not return.
 */
int app_main(void);

/**
 * @brief Drives the LED on PB9 (active high).
 */
void ww500_minimal_ledPb9(bool on);

/**
 * @brief Drives the LED on PB10 (active high).
 */
void ww500_minimal_ledPb10(bool on);

/**
 * @brief Returns the build time and date as a string.
 */
char * ww500_minimal_getVersionString(void);

/**
 * @brief Returns the board name, defined by BOARD_NAME_STRING in ww.mk.
 */
char * ww500_minimal_getBoardNameString(void);

/**
 * @brief Returns the RTC alarm period, in seconds, used to wake from DPD.
 */
uint16_t ww500_minimal_getAlarmPeriod(void);

/**
 * @brief Sets the RTC alarm period, in seconds, used to wake from DPD.
 */
void ww500_minimal_setAlarmPeriod(uint16_t seconds);

/**
 * @brief Resets the processor using the watchdog.
 */
void ww500_minimal_reset(uint32_t delayMs);

/**
 * @brief Calculates an elapsed time in ms.
 */
uint32_t ww500_minimal_getElapsedMs(TickType_t startTime);

/**
 * @brief Returns the reason for this wakeup.
 */
WW500_MINIMAL_WAKE_REASON_E ww500_minimal_getWakeReason(void);

/**
 * @brief Callback when all tasks have been inactive for a period.
 */
void ww500_minimal_onInactivity(void);

#ifdef __cplusplus
}
#endif

#endif // WW500_MINIMAL_H_
