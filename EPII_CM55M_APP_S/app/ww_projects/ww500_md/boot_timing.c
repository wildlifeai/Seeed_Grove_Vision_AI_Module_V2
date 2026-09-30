/*
 * boot_timing.c
 *
 *  Created on: 30 Sep 2026
 *      Author: Charles Palmer
 *
 *  Times the stages from the start of app_main() to the first frame after a boot. See boot_timing.h.
 */

/*********************************************** Includes ****************************************************/

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"

#include "WE2_device.h"
#include "xprintf.h"
#include "printf_x.h"

#include "boot_timing.h"

/*********************************************** Local Defines ***********************************************/

/********************************************** Local Variables **********************************************/

#if BOOT_TIMING_ENABLED
// Names of the points, in the order of BOOT_TIMING_POINT_E
static const char * pointName[BOOT_TIMING_NUM_POINTS] = {
		"cameras checked",
		"scheduler started",
		"SD mounted",
		"CONFIG.TXT loaded",
		"image dir ready",
		"SD ready",
		"camera ready",
		"capture started",
		"frame ready",
};

// Time of each point in ms since app_main() started, valid if pointRecorded[] is true
static uint32_t pointMs[BOOT_TIMING_NUM_POINTS];
static bool pointRecorded[BOOT_TIMING_NUM_POINTS];

// True if the core has a cycle counter and it was started
static bool cycleCounterRunning = false;

// Set just before the scheduler starts: the ms before it, to which the tick count is then added
static bool schedulerStarted = false;
static uint32_t msBeforeScheduler = 0;

// True once the times have been printed in this boot
static bool reported = false;
#endif // BOOT_TIMING_ENABLED

/**************************************** Local Function Declarations ****************************************/

#if BOOT_TIMING_ENABLED
static uint32_t cycleCounterMs(void);
static uint32_t msSinceStart(void);
#endif // BOOT_TIMING_ENABLED

/**************************************** Local Function Definitions *****************************************/

#if BOOT_TIMING_ENABLED
/**
 * @brief Returns the cycle counter as ms since boot_timing_start(), or 0 if there is no cycle counter.
 *
 * @return ms. The 32-bit counter wraps after about 10 s at 400 MHz, far longer than the time before the scheduler.
 */
static uint32_t cycleCounterMs(void) {
	uint32_t cyclesPerMs = SystemCoreClock / 1000;

	if (!cycleCounterRunning || (cyclesPerMs == 0)) {
		return 0;
	}
	return DWT->CYCCNT / cyclesPerMs;
}

/**
 * @brief Returns the time since boot_timing_start().
 *
 * Before the scheduler starts: the cycle counter. After it: the time before it plus the tick count.
 *
 * @return ms since app_main() started.
 */
static uint32_t msSinceStart(void) {
	if (!schedulerStarted) {
		return cycleCounterMs();
	}
	return msBeforeScheduler + (uint32_t) ((xTaskGetTickCount() * 1000ULL) / configTICK_RATE_HZ);
}
#endif // BOOT_TIMING_ENABLED

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Starts the timing. Call first thing in app_main().
 *
 * Enables and zeroes the core's cycle counter (DWT CYCCNT), if the core has one.
 */
void boot_timing_start(void) {
#if BOOT_TIMING_ENABLED
	if ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) == 0) {
		DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;		// enable the DWT
		DWT->CYCCNT = 0;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
		cycleCounterRunning = true;
	}
#endif // BOOT_TIMING_ENABLED
}

/**
 * @brief Records the time of a point, the first time it is reached in this boot.
 *
 * BOOT_TIMING_SCHEDULER must be marked just before vTaskStartScheduler(): from then on the tick count is used.
 * Call from task context (or before the scheduler), not from an interrupt.
 *
 * @param point The point reached.
 */
void boot_timing_mark(BOOT_TIMING_POINT_E point) {
#if BOOT_TIMING_ENABLED
	if ((point >= BOOT_TIMING_NUM_POINTS) || pointRecorded[point]) {
		return;
	}

	pointMs[point] = msSinceStart();
	pointRecorded[point] = true;

	if (point == BOOT_TIMING_SCHEDULER) {
		msBeforeScheduler = pointMs[point];
		schedulerStarted = true;
	}
#else
	(void) point;
#endif // BOOT_TIMING_ENABLED
}

/**
 * @brief Prints the times recorded, once per boot.
 *
 * Call when the first frame is ready. Points not reached are left out.
 *
 * @param wakeReason Text for the console, e.g. "motion", "timer", "cold".
 */
void boot_timing_report(const char *wakeReason) {
#if BOOT_TIMING_ENABLED
	uint8_t i;

	if (reported) {
		return;
	}
	reported = true;

	XP_LT_CYAN;
	xprintf("Boot timing (%s wake), ms since app_main()%s:", wakeReason,
			cycleCounterRunning ? "" : " (no cycle counter: times before the scheduler are 0)");
	for (i = 0; i < BOOT_TIMING_NUM_POINTS; i++) {
		if (pointRecorded[i]) {
			xprintf(" %s %u,", pointName[i], (unsigned) pointMs[i]);
		}
	}
	xprintf("\n");
	XP_WHITE;
#else
	(void) wakeReason;
#endif // BOOT_TIMING_ENABLED
}
