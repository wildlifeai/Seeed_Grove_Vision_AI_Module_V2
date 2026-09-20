/*
 * blinky_task.h
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that alternately blinks the LEDs on PB9 and PB10, and owns entry to DPD.
 *
 *  The task blinks for a run time (longer after a cold boot), then stops. Stopping means that
 *  every task is idle, so the inactivity mechanism (inactivity.c) reports inactivity. The
 *  resulting message tells this task to switch the LEDs off and enter DPD.
 *
 *  Blinking can be stopped and started by messages (e.g. from the CLI) so that operating
 *  current can be measured with the LEDs dark, or so that DPD can be forced.
 *
 *  While blinking, the task can also print the RTC time periodically, so that RTC accuracy can
 *  be observed. This is only done while blinking: a task that prints while otherwise idle would
 *  stop the tasks becoming inactive.
 */

#ifndef BLINKY_TASK_H_
#define BLINKY_TASK_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ww500_minimal.h"

/********************************************** Global Defines ***********************************************/

// Time between LED changes, in ms
#define BLINKY_TASK_PERIOD_MS			500

/*********************************************** Global Types ************************************************/

// The states of the task. Values must match the strings in blinky_task.c
typedef enum {
	BLINKY_TASK_STATE_UNINIT,
	BLINKY_TASK_STATE_BLINKING,
	BLINKY_TASK_STATE_STOPPED,
	BLINKY_TASK_STATE_SLEEPING,
	BLINKY_TASK_NUMSTATES
} BLINKY_TASK_STATE_E;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 */
TaskHandle_t blinky_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason);

/**
 * @brief Returns the internal state as a number.
 */
uint16_t blinky_task_getState(void);

/**
 * @brief Returns the internal state as a string.
 */
const char * blinky_task_getStateString(void);

/**
 * @brief Tells the task that all tasks are inactive, so it should enter DPD.
 */
void blinky_task_notifyInactivity(void);

/**
 * @brief Starts the blinking with a period, or stops it if the period is 0.
 */
bool blinky_task_setPeriod(uint32_t periodMs);

/**
 * @brief Sets how long the task blinks before it stops.
 */
void blinky_task_setRunTime(uint32_t runTimeMs);

/**
 * @brief Sets how often the time is printed while blinking. 0 turns it off.
 */
void blinky_task_setTimePrintPeriod(uint32_t periodMs);

#ifdef __cplusplus
}
#endif

#endif /* BLINKY_TASK_H_ */
