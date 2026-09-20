/*
 * rtc_util.h
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  Helpers for the RTC: clock enable/disable around DPD, reading and setting the time,
 *  ISO string conversion, and adding seconds to a time (for setting an alarm).
 *
 *  Derived from the RTC functions in exif_utc.c (ww500_md) without the EXIF, GPS and FatFS parts.
 *
 *  Conventions (as in exif_utc.c): rtc_time.tm_year is a full 4-digit year (e.g. 2026) and
 *  rtc_time.tm_mon is 1-12.
 *
 *  Note: setting the RTC takes 1-2 s, and suppresses interrupts for about 1.4 s.
 *
 *  Note: the first read after waking from DPD returns the time from before DPD, unless
 *  rtc_util_getTimeAfterDpd() is used, which takes about 1 s to synchronise the counter.
 */

#ifndef RTC_UTIL_H_
#define RTC_UTIL_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "hx_drv_rtc.h"

/********************************************** Global Defines ***********************************************/

// Length of UTC timestamp "YYYY-MM-DDTHH:MM:SSZ", plus trailing '\0'
#define RTC_UTIL_UTC_STRING_LENGTH 21

/*********************************************** Global Types ************************************************/

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enables the RTC clocks and sets the time from an ISO string.
 */
RTC_ERROR_E rtc_util_init(const char *str);

/**
 * @brief Enables the clocks needed by the RTC. Call when exiting DPD.
 */
void rtc_util_clkEnable(void);

/**
 * @brief Disables the clocks the RTC does not need in DPD. Call when entering DPD.
 */
void rtc_util_clkDisable(void);

/**
 * @brief Reads the RTC hardware.
 */
RTC_ERROR_E rtc_util_getTime(rtc_time *tm);

/**
 * @brief Reads the RTC hardware for the first time after waking from DPD. Slow: about 1 s.
 */
RTC_ERROR_E rtc_util_getTimeAfterDpd(rtc_time *tm);

/**
 * @brief Sets the RTC hardware. Slow: see the note in the file header.
 */
RTC_ERROR_E rtc_util_setTime(rtc_time *tm);

/**
 * @brief Converts an ISO UTC string "YYYY-MM-DDTHH:MM:SSZ" to a rtc_time object.
 */
RTC_ERROR_E rtc_util_stringToTime(const char *str, rtc_time *tm);

/**
 * @brief Converts a rtc_time object to an ISO UTC string. Needs RTC_UTIL_UTC_STRING_LENGTH bytes.
 */
RTC_ERROR_E rtc_util_timeToString(const rtc_time *tm, char *str, uint8_t length);

/**
 * @brief Reads the RTC hardware and converts the result to an ISO UTC string.
 */
RTC_ERROR_E rtc_util_getString(char *str, uint8_t length);

/**
 * @brief Returns a time which is later than the one supplied by a number of seconds.
 */
rtc_time rtc_util_addSeconds(rtc_time input_rtc, time_t seconds_to_add);

#ifdef __cplusplus
}
#endif

#endif /* RTC_UTIL_H_ */
