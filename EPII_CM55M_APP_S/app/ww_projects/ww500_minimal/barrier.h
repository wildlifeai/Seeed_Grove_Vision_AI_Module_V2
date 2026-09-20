/*
 * barrier.h - FreeRTOS Task Barrier
 *
 *  Created on: 19 Nov 2025
 *      Author: charl
 *
 *  A simple barrier for FreeRTOS tasks. Each task calls barrier_ready() when it has reached
 *  its "ready" point (typically after initialisation but before its main for(;;) loop).
 *  The last task to arrive calls a user-provided callback, exactly once, outside the mutex
 *  so that the callback cannot deadlock against it.
 *
 *  Usage:
 *   1. Define one Barrier_t (and declare it extern elsewhere).
 *   2. Before starting the scheduler: barrier_init(&b, numberOfTasks, callback);
 *   3. In each task, when ready: barrier_ready(&b);
 *
 *  The barrier counts calls, not tasks: numberOfTasks must equal the number of
 *  barrier_ready() calls that will be made.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted.
 */

#ifndef BARRIER_H_
#define BARRIER_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

typedef void (*BarrierCallback_t)(void);

typedef struct {
    uint32_t totalTasks;
    uint32_t readyCount;
    bool callbackCalled;
    SemaphoreHandle_t mutex;
    BarrierCallback_t callback;
} Barrier_t;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

/**
 * @brief Initialises a barrier. Call once, before the tasks that use it are running.
 */
void barrier_init(Barrier_t *b, uint32_t totalTasks, BarrierCallback_t cb);

/**
 * @brief Called by each task when it is ready. The last caller triggers the callback.
 */
void barrier_ready(Barrier_t *b);

#endif /* BARRIER_H_ */
