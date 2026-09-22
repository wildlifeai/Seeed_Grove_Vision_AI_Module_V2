/*
 * ww500_minimal.c
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  Main entry point for the 'ww500_minimal' app. Execution begins at app_main().
 *  See ww500_minimal.h for a description of the app.
 *
 *  Derived from ww500_md.c. Left out, for power and simplicity: I2C master, camera and PCA9574
 *  checks, the SPI and I2C slave pins, the flash manager, self test and the EXIF time handling.
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>
#include <stdlib.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"

#include "hx_drv_scu_export.h"
#include "hx_drv_scu.h"
#include "hx_drv_gpio.h"
#include "hx_drv_timer.h"
#include "hx_drv_pmu.h"
#include "hx_drv_rtc.h"
#include "hx_drv_watchdog.h"

#include "xprintf.h"
#include "printf_x.h"

#include "barrier.h"
#include "blinky_task.h"
#include "CLI-commands.h"
#include "fatfs_task.h"
#include "image_task.h"
#include "inactivity.h"
#include "rtc_util.h"
#include "sleep_mode.h"
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

// Flash time at reset
#define LED_DELAY						50

// Number of times the LEDs flash at cold boot
#define COLD_BOOT_FLASHES				3

// Wake event bits reported after a wake from Power-down mode (see wakeup_event_str[] in sleep_mode.c).
// A wake from DPD is reported differently (PMU_WAKEUPEVENT1_DPD_PAD_AON_GPIO_0 and PMU_WAKEUP_DPD_RTC_INT).
#define WAKE_EVENT_PD_EXT_GPIO			0x10	// the WAKE pin (PA0)
#define WAKE_EVENT_PD_TIMER				0x460	// RTC timer, SB timer 2 and SB timer 0

// A value in retained RAM that shows the RAM survived a sleep (see checkRetention())
#define RETENTION_MAGIC					0x52455431

// To print git information
#ifndef GIT_BRANCH
#define GIT_BRANCH "unknown"
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

#ifndef GIT_DIRTY
#define GIT_DIRTY ""
#endif

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

/********************************************** Local Variables **********************************************/

internal_state_t internalStates[WW500_MINIMAL_NUMBER_OF_TASKS];

// How many internalStates[] entries app_main() actually filled in
uint8_t numTasksRegistered = 0;

// Object that calls a function when all tasks are ready. Available to all of the tasks.
Barrier_t startupBarrier;

// Object that calls a function when every task that takes part has finished what it was doing and is ready for DPD.
// The tasks are the blinky task and the FatFS task; the function is blinky_task_sleepNow().
Barrier_t shutdownBarrier;

static char versionString[64]; // Make sure the buffer is large enough

// These are in the .noinit section, which the start-up code does not clear, so they show whether the RAM
// was kept while asleep. After Power-down with retention they survive; after DPD or a power-cycle they do not.
static uint32_t retentionMagic __attribute__((section(".noinit")));
static uint32_t retentionWakes __attribute__((section(".noinit")));

static WW500_MINIMAL_WAKE_REASON_E wakeReason = WW500_MINIMAL_WAKE_REASON_UNKNOWN;

// The RTC alarm period used to wake from DPD. Reverts to the default after DPD.
static uint16_t alarmPeriodS = WW500_MINIMAL_ALARM_PERIOD_S;

/**************************************** Local Function Declarations ****************************************/

static void initPins(void);
static void initLeds(void);
static void initVersionString(void);
static void showResetOnLeds(uint8_t numFlashes);
static void allTasksReady(void);
static void checkRetention(void);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Initialises the pins used by this app.
 *
 * The console UART (PB0, PB1), the SPI master for the SD card (PB2 data out, PB3 data in, PB4 clock,
 * PB5 chip select) and the LEDs (PB9, PB10) are configured. Everything else is left alone so that it
 * draws no current.
 *
 * NOTE: there is a weak version of pinmux_init() in board/epii_evb/pinmux_init.c that just
 * initialises PB0 and PB1 for UART.
 */
static void initPins(void) {
	SCU_PINMUX_CFG_T pinmux_cfg;

	hx_drv_scu_get_all_pinmux_cfg(&pinmux_cfg);

	/* Init UART0 pin mux to PB0 and PB1 */
	pinmux_cfg.pin_pb0 = SCU_PB0_PINMUX_UART0_RX_1;
	pinmux_cfg.pin_pb1 = SCU_PB1_PINMUX_UART0_TX_1;

	/* Init the SPI master pin mux for the SD card. The SD card driver takes PB5 over as a GPIO while it needs it */
	pinmux_cfg.pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;
	pinmux_cfg.pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;
	pinmux_cfg.pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;
	pinmux_cfg.pin_pb5 = SCU_PB5_PINMUX_SPI_M_CS_1;

	hx_drv_scu_set_all_pinmux_cfg(&pinmux_cfg, 1);

	initLeds();
}

