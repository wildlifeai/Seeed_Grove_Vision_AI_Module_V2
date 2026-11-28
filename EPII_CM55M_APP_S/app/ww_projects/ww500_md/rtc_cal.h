/*
 * rtc_cal.h
 *
 *  Created on: 31 Oct 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  RTC calibration (#245). The RTC and the DPD wake alarm run from the RC32K oscillator, which runs
 *  2 to 5 % fast and differs from board to board. Its trim register has no effect during DPD. So the
 *  board measures the oscillator itself, while awake: it times one RTC second, between two RTC alarm
 *  interrupts, against the core's cycle counter (400 MHz from the 24 MHz crystal). The result, in ppm,
 *  is kept in the board record (xip_state.c) and each DPD alarm is lengthened to match.
 *
 *  Measured before sleep on a cold boot, when there is no saved value, and once a day of RTC time.
 *  A measurement keeps the core busy for 2 to 3 s. Awake and asleep rates agreed within 0 to 0.2 %
 *  on the one board compared (31 Oct 2026). See _Documentation/development reports/2026-10-27_XIP_save_state/
 *  CLAUDE_time_accuracy.md.
 *
 *  Only the sleep is corrected: the RTC's time of day still runs fast between 'setutc's.
 */

#ifndef RTC_CAL_H_
#define RTC_CAL_H_

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*********************************************** Global Defines **********************************************/

#define RTC_CAL_INTERVAL_S			86400	// measure again after this many RTC seconds
#define RTC_CAL_MEASURE_S			1		// RTC seconds timed per measurement (repeatable to about 10 ppm)
// Added to the measured value for the sleep: the difference between the awake and asleep rates.
// 0 until measured on more boards (0 to -2000 ppm on the first one)
#define RTC_CAL_SLEEP_OFFSET_PPM	0
// A measurement further from 0 than this is taken as a failure
#define RTC_CAL_PPM_LIMIT			200000

/*********************************************** Global Types ************************************************/


/*********************************************** Global Variables ********************************************/


/*********************************************** Global Function Declarations *********************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Time RTC seconds against the crystal. Keeps the core busy for up to 2 s more than 'seconds'.
 */
bool rtc_cal_measure(uint8_t seconds, int32_t *ppm);

/**
 * @brief Measure now and save the result in the board record (the 'rtccal measure' command).
 */
bool rtc_cal_measureAndSave(int32_t *ppm);

/**
 * @brief Before sleep: measure if due, and load the correction for sleep_mode_enter_dpd().
 */
void rtc_cal_beforeSleep(bool coldBoot);

/**
 * @brief The correction for this sleep, if there is one.
 */
bool rtc_cal_getCorrection(int32_t *ppm);

/**
 * @brief The RTC seconds to ask for, so the sleep lasts 'seconds' real seconds.
 */
uint16_t rtc_cal_correctAlarm(uint16_t seconds);

/**
 * @brief Turn the calibration off (no correction, no measuring) or back on, in the board record.
 */
int rtc_cal_setOff(bool off);

/**
 * @brief Format ppm as a percentage with two decimals, e.g. "-3.91".
 */
void rtc_cal_ppmToPercent(int32_t ppm, char *str, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RTC_CAL_H_ */
