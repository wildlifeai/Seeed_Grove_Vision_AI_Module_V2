/*
 * inactivity.h
 *
 *  Created on: 13 Apr 2025
 *      Author: charl
 *
 *  Detects when all tasks have been inactive for a period, then makes a callback which would
 *  typically enter DPD.
 *
 *  It relies on:
 *   (a) inactivity_on_task_switched_in(), called from vApplicationTaskSwitchedIn() on every
 *       task switch, and
 *   (b) inactivity_IdleHook(), called from vApplicationIdleHook().
 *  Both are in freertos_app.c, and need configUSE_IDLE_HOOK and the task-switched-in trace
 *  hook to be enabled in FreeRTOSConfig.h.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted. The unused USETIMER alternative
 *  and inactivity_reset() were left behind.
 */

#ifndef INACTIVITY_H_
#define INACTIVITY_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialises inactivity detection.
 */
void inactivity_init(uint32_t timeout_ms, void (*callback)(void));

/**
 * @brief Idle hook handler. Called from vApplicationIdleHook().
 */
void inactivity_IdleHook(void);

/**
 * @brief Resets the idle tracking when a non-idle task runs. Called from vApplicationTaskSwitchedIn().
 */
void inactivity_on_task_switched_in(void);

/**
 * @brief Returns the inactivity period in ms.
 */
uint32_t inactivity_getPeriod(void);

/**
 * @brief Sets the inactivity period in ms.
 */
void inactivity_setPeriod(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* INACTIVITY_H_ */
