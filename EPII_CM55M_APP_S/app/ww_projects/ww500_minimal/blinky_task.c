/*
 * blinky_task.c
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  FreeRTOS task that alternately blinks the LEDs on PB9 and PB10, and owns entry to DPD.
 *  See blinky_task.h for a description.
 */

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "printf_x.h"
#include "xprintf.h"

#include "app_msg.h"
#include "barrier.h"
#include "blinky_task.h"
#include "power_diag.h"
#include "rtc_util.h"
#include "sleep_mode.h"
#include "ww500_minimal.h"

/*********************************************** Local Defines ***********************************************/

#define BLINKY_TASK_QUEUE_LEN			5

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

extern Barrier_t startupBarrier;   // Object that calls a function when all tasks are ready
extern Barrier_t shutdownBarrier;  // Object that calls a function when all tasks are ready to shut down

/********************************************** Local Variables **********************************************/

// The queue that other tasks use to send messages to this task
static QueueHandle_t xBlinkyTaskQueue;

// This is the handle of the task
static TaskHandle_t blinky_task_id;

static volatile BLINKY_TASK_STATE_E blinkyState = BLINKY_TASK_STATE_UNINIT;

// How long to blink before stopping, decided from the wake reason. Set by the CLI.
static volatile uint32_t runTimeMs;

// Time between LED changes. Set by the CLI.
static volatile uint32_t blinkPeriodMs = BLINKY_TASK_PERIOD_MS;

// Time between prints of the RTC time while blinking (0 = off). Set by the CLI.
static volatile uint32_t timePrintPeriodMs = WW500_MINIMAL_TIME_PRINT_PERIOD_MS;

// Strings for each of the states. Values must match BLINKY_TASK_STATE_E in blinky_task.h
static const char * blinkyStateString[BLINKY_TASK_NUMSTATES] = {
		"Uninitialised",
		"Blinking",
		"Stopped",
		"Sleeping",
};

/**************************************** Local Function Declarations ****************************************/

static void vBlinkyTask(void *pvParameters);
static void setLeds(bool pb9, bool pb10);
static void printTime(void);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Sets both LEDs.
 *
 * @param pb9  True to switch on the LED on PB9.
 * @param pb10 True to switch on the LED on PB10.
 */
static void setLeds(bool pb9, bool pb10) {
	ww500_minimal_ledPb9(pb9);
	ww500_minimal_ledPb10(pb10);
}

/**
 * @brief Prints the RTC time.
 */
static void printTime(void) {
	char timeString[RTC_UTIL_UTC_STRING_LENGTH];

	if (rtc_util_getString(timeString, sizeof(timeString)) == RTC_NO_ERROR) {
		XP_LT_GREEN;
		xprintf(" >>> %s\n", timeString);
		XP_WHITE;
	}
}

/**
 * @brief The FreeRTOS task.
 *
 * While blinking, the LEDs change every BLINKY_TASK_PERIOD_MS. After the run time the task
 * stops blinking and waits for messages.
 *
 * @param pvParameters Not used.
 */
