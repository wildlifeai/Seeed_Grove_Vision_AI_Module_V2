/*
 * freertos_app.c
 *
 *  Created on: 12 Aug 2024
 *      Author: CGP
 *
 *  Application hooks required by FreeRTOS: static allocation for the idle and timer tasks,
 *  the idle hook and task-switched-in hook that drive inactivity detection, and the stack
 *  overflow hook.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted. The unused fault-register helper
 *  prvGetRegistersFromStack() was left behind (fault handlers are in hardfault_handler.c).
 */

/*********************************************** Includes ****************************************************/

#include <stdint.h>

#include "WE2_device.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

#include "xprintf.h"
#include "inactivity.h"

/*********************************************** Local Defines ***********************************************/

/************************************************ Local Types ************************************************/

/********************************************** Local Variables **********************************************/

/**************************************** Local Function Declarations ****************************************/

/**************************************** Local Function Definitions *****************************************/

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Called by FreeRTOS on every task switch.
 *
 * Used to determine when all tasks have been inactive for a time, in which case the chip can
 * enter deep power down (DPD). See FreeRTOSConfig.h.
 */
void vApplicationTaskSwitchedIn(void) {
	inactivity_on_task_switched_in();
}

/**
 * @brief Called when FreeRTOS enters the idle state.
 *
 * Lets the application add background functionality without a separate task. MUST NOT, UNDER
 * ANY CIRCUMSTANCES, CALL A FUNCTION THAT MIGHT BLOCK.
 *
 * If used this must be enabled in FreeRTOSConfig.h with: #define configUSE_IDLE_HOOK 1
 */
void vApplicationIdleHook(void) {
	inactivity_IdleHook();
}

/**
 * @brief Provides the memory used by the Idle task.
 *
 * configUSE_STATIC_ALLOCATION is set to 1, so the application must provide this.
 * The buffers are static so they still exist after this function exits.
 *
 * @param ppxIdleTaskTCBBuffer   Receives a pointer to the StaticTask_t for the Idle task.
 * @param ppxIdleTaskStackBuffer Receives a pointer to the Idle task's stack array.
 * @param pulIdleTaskStackSize   Receives the stack size in words (not bytes).
 */
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
		StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize) {
	static StaticTask_t xIdleTaskTCB;
	static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE + 100];

	*ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
	*ppxIdleTaskStackBuffer = uxIdleTaskStack;
	*pulIdleTaskStackSize = configMINIMAL_STACK_SIZE + 100;
}

/**
 * @brief Provides the memory used by the Timer service task.
 *
 * configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both 1, so the application must provide this.
 * The buffers are static so they still exist after this function exits.
 *
 * @param ppxTimerTaskTCBBuffer   Receives a pointer to the StaticTask_t for the Timer task.
 * @param ppxTimerTaskStackBuffer Receives a pointer to the Timer task's stack array.
 * @param pulTimerTaskStackSize   Receives the stack size in words (not bytes).
 */
void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
		StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize) {
	static StaticTask_t xTimerTaskTCB;
	static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

	*ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
	*ppxTimerTaskStackBuffer = uxTimerTaskStack;
	*pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

/**
 * @brief Called if a task stack overflow is detected.
 *
 * Used if configCHECK_FOR_STACK_OVERFLOW is set to 1 or 2 in FreeRTOSConfig.h. See
 * https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/02-Stack-usage-and-stack-overflow-checking
 *
 * @param xTask      Handle of the offending task.
 * @param pcTaskName Name of the offending task.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {

	xprintf("Stack overflow in task %d '%s'\n", (int) xTask, pcTaskName);

	/* Force an assert. */
	configASSERT(pcTaskName == 0);
}
