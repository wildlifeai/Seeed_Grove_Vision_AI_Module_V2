/*
 * inactivity.c
 *
 *  Created on: 13 Apr 2025
 *      Author: charl
 *
 *  Detects inactivity and makes a callback when it is detected. The callback would typically
 *  switch to DPD. See inactivity.h for how it is hooked into FreeRTOS.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted. Only the idle-task approach is
 *  kept: the timer-based alternative was never tested and is not needed here.
 */

/*********************************************** Includes ****************************************************/

#include "FreeRTOS.h"
#include "task.h"

#include "inactivity.h"

/*********************************************** Local Defines ***********************************************/

// Best stop unreasonable times, such as 0
#define MINIMUMINACTIVEPERIOD	200

/********************************************** Local Variables **********************************************/

static void (*inactivity_callback)(void) = NULL;

// Not properly used - probably refactor to remove this?
static BaseType_t inactivity_enabled = pdFALSE;

// Idle hook state
static TickType_t idle_start_tick = 0;

// This ensures that the inactivity callback is not called too often
static BaseType_t inactivity_triggered = pdFALSE;

static uint32_t tasksInactivePeriod = 0;

// This is the number of ticks that must happen before 'inactivity' is declared.
static TickType_t tasksInactiveTicks = 0;

// How many times the idle hook has run. Read by power_diag.c
static volatile uint32_t idleHookCount = 0;

/**************************************** Local Function Declarations ****************************************/

/**************************************** Local Function Definitions *****************************************/

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Initialises inactivity detection.
 *
 * If timeout_ms is 0 then inactivity detection is inhibited.
 *
 * @param timeout_ms The period of inactivity, in ms, before the callback is made. (0 disables)
 * @param callback   The function to call when inactivity is detected. NULL disables detection.
 */
void inactivity_init(uint32_t timeout_ms, void (*callback)(void)) {
	inactivity_triggered = pdFALSE;
	idle_start_tick = 0;

	if (callback == NULL) {
		inactivity_enabled = pdFALSE;
	}
	else {
		inactivity_callback = callback;
		inactivity_enabled = pdTRUE;
		tasksInactivePeriod = timeout_ms;
		tasksInactiveTicks = pdMS_TO_TICKS(tasksInactivePeriod);
	}
}

/**
 * @brief Called when FreeRTOS enters the idle state.
 *
 * Called via vApplicationIdleHook() in freertos_app.c. Requires configUSE_IDLE_HOOK to be 1
 * in FreeRTOSConfig.h. Must not block.
 */
void inactivity_IdleHook(void) {
    TickType_t now;
    TickType_t timeSinceActivity;

    idleHookCount++;

    if (!inactivity_enabled || (tasksInactiveTicks == 0)) {
    	return;
    }

    now = xTaskGetTickCount();

    // At reset and when there is a task switch - inactivity_on_task_switched_in()
    if (idle_start_tick == 0) {
        idle_start_tick = now;
    }

    // calculate the time since one of our tasks ran
    timeSinceActivity = now - idle_start_tick;

    if (!inactivity_triggered && (timeSinceActivity >= tasksInactiveTicks)) {
        inactivity_triggered = pdTRUE;

        if (inactivity_callback) {
            inactivity_callback();
        }
    }
}

/**
 * @brief Returns how many times the idle hook has run.
 *
 * With tickless idle the idle task sleeps between events, so this rises slowly. If the idle
 * task is spinning it rises tens of thousands of times a second.
 *
 * @return The number of calls to inactivity_IdleHook().
 */
uint32_t inactivity_getIdleHookCount(void) {
	return idleHookCount;
}

/**
 * @brief Restarts the idle measurement when any non-idle task is switched in.
 *
 * Called from vApplicationTaskSwitchedIn() in freertos_app.c.
 */
void inactivity_on_task_switched_in(void) {
    if (!inactivity_enabled) {
    	return;
    }

    if (xTaskGetCurrentTaskHandle() != xTaskGetIdleTaskHandle()) {
        idle_start_tick = 0;
        inactivity_triggered = pdFALSE;
    }
}

/**
 * @brief Returns the inactivity period.
 *
 * @return inactivity period in ms.
 */
uint32_t inactivity_getPeriod(void) {
	return tasksInactivePeriod;
}

/**
 * @brief Sets the inactivity period.
 *
 * Used if the user types at the console: inactivity_setPeriod(WW500_MINIMAL_INACTIVITY_CLI_MS)
 *
 * @param timeout_ms Inactivity period in ms. Values below MINIMUMINACTIVEPERIOD are raised to it.
 */
void inactivity_setPeriod(uint32_t timeout_ms) {

	// Prevent unreasonable period, such as 0!
    if (timeout_ms < MINIMUMINACTIVEPERIOD) {
    	tasksInactivePeriod = MINIMUMINACTIVEPERIOD;
    }
    else {
    	tasksInactivePeriod = timeout_ms;
    }

    tasksInactiveTicks = pdMS_TO_TICKS(tasksInactivePeriod);
}
