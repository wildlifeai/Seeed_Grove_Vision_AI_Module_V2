/*
 * watchdog.c
 *
 * A watchdog that runs while the AI processor is awake.
 * See watchdog.h for a description
 *
 *  Created on: 13 Oct 2026
 *      Author: Claude, for Charles Palmer
 */

/********************************** Includes ******************************************/

#include "FreeRTOS.h"
#include "timers.h"
#include "task.h"

#include "hx_drv_watchdog.h"

#include "watchdog.h"
#include "xprintf.h"

/**
 * Stops the hardware watchdog and clears its interrupt flag.
 *
 * Call this first thing at every boot. It is needed whether or not AWAKE_WATCHDOG is defined.
 *
 * A system reset request (used by the fault handlers) does not reset the watchdog. Found 13 Oct 2026:
 * the 1s watchdog started by a fault handler was still running in the next boot, and reset the processor
 * a second time, 1.4s into that boot.
 *
 * The flag matters as well. The hardware resets at the second timeout: the first only sets this flag.
 * If the flag is left set from before the reset, the next watchdog resets at its first timeout,
 * in half the expected time.
 */
void watchdog_init(void) {
	hx_drv_watchdog_stop(WATCHDOG_ID_0);
	hx_drv_watchdog_irq_clear(WATCHDOG_ID_0);
}

#ifdef AWAKE_WATCHDOG

/**************************************** Local Variables **************************************/

// Feeds the watchdog. Created and started the first time the watchdog is started, and then left running
// for ever: when the watchdog is stopped or suspended the callback simply does nothing.
//
// It is NOT stopped and restarted with the watchdog, because each xTimerStop()/xTimerStart() puts a command
// in the FreeRTOS timer queue, which is short (configTIMER_QUEUE_LENGTH is 5). A firmware update suspends and
// resumes the watchdog several times while the timer task can't run, and on 13 Oct 2026 that filled the queue:
// the IF task's next xTimerChangePeriod() failed and its configASSERT(0) hung the processor.
static TimerHandle_t feedTimer = NULL;

// True while the hardware watchdog is running
static volatile bool running = false;

// True if watchdog_suspend() stopped a running watchdog, so watchdog_resume() must restart it
static bool suspended = false;

/**************************************** Local function definitions  *************************************/

/**
 * This is the local callback that executes when the feedTimer expires
 *
 * It restarts the watchdog's count, if the watchdog is running.
 *
 * Timer callbacks run in the FreeRTOS timer task. The inactivity detector ignores that task
 * (see inactivity_on_task_switched_in()), so feeding does not keep the board awake.
 */
static void feedTimerCallback(TimerHandle_t xTimer) {
	(void)xTimer;

	// The critical section stops a higher priority task stopping the watchdog between the test and the feed.
	// (watchdog_start() and watchdog_stop() need none: they change 'running' on the safe side of the hardware call.)
	taskENTER_CRITICAL();
	if (running) {
		hx_drv_watchdog_update(WATCHDOG_ID_0, WATCHDOG_PERIOD_MS);
	}
	taskEXIT_CRITICAL();
}

/**************************************** Global function definitions  *************************************/

/**
 * Starts the watchdog. The first call also creates and starts the timer that feeds it.
 *
 * Can be called before the scheduler starts: the feed timer then starts when the scheduler does.
 */
void watchdog_start(void) {
	WATCHDOG_CFG_T wdg_cfg;

	if (feedTimer == NULL) {
		feedTimer = xTimerCreate("WatchdogFeed",
				pdMS_TO_TICKS(WATCHDOG_FEED_INTERVAL_MS),
				pdTRUE,              // auto-reload
				NULL,                // timer ID (optional)
				feedTimerCallback);
		if (feedTimer == NULL) {
			// With nothing to feed it the watchdog would just reset us, so don't start it
			xprintf("Failed to create the watchdog feed timer. Watchdog not started.\n");
			return;
		}
		if (xTimerStart(feedTimer, 0) != pdPASS) {
			xprintf("Failed to start the watchdog feed timer. Watchdog not started.\n");
			xTimerDelete(feedTimer, 0);
			feedTimer = NULL;
			return;
		}
	}

	wdg_cfg.period = WATCHDOG_PERIOD_MS;
	wdg_cfg.ctrl = WATCHDOG_CTRL_CPU;
	wdg_cfg.state = WATCHDOG_STATE_DC;
	wdg_cfg.type = WATCHDOG_RESET;

	// Clear the first-timeout flag, so that the reset always comes at the second timeout from now
	hx_drv_watchdog_irq_clear(WATCHDOG_ID_0);

	// The feed timer is already running, so the first feed comes within WATCHDOG_FEED_INTERVAL_MS.
	// 'running' is set after the hardware is started, so the callback never feeds a stopped watchdog.
	hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg, NULL);
	suspended = false;
	running = true;
}

/**
 * Stops the watchdog. The feed timer keeps running but does nothing.
 */
void watchdog_stop(void) {
	// 'running' is cleared before the hardware is stopped, so the callback never feeds a stopped watchdog
	running = false;
	suspended = false;
	hx_drv_watchdog_stop(WATCHDOG_ID_0);
}

/**
 * Stops the watchdog for an operation that must not be interrupted by a reset.
 *
 * Does nothing if the watchdog is not running.
 */
void watchdog_suspend(void) {
	if (running) {
		watchdog_stop();
		suspended = true;
	}
}

/**
 * Restarts the watchdog after watchdog_suspend(), if it was running before.
 */
void watchdog_resume(void) {
	if (suspended) {
		watchdog_start();
	}
}

/**
 * @return true if the watchdog is running
 */
bool watchdog_isRunning(void) {
	return running;
}

#else // AWAKE_WATCHDOG

void watchdog_start(void) {
}

void watchdog_stop(void) {
}

void watchdog_suspend(void) {
}

void watchdog_resume(void) {
}

bool watchdog_isRunning(void) {
	return false;
}

#endif // AWAKE_WATCHDOG
