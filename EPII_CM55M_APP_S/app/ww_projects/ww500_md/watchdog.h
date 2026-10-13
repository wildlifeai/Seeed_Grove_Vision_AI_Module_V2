/*
 * watchdog.h
 *
 * A watchdog that runs while the AI processor is awake.
 *
 * If the processor stops (interrupts dead, the scheduler dead, or stuck in an interrupt handler)
 * the watchdog resets it, and a cold boot follows. Before this the only way out was the RESET
 * button or a power cycle.
 *
 * How it works:
 *  - watchdog_start() starts the hardware watchdog and a FreeRTOS timer that feeds it.
 *  - If the feeds stop, the watchdog resets the processor.
 *  - watchdog_stop() is called on the way into DPD.
 *  - watchdog_suspend() and watchdog_resume() go round operations that write to the flash,
 *    where a reset part way through could leave the board unable to boot.
 *
 * What it does not catch: one task stuck while the others still run, because the timer task
 * still feeds the watchdog.
 *
 * See _Documentation/development reports/2026-10-12_cameraErrors (step 8)
 *
 *  Created on: 13 Oct 2026
 *      Author: Claude, for Charles Palmer
 */

#ifndef WATCHDOG_H_
#define WATCHDOG_H_

#include <stdbool.h>

// Comment this out to build without the awake watchdog. The functions below then do nothing.
#define AWAKE_WATCHDOG

// The watchdog period. The hardware resets the processor at the SECOND timeout (the first only
// raises an interrupt), so the reset comes at twice this time after the last feed.
// Measured 13 Oct 2026: 4s gives 8.0s, 10s gives 19.9s, 20s gives 39.8s.
#define WATCHDOG_PERIOD_MS			10000

// How often the watchdog is fed
#define WATCHDOG_FEED_INTERVAL_MS	4000

// Start the watchdog and the timer that feeds it. Can be called before the scheduler starts.
void watchdog_start(void);

// Stop the watchdog and the feed timer. Call on the way into DPD, and before code that uses
// the hardware watchdog for something else (the 'reset' command).
void watchdog_stop(void);

// Stop the watchdog for an operation that must not be interrupted by a reset (a flash write).
// Always follow with watchdog_resume().
void watchdog_suspend(void);

// Restart the watchdog after watchdog_suspend(), if it was running before
void watchdog_resume(void);

// True if the watchdog is running
bool watchdog_isRunning(void);

#endif /* WATCHDOG_H_ */
