/*
 * sleep_mode.h
 *
 *  Created on: 2023/10/24
 *      Author: Himax
 *
 *  Deep power down (DPD) entry, and decoding of the reason for a wakeup.
 *
 *  Copied into ww500_minimal from ww500_md and reformatted. sleep_mode_enter_sleep()
 *  (PD mode with a CM55M timer wakeup) was left behind: it is not used.
 */

#ifndef SLEEP_MODE_H_
#define SLEEP_MODE_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

// Options for wake from DPD - each one bit
typedef enum {
	SLEEPMODE_WAKE_SOURCE_WAKE_PIN = 0x1,
	SLEEPMODE_WAKE_SOURCE_RTC = 0x2
} SLEEPMODE_WAKE_SOURCE_E;

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prints the reason for a wakeup, or "Cold boot" if there was none.
 */
void sleep_mode_print_event(uint32_t event, uint32_t event1);

/**
 * @brief Puts the processor in deep power down (DPD). Does not return.
 */
void sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_E wakeSource, uint16_t alarmDelay, bool verbose);

#ifdef __cplusplus
}
#endif

#endif  /* SLEEP_MODE_H_ */