/**
 * @brief Initialises the GPIO pins that drive the LEDs, both off.
 *
 * PB9  = LED on GPIO0, active high
 * PB10 = LED on GPIO1, active high
 */
static void initLeds(void) {
    hx_drv_gpio_set_output(GPIO0, GPIO_OUT_LOW);
    hx_drv_scu_set_PB9_pinmux(SCU_PB9_PINMUX_GPIO0, 1);
	hx_drv_gpio_set_out_value(GPIO0, GPIO_OUT_LOW);

    hx_drv_gpio_set_output(GPIO1, GPIO_OUT_LOW);
    hx_drv_scu_set_PB10_pinmux(SCU_PB10_PINMUX_GPIO1, 1);
	hx_drv_gpio_set_out_value(GPIO1, GPIO_OUT_LOW);
}

/**
 * @brief Initialises a string with the time and date of build.
 *
 * It is used by the CLI command "ver".
 */
static void initVersionString(void) {
    snprintf(versionString, sizeof(versionString), "%s %s",__TIME__, __DATE__);
}

/**
 * @brief Flashes a distinctive pattern on both LEDs, to show life.
 *
 * Uses a blocking delay, as the FreeRTOS scheduler has not started yet.
 *
 * @param numFlashes The number of times to flash.
 */
static void showResetOnLeds(uint8_t numFlashes) {

    for (uint8_t i = 0; i < numFlashes; i++) {

    	ww500_minimal_ledPb9(true);
    	hx_drv_timer_cm55s_delay_ms(LED_DELAY, TIMER_STATE_DC);
    	ww500_minimal_ledPb10(true);
    	hx_drv_timer_cm55s_delay_ms(LED_DELAY, TIMER_STATE_DC);

    	ww500_minimal_ledPb9(false);
    	hx_drv_timer_cm55s_delay_ms(LED_DELAY, TIMER_STATE_DC);
    	ww500_minimal_ledPb10(false);
    	hx_drv_timer_cm55s_delay_ms(LED_DELAY, TIMER_STATE_DC);
    }
}

/**
 * @brief Called when every task has reached its main loop (see the startup barrier).
 *
 * Starts inactivity detection. The blinky task stops blinking after its run time, which
 * makes all tasks idle, and DPD follows after WW500_MINIMAL_INACTIVITY_MS.
 */
static void allTasksReady(void) {
	inactivity_init(WW500_MINIMAL_INACTIVITY_MS, ww500_minimal_onInactivity);
}

/**
 * @brief Reports whether the RAM was kept while asleep.
 *
 * Uses two variables in the .noinit section. If the magic value is still there the RAM was retained (Power-down
 * with retention) and the wake counter is incremented; otherwise (first boot, DPD, power-cycle) it starts again.
 */
