/*
 * app_msg.h
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  Messages passed between the tasks of the ww500_minimal app, using FreeRTOS queues.
 *
 *  Derived from ww500_md/app_msg.h, which is mostly camera data path events that are not
 *  needed here. Each task has a block of 0x100 event values.
 */

#ifndef APP_MSG_H_
#define APP_MSG_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

/**
 * \enum APP_MSG_EVENT_E
 * \brief Events sent to the tasks
 */
typedef enum {
	APP_MSG_NONE								= 0x0000,

	// Messages directed to the blinky task
	APP_MSG_BLINKYTASK_FIRST					= 0x0100,
	APP_MSG_BLINKYTASK_START					= 0x0100,	// Start (or restart) the blinking. msg_data = the period in ms
	APP_MSG_BLINKYTASK_STOP						= 0x0101,	// Stop the blinking, so the tasks become inactive
	APP_MSG_BLINKYTASK_INACTIVITY				= 0x0102,	// All tasks are inactive: enter DPD
	APP_MSG_BLINKYTASK_LAST						= 0x0103,

	// Messages directed to the CLI task
	APP_MSG_CLITASK_FIRST						= 0x0200,
	APP_MSG_CLITASK_RXCHAR						= 0x0200,	// A character has arrived from the console UART
	APP_MSG_CLITASK_LAST						= 0x0201,
} APP_MSG_EVENT_E;

/**
 * \struct APP_MSG_T
 * \brief A message: an event and two values whose meaning depends on the event
 */
typedef struct {
	APP_MSG_EVENT_E  	msg_event;		// An event value, from this file
	uint32_t 			msg_data;		// A data value, often a pointer to a buffer or a structure
	uint32_t 			msg_parameter;	// A second data value, such as the length of the buffer in 'data'
} APP_MSG_T;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#endif /* APP_MSG_H_ */