static void vBlinkyTask(void *pvParameters) {
	APP_MSG_T rxMessage;
	TickType_t startTick;
	TickType_t lastPrintTick;
	TickType_t waitTicks;
	bool pb9State = false;

	XP_CYAN;
	// Observing these messages confirms the initialisation sequence
	xprintf("Starting Blinky Task\n");
	XP_WHITE;

	startTick = xTaskGetTickCount();
	lastPrintTick = startTick;
	blinkyState = BLINKY_TASK_STATE_BLINKING;

	barrier_ready(&startupBarrier);		// Call a function when every task reaches this point

	for (;;) {
		// Wake on a timer while blinking, otherwise wait for a message
		waitTicks = (blinkyState == BLINKY_TASK_STATE_BLINKING) ? pdMS_TO_TICKS(blinkPeriodMs) : portMAX_DELAY;

		if (xQueueReceive(xBlinkyTaskQueue, &rxMessage, waitTicks) == pdTRUE) {

			switch (rxMessage.msg_event) {

			case APP_MSG_BLINKYTASK_START:
				if (rxMessage.msg_data != 0) {
					blinkPeriodMs = rxMessage.msg_data;
				}
				startTick = xTaskGetTickCount();
				lastPrintTick = startTick;
				blinkyState = BLINKY_TASK_STATE_BLINKING;
				break;

			case APP_MSG_BLINKYTASK_STOP:
				setLeds(false, false);
				blinkyState = BLINKY_TASK_STATE_STOPPED;
				break;

			case APP_MSG_BLINKYTASK_INACTIVITY:
				// Every task has been idle for the inactivity period. Stop blinking and report to the barrier.
				// When the other tasks have also reported the barrier calls blinky_task_sleepNow().
				blinkyState = BLINKY_TASK_STATE_STOPPED;
				setLeds(false, false);
				barrier_ready(&shutdownBarrier);
				break;

			default:
				xprintf("Blinky Task: unexpected event 0x%04x\n", rxMessage.msg_event);
				break;
			}
		}
		else if (blinkyState == BLINKY_TASK_STATE_BLINKING) {
			// Timed out: change the LEDs
			pb9State = !pb9State;
			setLeds(pb9State, !pb9State);

			if ((timePrintPeriodMs != 0) && (ww500_minimal_getElapsedMs(lastPrintTick) >= timePrintPeriodMs)) {
				lastPrintTick = xTaskGetTickCount();
				printTime();
			}

			if (ww500_minimal_getElapsedMs(startTick) >= runTimeMs) {
				setLeds(false, false);
				blinkyState = BLINKY_TASK_STATE_STOPPED;
			}
		}
	}
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Creates the task and its queue. Call before the scheduler is started.
 *
 * @param priority   The FreeRTOS priority for the task.
 * @param wakeReason The reason for this wakeup. Cold boots blink for longer.
 * @return The handle of the task.
 */
TaskHandle_t blinky_task_createTask(int8_t priority, WW500_MINIMAL_WAKE_REASON_E wakeReason) {

	if (priority < 0) {
		priority = 0;
	}

	runTimeMs = (wakeReason == WW500_MINIMAL_WAKE_REASON_COLD) ?
			WW500_MINIMAL_RUN_TIME_COLD_MS : WW500_MINIMAL_RUN_TIME_WARM_MS;

	xBlinkyTaskQueue = xQueueCreate(BLINKY_TASK_QUEUE_LEN, sizeof(APP_MSG_T));
	if (xBlinkyTaskQueue == 0) {
		xprintf("Failed to create xBlinkyTaskQueue\n");
		configASSERT(0);
	}

	if (xTaskCreate(vBlinkyTask, (const char *)"Blinky",
			configMINIMAL_STACK_SIZE * 4,
			NULL, priority,
			&blinky_task_id) != pdPASS)  {
		xprintf("Failed to create vBlinkyTask\n");
		configASSERT(0);
	}

	return blinky_task_id;
}

/**
 * @brief Returns the internal state as a number.
 *
 * @return The state, a BLINKY_TASK_STATE_E value.
 */
uint16_t blinky_task_getState(void) {
	return blinkyState;
}

/**
 * @brief Returns the internal state as a string.
 *
 * @return A pointer to a constant string.
 */
const char * blinky_task_getStateString(void) {
	return blinkyStateString[blinkyState];
}

/**
 * @brief Tells the task that all tasks are inactive, so it should enter DPD.
 *
 * Called from the FreeRTOS idle hook (see inactivity.c) so it must not block: the message is
 * dropped if the queue is full.
 */
void blinky_task_notifyInactivity(void) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = APP_MSG_BLINKYTASK_INACTIVITY;
	sendMsg.msg_data = 0;
	sendMsg.msg_parameter = 0;

	xQueueSend(xBlinkyTaskQueue, (void *)&sendMsg, 0);
}

/**
 * @brief Switches the LEDs off and enters DPD. Does not return.
 *
 * Called by the shutdown barrier, in the context of the last task to report to it (the blinky task or the FatFS
 * task), once every task is ready.
 *
 * Wakes on the WAKE signal (PA0, level high) or after the alarm period (see ww500_minimal_getAlarmPeriod()).
 * The LEDs are driven low first: the state of PB9 and PB10 in DPD is not known.
 *
 * Any clocks switched off by the 'clkoff' experiment are switched back on first. The bootloader
 * has to read the application back from flash on every wake, and a wake with the flash interface
 * clocks still off did not resume (see doc/power_investigation.md). This does nothing if no clocks
 * have been switched off.
 */
void blinky_task_sleepNow(void) {
	blinkyState = BLINKY_TASK_STATE_SLEEPING;

	setLeds(false, false);

	power_diag_restoreClocks();

	sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_WAKE_PIN | SLEEPMODE_WAKE_SOURCE_RTC,
			ww500_minimal_getAlarmPeriod(), false);
}

/**
 * @brief Starts the blinking with a period, or stops it if the period is 0.
 *
 * Starting restarts the run time. Stopping makes all tasks idle, so DPD follows after the
 * inactivity period.
 *
 * @param periodMs The time between LED changes in ms, or 0 to stop blinking.
 * @return true if the message was queued.
 */
bool blinky_task_setPeriod(uint32_t periodMs) {
	APP_MSG_T sendMsg;

	sendMsg.msg_event = (periodMs != 0) ? APP_MSG_BLINKYTASK_START : APP_MSG_BLINKYTASK_STOP;
	sendMsg.msg_data = periodMs;
	sendMsg.msg_parameter = 0;

	return (xQueueSend(xBlinkyTaskQueue, (void *)&sendMsg, WW500_MINIMAL_QUEUE_SEND_TICKS) == pdTRUE);
}

/**
 * @brief Sets how long the task blinks before it stops.
 *
 * Measured from when blinking started, so it applies immediately if still blinking.
 *
 * @param newRunTimeMs The run time in ms.
 */
void blinky_task_setRunTime(uint32_t newRunTimeMs) {
	runTimeMs = newRunTimeMs;
}

/**
 * @brief Sets how often the time is printed while blinking.
 *
 * @param periodMs The time between prints in ms. 0 turns printing off.
 */
void blinky_task_setTimePrintPeriod(uint32_t periodMs) {
	timePrintPeriodMs = periodMs;
}