static void checkRetention(void) {
	if (retentionMagic == RETENTION_MAGIC) {
		retentionWakes++;
		XP_LT_GREEN;
		xprintf("Retention check: the RAM was kept (%u retained wake%s in a row)\n", (unsigned) retentionWakes,
				(retentionWakes == 1) ? "" : "s");
		XP_WHITE;
	}
	else {
		retentionMagic = RETENTION_MAGIC;
		retentionWakes = 0;
		xprintf("Retention check: the RAM was not kept (first boot, DPD or a power-cycle)\n");
	}
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Drives the LED on PB9 (active high).
 *
 * @param on True to switch the LED on.
 */
void ww500_minimal_ledPb9(bool on) {
	hx_drv_gpio_set_out_value(GPIO0, on ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
}

/**
 * @brief Drives the LED on PB10 (active high).
 *
 * @param on True to switch the LED on.
 */
void ww500_minimal_ledPb10(bool on) {
	hx_drv_gpio_set_out_value(GPIO1, on ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
}

/**
 * @brief Returns the build time and date as a string.
 *
 * @return Pointer to a static string, e.g. "18:29:31 Mar 26 2025".
 */
char * ww500_minimal_getVersionString(void) {
	return versionString;
}

/**
 * @brief Returns the board name, defined by BOARD_NAME_STRING in ww.mk.
 *
 * @return Pointer to a static string.
 */
char * ww500_minimal_getBoardNameString(void) {
	static char * boardString = BOARD_NAME_STRING;
	return boardString;
}

/**
 * @brief Returns the RTC alarm period used to wake from DPD.
 *
 * @return The period in seconds.
 */
uint16_t ww500_minimal_getAlarmPeriod(void) {
	return alarmPeriodS;
}

/**
 * @brief Sets the RTC alarm period used to wake from DPD.
 *
 * The value is lost in DPD, so the default applies after each wake.
 *
 * @param seconds The period in seconds.
 */
void ww500_minimal_setAlarmPeriod(uint16_t seconds) {
	alarmPeriodS = seconds;
}

/**
 * @brief Resets the processor using the watchdog.
 *
 * The reset happens after the delay. The following boot is a cold boot.
 *
 * @param delayMs The watchdog period in ms.
 */
void ww500_minimal_reset(uint32_t delayMs) {
	WATCHDOG_CFG_T wdg_cfg;

	XP_LT_RED;
	xprintf(">>> Reset by watchdog\n\n");
	XP_LT_GREY;	// Grey so the bootloader messages are printed in grey

	wdg_cfg.period = delayMs;
	wdg_cfg.ctrl = WATCHDOG_CTRL_CPU;
	wdg_cfg.state = WATCHDOG_STATE_DC;
	wdg_cfg.type = WATCHDOG_RESET;

	hx_drv_watchdog_start(WATCHDOG_ID_0, &wdg_cfg, NULL);
}

/**
 * @brief Calculates an elapsed time.
 *
 * @param startTime The tick count when the activity started.
 * @return The elapsed time in ms.
 */
uint32_t ww500_minimal_getElapsedMs(TickType_t startTime) {
	TickType_t elapsedTime = xTaskGetTickCount() - startTime;

	return (elapsedTime * 1000) / configTICK_RATE_HZ;
}

/**
 * @brief Returns the reason for this wakeup.
 *
 * @return The wakeup reason, determined in app_main().
 */
WW500_MINIMAL_WAKE_REASON_E ww500_minimal_getWakeReason(void) {
	return wakeReason;
}

/**
 * @brief Callback when all tasks have been inactive for a period.
 *
 * Called from the FreeRTOS idle hook (see inactivity.c). It prints a message, as ww500_md does,
 * and tells the FatFS task and the blinky task. The message sends do not block. DPD is entered
 * when both have finished (see shutdownBarrier).
 */
void ww500_minimal_onInactivity(void) {
	XP_LT_GREEN;
	xprintf("Inactive for %dms\n", inactivity_getPeriod());
	XP_WHITE;

	// Tell each task that takes part in the shutdown barrier. Each finishes what it is doing and then reports
	// to the barrier. The last one to report enters DPD.
	fatfs_task_notifyInactivity();
	image_task_notifyInactivity();
	blinky_task_notifyInactivity();
}

/**
 * @brief Main function. Called from main.c.
 *
 * Prints a banner and the wake reason, sets or restores the RTC, then starts FreeRTOS.
 *
 * @return Does not return.
 */
int app_main(void) {
	uint32_t wakeup_event;
	uint32_t wakeup_event1;
	rtc_time time = {0};
	char timeString[RTC_UTIL_UTC_STRING_LENGTH];
	UBaseType_t priority;
	TaskHandle_t task_id;
	internal_state_t internalState;
	uint8_t taskIndex = 0;

	initVersionString();
	initPins();

	ww500_minimal_ledPb10(true);	// On to show processor is active (not in DPD)

	XP_YELLOW;
	xprintf("\n**** WW500 MINIMAL. (%s) Built: %s %s ****\r\n\n", ww500_minimal_getBoardNameString(), __TIME__, __DATE__);
	XP_WHITE;

	xprintf("Git branch: '%s' %s%s\n",  GIT_BRANCH, GIT_COMMIT, GIT_DIRTY);
	xprintf("Compiler Version: ARM GNU, %s\n\n", __VERSION__);

	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT, &wakeup_event);
	hx_drv_pmu_get_ctrl(PMU_pmu_wakeup_EVT1, &wakeup_event1);

	XP_CYAN;
	sleep_mode_print_event(wakeup_event, wakeup_event1);	// print descriptive string
	XP_WHITE;

	checkRetention();

	if ((wakeup_event == PMU_WAKEUP_NONE) && (wakeup_event1 == PMU_WAKEUPEVENT1_NONE)) {
		showResetOnLeds(COLD_BOOT_FLASHES);	// pattern on LEDs to show cold boot

		XP_LT_BLUE;
		xprintf("\n### Cold Boot ###\n");
		XP_WHITE;
		wakeReason = WW500_MINIMAL_WAKE_REASON_COLD;

		if (configUSE_TICKLESS_IDLE) {
			xprintf("FreeRTOS tickless idle is enabled. configMAX_PRIORITIES = %d\n", configMAX_PRIORITIES);
		}
		else {
			XP_RED;
			xprintf("FreeRTOS tickless idle is disabled. configMAX_PRIORITIES = %d\n", configMAX_PRIORITIES);
			XP_WHITE;
		}

		// Initialises the clocks and sets a time to be going on with
		rtc_util_init(WW500_MINIMAL_DEFAULT_TIME);
	}
	else {
		XP_LT_GREEN;
		xprintf("### Warm Boot ###\n");
		XP_WHITE;

		// We need to clear the RTC interrupt or it will trigger immediately
		hx_drv_rtc_clear_alarm_int_status(RTC_ID_0);

		// Call when exiting DPD
		rtc_util_clkEnable();

#if WW500_MINIMAL_SYNC_RTC_AFTER_DPD
		rtc_util_getTimeAfterDpd(&time);
		rtc_util_timeToString(&time, timeString, sizeof(timeString));
		xprintf("Woke at %s \n", timeString);
#else
		// Not synchronised, so this is the time at which DPD was entered
		rtc_util_getTime(&time);
		rtc_util_timeToString(&time, timeString, sizeof(timeString));
		xprintf("RTC before synchronising (time DPD was entered): %s \n", timeString);
#endif // WW500_MINIMAL_SYNC_RTC_AFTER_DPD

		XP_YELLOW;
		if ((wakeup_event1 == PMU_WAKEUPEVENT1_DPD_PAD_AON_GPIO_0) || ((wakeup_event & WAKE_EVENT_PD_EXT_GPIO) != 0)) {
			xprintf("WAKE pin wake\n");
			wakeReason = WW500_MINIMAL_WAKE_REASON_WAKE_PIN;
		}
		else if ((wakeup_event == PMU_WAKEUP_DPD_RTC_INT) || ((wakeup_event & WAKE_EVENT_PD_TIMER) != 0)) {
			xprintf("Timer wake\n");
			wakeReason = WW500_MINIMAL_WAKE_REASON_TIMER;
		}
		else {
			wakeReason = WW500_MINIMAL_WAKE_REASON_UNKNOWN;
		}
		XP_WHITE;
	}

	xprintf("Initialising FreeRTOS tasks\n");

	// Each task has its own file. Call these to do the task creation and initialisation.
	// Place the highest priority task first. All are allocated successively lower priorities.
	priority = configMAX_PRIORITIES;

	task_id = cli_createTask(--priority, wakeReason);
	internalState.task_id = task_id;
	internalState.getState = cli_getState;
	internalState.stateString = cli_getStateString;
	internalState.priority = priority;
	internalStates[taskIndex++] = internalState;
	xprintf("Created task '%s' Priority %d\n", pcTaskGetName(task_id), priority);

	// The FatFS task mounts the SD card and updates the boot count
	task_id = fatfs_task_createTask(--priority, wakeReason);
	internalState.task_id = task_id;
	internalState.getState = fatfs_task_getState;
	internalState.stateString = fatfs_task_getStateString;
	internalState.priority = priority;
	internalStates[taskIndex++] = internalState;
	xprintf("Created task '%s' Priority %d\n", pcTaskGetName(task_id), priority);

	// The image task initialises the HM0360 and takes pictures when asked
	task_id = image_task_createTask(--priority, wakeReason);
	internalState.task_id = task_id;
	internalState.getState = image_task_getState;
	internalState.stateString = image_task_getStateString;
	internalState.priority = priority;
	internalStates[taskIndex++] = internalState;
	xprintf("Created task '%s' Priority %d\n", pcTaskGetName(task_id), priority);

	task_id = blinky_task_createTask(--priority, wakeReason);
	internalState.task_id = task_id;
	internalState.getState = blinky_task_getState;
	internalState.stateString = blinky_task_getStateString;
	internalState.priority = priority;
	internalStates[taskIndex++] = internalState;
	xprintf("Created task '%s' Priority %d\n", pcTaskGetName(task_id), priority);

	numTasksRegistered = taskIndex;

	// A barrier so that a function is called when all tasks are ready in their for(;;) loop
	barrier_init(&startupBarrier, taskIndex, allTasksReady);

	// Also a barrier to entering DPD: the blinky, FatFS and image tasks must all be ready
	barrier_init(&shutdownBarrier, 3, blinky_task_sleepNow);

	xprintf("FreeRTOS scheduler started.\n");
	vTaskStartScheduler();

	for (;;) {
		// Should not get here...
	}

	return 0;
}
