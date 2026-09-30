/*
 * rtc_util.c
 *
 *  Created on: 20 Sep 2026
 *      Author: Charles Palmer
 *
 *  Helpers for the RTC. See rtc_util.h for a description.
 */

/*********************************************** Includes ****************************************************/

#include <stdio.h>
#include <string.h>

#include "hx_drv_rtc.h"
#include "hx_drv_scu.h"

#include "rtc_util.h"

/*********************************************** Local Defines ***********************************************/

/************************************************ Local Types ************************************************/

/********************************************** Local Variables **********************************************/

/**************************************** Local Function Declarations ****************************************/

static int isLeapYear(int year);
static int daysInMonth(int mon, int year);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Checks if a year is a leap year. Helper for rtc_util_addSeconds().
 *
 * @param year Full 4-digit year (e.g. 2026), not years since 1900.
 * @return 1 if a leap year, otherwise 0.
 */
static int isLeapYear(int year) {
    return ((year % 4 == 0) && (year % 100 != 0 || year % 400 == 0));
}

/**
 * @brief Gets the number of days in a given month/year. Helper for rtc_util_addSeconds().
 *
 * @param mon  Month, 1-12.
 * @param year Full 4-digit year (see isLeapYear()).
 * @return The number of days in the month.
 */
static int daysInMonth(int mon, int year) {
    static const int days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

    if (mon < 1 || mon > 12) {
        return 31; // defensive fallback; should not happen
    }

    if (mon == 2 && isLeapYear(year)) {
        return 29;
    }

    return days[mon - 1];
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Enables the RTC clocks and sets the time from an ISO string.
 *
 * @param str ISO string e.g. "2024-01-01T00:00:00Z".
 * @return RTC_NO_ERROR on success, otherwise an error code.
 */
RTC_ERROR_E rtc_util_init(const char *str) {
	RTC_ERROR_E ret;
	rtc_time tm;

	// Required to get the RTC working
	rtc_util_clkEnable();

	ret = rtc_util_stringToTime(str, &tm);

	if (ret != RTC_NO_ERROR) {
		return ret;
	}

	return rtc_util_setTime(&tm);
}

/**
 * @brief Enables the clocks needed by the RTC. Call when exiting DPD.
 */
void rtc_util_clkEnable(void) {
	SCU_PDAON_CLKEN_CFG_T aonclken;

	aonclken.rtc0_clk_en = 1;/*!< RTC0 Clock enable */
	aonclken.rtc1_clk_en = 1;/*!< RTC1 Clock enable */
	aonclken.rtc2_clk_en = 1;/*!< RTC2 Clock enable */
	aonclken.pmu_clk_en = 1;/*!< PMU Clock enable */
	aonclken.aon_gpio_clk_en = 1;/*!< AON GPIO Clock enable */
	aonclken.aon_swreg_clk_en = 1;/*!< AON SW REG Clock enable */
	aonclken.antitamper_clk_en = 1;/*!< ANTI TAMPER Clock enable */
	hx_drv_scu_set_pdaon_clken_cfg(aonclken);
}

/**
 * @brief Disables the clocks the RTC does not need in DPD. Call when entering DPD.
 */
void rtc_util_clkDisable(void) {
	SCU_PDAON_CLKEN_CFG_T aonclken;

	aonclken.rtc0_clk_en = 1;/*!< RTC0 Clock enable */
	aonclken.rtc1_clk_en = 0;/*!< RTC1 Clock enable */
	aonclken.rtc2_clk_en = 0;/*!< RTC2 Clock enable */
	aonclken.pmu_clk_en = 1;/*!< PMU Clock enable */
	aonclken.aon_gpio_clk_en = 0;/*!< AON GPIO Clock enable */
	aonclken.aon_swreg_clk_en = 1;/*!< AON SW REG Clock enable */
	aonclken.antitamper_clk_en = 0;/*!< ANTI TAMPER Clock enable */
	hx_drv_scu_set_pdaon_clken_cfg(aonclken);
}

/**
 * @brief Reads the RTC hardware.
 *
 * @param tm Pointer to a rtc_time object to receive the time.
 * @return RTC_NO_ERROR on success, otherwise an error code.
 */
RTC_ERROR_E rtc_util_getTime(rtc_time *tm) {
	return hx_drv_rtc_read_time(RTC_ID_0, tm, RTC_TIME_AFTER_DPD_1ST_READ_NO);
}

/**
 * @brief Reads the RTC hardware for the first time after waking from DPD.
 *
 * The RTC counter needs to synchronise after DPD, so this takes about 1 s (it was measured at
 * 961 ms in ww500_md). A plain rtc_util_getTime() as the first read after DPD returns the time
 * at which DPD was entered.
 *
 * @param tm Pointer to a rtc_time object to receive the time.
 * @return RTC_NO_ERROR on success, otherwise an error code.
 */
RTC_ERROR_E rtc_util_getTimeAfterDpd(rtc_time *tm) {
	return hx_drv_rtc_read_time(RTC_ID_0, tm, RTC_TIME_AFTER_DPD_1ST_READ_YES);
}

/**
 * @brief Sets the RTC hardware.
 *
 * This takes 1-2 s and suppresses interrupts for about 1.4 s.
 *
 * @param tm Pointer to a rtc_time object holding the new time.
 * @return RTC_NO_ERROR on success, otherwise an error code.
 */
RTC_ERROR_E rtc_util_setTime(rtc_time *tm) {
	return hx_drv_rtc_set_time(RTC_ID_0, tm);
}

/**
 * @brief Converts an ISO UTC string to a rtc_time object.
 *
 * The ISO 8601 format is "YYYY-MM-DDTHH:MM:SSZ", e.g. '2025-03-21T09:05:00Z'.
 *
 * @param str Pointer to the string containing the UTC time.
 * @param tm  Pointer to a rtc_time object to receive the result.
 * @return RTC_NO_ERROR on success, RTC_ERROR_INVALID_PARAMETERS if the string is malformed.
 */
RTC_ERROR_E rtc_util_stringToTime(const char *str, rtc_time *tm) {
	int charsRead;

	if ((sscanf(str, "%4d-%2d-%2dT%2d:%2d:%2dZ%n",
			&tm->tm_year, &tm->tm_mon, &tm->tm_mday,
			&tm->tm_hour, &tm->tm_min, &tm->tm_sec, &charsRead) != 6)
	|| (charsRead != strlen(str))){
		return RTC_ERROR_INVALID_PARAMETERS;
	}

	return RTC_NO_ERROR;
}

/**
 * @brief Converts a rtc_time object to an ISO UTC string.
 *
 * The ISO 8601 format is "YYYY-MM-DDTHH:MM:SSZ", e.g. '2025-03-21T09:05:00Z'.
 *
 * @param tm     Pointer to a rtc_time object.
 * @param str    Buffer to receive the string.
 * @param length Length of the buffer, which must be at least RTC_UTIL_UTC_STRING_LENGTH.
 * @return RTC_NO_ERROR on success, RTC_ERROR_INVALID_PARAMETERS if the buffer is too short.
 */
RTC_ERROR_E rtc_util_timeToString(const rtc_time *tm, char *str, uint8_t length) {

	if (length < RTC_UTIL_UTC_STRING_LENGTH) {
		return RTC_ERROR_INVALID_PARAMETERS;
	}

	snprintf(str, length, "%04d-%02d-%02dT%02d:%02d:%02dZ",
			tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

	return RTC_NO_ERROR;
}

/**
 * @brief Reads the RTC hardware and converts the result to an ISO UTC string.
 *
 * @param str    Buffer to receive the string.
 * @param length Length of the buffer, which must be at least RTC_UTIL_UTC_STRING_LENGTH.
 * @return RTC_NO_ERROR on success, otherwise an error code.
 */
RTC_ERROR_E rtc_util_getString(char *str, uint8_t length) {
	RTC_ERROR_E ret;
	rtc_time tm = {0};

	ret = rtc_util_getTime(&tm);

	if (ret != RTC_NO_ERROR) {
		return ret;
	}

	return rtc_util_timeToString(&tm, str, length);
}

/**
 * @brief Returns a time which is later than the one supplied by a number of seconds.
 *
 * Can be used to create an rtc_time object in the future - e.g. for setting an alarm.
 * tm_wday and tm_yday are left unchanged.
 *
 * @param input_rtc      A time/date.
 * @param seconds_to_add The number of seconds to add.
 * @return The later time/date.
 */
rtc_time rtc_util_addSeconds(rtc_time input_rtc, time_t seconds_to_add) {
    rtc_time t = input_rtc;

    // Add seconds
    t.tm_sec += seconds_to_add;

    // Normalize seconds to minutes
    if (t.tm_sec >= 60) {
        t.tm_min += t.tm_sec / 60;
        t.tm_sec = t.tm_sec % 60;
    }

    // Normalize minutes to hours
    if (t.tm_min >= 60) {
        t.tm_hour += t.tm_min / 60;
        t.tm_min = t.tm_min % 60;
    }

    // Normalize hours to days
    if (t.tm_hour >= 24) {
        t.tm_mday += t.tm_hour / 24;
        t.tm_hour = t.tm_hour % 24;
    }

    // Normalize days to months/years
    while (1) {
        int dim = daysInMonth(t.tm_mon, t.tm_year);

        if (t.tm_mday <= dim) {
        	break;
        }

        t.tm_mday -= dim;
        t.tm_mon += 1;

        // 1-indexed months (1-12): only wrap once we go PAST December, not
        // when we merely arrive at it, and wrap to January (1), not 0.
        if (t.tm_mon > 12) {
            t.tm_mon = 1;
            t.tm_year += 1;
        }
    }

    return t;
}
