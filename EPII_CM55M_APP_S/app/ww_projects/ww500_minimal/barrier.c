/*
 * barrier.c
 *
 *  Created on: 19 Nov 2025
 *      Author: charl
 *
 *  A simple barrier mechanism for FreeRTOS tasks. See barrier.h for a description.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted.
 */

/*********************************************** Includes ****************************************************/

#include "barrier.h"

/*********************************************** Local Defines ***********************************************/

/********************************************** Local Variables **********************************************/

/**************************************** Local Function Declarations ****************************************/

/**************************************** Local Function Definitions *****************************************/

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Initialises a barrier.
 *
 * Called once, before the tasks that use it are running.
 *
 * @param b          Pointer to a Barrier_t object.
 * @param totalTasks The number of barrier_ready() calls that must be made before the callback is invoked.
 * @param cb         Function to call when the last task is ready.
 */
void barrier_init(Barrier_t *b, uint32_t totalTasks, BarrierCallback_t cb) {
    b->totalTasks = totalTasks;
    b->readyCount = 0;
    b->callbackCalled = false;
    b->callback = cb;

    b->mutex = xSemaphoreCreateMutex();
    configASSERT(b->mutex != NULL);
}

/**
 * @brief Called by each task when it is ready.
 *
 * When all tasks have called this function, the callback is executed (once), in the context
 * of the last task to call.
 *
 * @param b Pointer to a Barrier_t object.
 */
void barrier_ready(Barrier_t *b) {
    xSemaphoreTake(b->mutex, portMAX_DELAY);

    b->readyCount++;

    bool callNow =
        (b->readyCount == b->totalTasks) &&
        (!b->callbackCalled);

    if (callNow) {
        b->callbackCalled = true;
    }

    xSemaphoreGive(b->mutex);

    /* Call outside the mutex to avoid deadlocks or priority inversions. */
    if (callNow && b->callback) {
        b->callback();
    }
}
