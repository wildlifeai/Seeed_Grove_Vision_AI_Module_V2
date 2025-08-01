/*
 * FreeRTOS Kernel V10.4.0
 * Copyright (C) 2019 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */


#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * Application specific definitions.
 *
 * These definitions should be adjusted for your particular hardware and
 * application requirements.
 *
 * THESE PARAMETERS ARE DESCRIBED WITHIN THE 'CONFIGURATION' SECTION OF THE
 * FreeRTOS API DOCUMENTATION AVAILABLE ON THE FreeRTOS.org WEB SITE.
 *
 * See http://www.freertos.org/a00110.html
 *----------------------------------------------------------*/
#include "WE2_device.h"
#if (defined(__ARMCC_VERSION) || defined(__GNUC__) || defined(__ICCARM__))
#include <stdint.h>

extern uint32_t SystemCoreClock;
#endif

/* Constants that describe the hardware and memory usage. */
#define configCPU_CLOCK_HZ                    (SystemCoreClock)
#define configTICK_RATE_HZ                    ((TickType_t)1000)
#define configTOTAL_HEAP_SIZE                 ((size_t)(50 * 1024))//((size_t)4096)
#define configMINIMAL_STACK_SIZE              ((uint16_t)256)
#define configSUPPORT_DYNAMIC_ALLOCATION      1
#if defined(FREERTOS_OSHAL)
#define configSUPPORT_STATIC_ALLOCATION       1
#else
#define configSUPPORT_STATIC_ALLOCATION       1//0
#endif

/* Constants related to the behaviour or the scheduler. */
#ifndef FREERTOS_OSHAL
#define configMAX_PRIORITIES                  5
#else
#define configMAX_PRIORITIES                  56
#endif
#define configUSE_PREEMPTION                  1
#define configUSE_TIME_SLICING                1
#define configIDLE_SHOULD_YIELD               1
#define configMAX_TASK_NAME_LEN               (10)
#define configUSE_16_BIT_TICKS                0

/* Software timer definitions. */
#define configUSE_TIMERS                      1
#define configTIMER_TASK_PRIORITY             2
#define configTIMER_QUEUE_LENGTH              5
#define configTIMER_TASK_STACK_DEPTH          (configMINIMAL_STACK_SIZE * 2)

/* Constants that build features in or out. */
#define configUSE_MUTEXES                     1
#define configUSE_RECURSIVE_MUTEXES           1
#define configUSE_COUNTING_SEMAPHORES         1
#define configUSE_QUEUE_SETS                  1
#define configUSE_TASK_NOTIFICATIONS          1
#define configUSE_TRACE_FACILITY              1
// CGP Added this so vTaskList is compiled:
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
// CGP can make a chnage here to turn off tickless idle
#define configUSE_TICKLESS_IDLE               1
//#define configUSE_TICKLESS_IDLE               0
#define configUSE_APPLICATION_TASK_TAG        0
#ifdef __GNU__
#define configUSE_NEWLIB_REENTRANT            1
#else
#define configUSE_NEWLIB_REENTRANT            0
#endif
#define configUSE_CO_ROUTINES                 0

/* Constants provided for debugging and optimisation assistance. */
#define configCHECK_FOR_STACK_OVERFLOW        2
#define configQUEUE_REGISTRY_SIZE             0
#define configASSERT( x )                     if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

/* Constants that define which hook (callback) functions should be used. */
#define configUSE_IDLE_HOOK                   1
#define configUSE_TICK_HOOK                   0
#define configUSE_DAEMON_TASK_STARTUP_HOOK    0
#define configUSE_MALLOC_FAILED_HOOK          0

// CGP - defines a function to call when tasks are switched, to determine inactivity
// Uncomment both of these lines together:
extern void vApplicationTaskSwitchedIn( void );
#define traceTASK_SWITCHED_IN() vApplicationTaskSwitchedIn()

/* Port specific configuration. */
//#define configENABLE_MPU                      0
#define configENABLE_FPU                      1
#define configENABLE_MVE                      0
#define configENABLE_TRUSTZONE                0
#define configMINIMAL_SECURE_STACK_SIZE       ((uint32_t)1024)
#define configRUN_FREERTOS_SECURE_ONLY        1

/* Cortex-M specific definitions. */
#ifdef __NVIC_PRIO_BITS
  /* __NVIC_PRIO_BITS will be specified when CMSIS is being used. */
  #define configPRIO_BITS                     __NVIC_PRIO_BITS
#else
  /* 7 priority levels */
  #define configPRIO_BITS                     3
#endif

/* The lowest interrupt priority that can be used in a call to a "set priority" function. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY       0x07

/* The highest interrupt priority that can be used by any interrupt service
 * routine that makes calls to interrupt safe FreeRTOS API functions.  DO NOT
 * CALL INTERRUPT SAFE FREERTOS API FUNCTIONS FROM ANY INTERRUPT THAT HAS A
 * HIGHER PRIORITY THAN THIS! (higher priorities are lower numeric values). */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY  5

/* Interrupt priorities used by the kernel port layer itself.  These are generic
 * to all Cortex-M ports, and do not rely on any particular library functions. */
#define configKERNEL_INTERRUPT_PRIORITY               (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* !!!! configMAX_SYSCALL_INTERRUPT_PRIORITY must not be set to zero !!!!
 * See http://www.FreeRTOS.org/RTOS-Cortex-M3-M4.html. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY          (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Set the following definitions to 1 to include the API function, or zero
 * to exclude the API function.  NOTE:  Setting an INCLUDE_ parameter to 0 is
 * only necessary if the linker does not automatically remove functions that are
 * not referenced anyway. */
#define INCLUDE_vTaskPrioritySet              1
#define INCLUDE_uxTaskPriorityGet             1
#define INCLUDE_vTaskDelete                   1
#define INCLUDE_vTaskSuspend                  1
#define INCLUDE_xTaskDelayUntil               1
#define INCLUDE_vTaskDelay                    1
#define INCLUDE_xTaskGetIdleTaskHandle        1
#define INCLUDE_xTaskAbortDelay               1
#define INCLUDE_xQueueGetMutexHolder          1
#define INCLUDE_xSemaphoreGetMutexHolder      1
#define INCLUDE_xTaskGetHandle                1
#define INCLUDE_uxTaskGetStackHighWaterMark   1
#define INCLUDE_uxTaskGetStackHighWaterMark2  1
#define INCLUDE_eTaskGetState                 1
#define INCLUDE_xTaskResumeFromISR            1
#define INCLUDE_xTimerPendFunctionCall        1
#define INCLUDE_xTaskGetSchedulerState        1
#define INCLUDE_xTaskGetCurrentTaskHandle     1

/* Map the FreeRTOS port interrupt handlers to their CMSIS standard names. */
#define xPortPendSVHandler                    PendSV_Handler
#define vPortSVCHandler                       SVC_Handler
#ifndef FREERTOS_OSHAL
#define xPortSysTickHandler                   SysTick_Handler
#else
/* Ensure Cortex-M port compatibility. */
#define SysTick_Handler                         xPortSysTickHandler
#endif
#if (defined(__ARMCC_VERSION) || defined(__GNUC__) || defined(__ICCARM__))
/* Include debug event definitions */
#ifdef FREERTOS_OSHAL
#include "freertos_evr.h"
#endif
#endif

#endif /* FREERTOS_CONFIG_H */
