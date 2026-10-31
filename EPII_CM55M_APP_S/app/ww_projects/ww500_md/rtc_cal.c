/*
 * rtc_cal.c
 *
 *  Created on: 31 Oct 2026
 *      Author: Claude (Opus 5.5), with Charles Palmer
 *
 *  RTC calibration (#245): measures the RC32K oscillator against the crystal while awake, keeps the
 *  result in the board record and lengthens each DPD alarm to match. See rtc_cal.h.
 *
 *  The measurement sets the RTC alarm for 2 s ahead (so it cannot be missed if the second changes just
 *  after the RTC is read), then for 'seconds' after that, and notes the cycle counter in each alarm
 *  interrupt. If the interrupt does not come, the alarm's raw status is polled instead. The core spins
 *  meanwhile: the cycle counter stops if it idles. The alarm is left disabled; sleep_mode_enter_dpd()
 *  sets it again.
 *
 *  Not for use while the NN is running: reading and writing the board record switches XIP off.
 */

/*********************************************** Includes ****************************************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "WE2_device.h"		// DWT cycle counter and SystemCoreClock
#include "hx_drv_rtc.h"
#include "xprintf.h"

#include "exif_utc.h"
#include "xip_state.h"
#include "rtc_cal.h"

/*********************************************** Local Defines **********************************************/

#define FIRST_ALARM_S			2		// the first alarm: far enough ahead that it cannot be missed
#define ALARM_MARGIN_MS			500		// wait this much longer than the alarm is due
#define PPM_SCALE				1000000

/*********************************************** Local Types ************************************************/


/*********************************************** Local Variables ********************************************/

// Set by alarmCallback(): the cycle counter when the RTC alarm interrupt came
static volatile uint32_t isrCycles;
static volatile bool isrFired;

// The correction for this sleep, set by rtc_cal_beforeSleep() or rtc_cal_measureAndSave()
static int32_t correctionPpm;
static bool correctionValid = false;

/*********************************************** Local Function Declarations *********************************/

static void alarmCallback(uint32_t event);
static bool waitForAlarm(rtc_time alarmTime, uint32_t *cycles, uint32_t timeoutMs);
static bool isDue(const XipStateBoard_t *board, bool coldBoot, uint32_t now);
static bool measureAndStore(XipStateBoard_t *board, int32_t *ppm);

/*********************************************** Local Function Definitions *********************************/

/**
 * @brief RTC alarm callback: notes the cycle counter.
 *
 * Runs in the RTC interrupt.
 *
 * @param event  the driver's interrupt event (unused)
 */
static void alarmCallback(uint32_t event) {
	(void) event;
	isrCycles = DWT->CYCCNT;
	isrFired = true;
}

/**
 * @brief Sets the RTC alarm, then spins until it fires.
 *
 * Takes the cycle count from the alarm interrupt if it comes, otherwise from polling the alarm's raw
 * interrupt status.
 *
 * @param alarmTime  the RTC time to fire at
 * @param cycles     output: the cycle counter when the alarm fired
 * @param timeoutMs  how long to wait
 * @return true if the alarm fired before the timeout
 */
static bool waitForAlarm(rtc_time alarmTime, uint32_t *cycles, uint32_t timeoutMs) {
	rtc_wkalrm alarm;
	uint32_t status = 0;
	uint32_t start;
	uint32_t cyclesPerMs = SystemCoreClock / 1000;

	isrFired = false;
	hx_drv_rtc_clear_alarm_int_status(RTC_ID_0);
	alarm.enabled = 1;
	alarm.pending = 0;
	alarm.time = alarmTime;

	start = DWT->CYCCNT;
	hx_drv_rtc_set_alarm(RTC_ID_0, &alarm, alarmCallback);

	while ((DWT->CYCCNT - start) / cyclesPerMs < timeoutMs) {
		if (isrFired) {
			*cycles = isrCycles;
			return true;
		}
		hx_drv_rtc_get_alarm_rawint_status(RTC_ID_0, &status);
		if (status != 0) {
			*cycles = DWT->CYCCNT;
			return true;
		}
	}
	return false;
}

/**
 * @brief Whether a new measurement is due.
 *
 * Due if there is no saved value, on a cold boot, after RTC_CAL_INTERVAL_S of RTC time, or if the RTC is
 * earlier than the last measurement (it restarts at 2024 on a cold boot, and 'setutc' can move it back).
 *
 * @param board     the board record
 * @param coldBoot  true if this wake is a cold boot
 * @param now       the RTC now, in seconds
 * @return true if due
 */
static bool isDue(const XipStateBoard_t *board, bool coldBoot, uint32_t now) {
	if (!board->rtcCalValid || coldBoot || (now < board->rtcMeasuredAt)) {
		return true;
	}
	return ((now - board->rtcMeasuredAt) >= RTC_CAL_INTERVAL_S);
}

/**
 * @brief Measure, put the result in the board record and save it, and use it for this sleep.
 *
 * On failure the board record and the correction are left as they were.
 *
 * @param board  the board record: updated and written if the measurement succeeds
 * @param ppm    output: the measured error
 * @return true if measured and saved
 */
static bool measureAndStore(XipStateBoard_t *board, int32_t *ppm) {
	uint32_t now = 0;
	char percent[12];

	if (!rtc_cal_measure(RTC_CAL_MEASURE_S, ppm)) {
		xprintf("RTC calibration: measurement failed%s\n", board->rtcCalValid ? ", saved value kept" : "");
		return false;
	}
	exif_utc_get_rtc_as_seconds(&now);
	board->rtcCalValid = 1;
	board->rtcPpm = *ppm;
	board->rtcMeasuredAt = now;

	rtc_cal_ppmToPercent(*ppm, percent, sizeof(percent));
	if (xip_state_writeBoard(board) != 0) {
		xprintf("RTC calibration: %ld ppm (%s %%), but the board record could not be written\n", (long) *ppm, percent);
		return false;
	}
	xprintf("RTC calibration: %ld ppm (%s %%), saved\n", (long) *ppm, percent);
	correctionPpm = *ppm;
	correctionValid = true;
	return true;
}

/*********************************************** Global Function Definitions *********************************/

/**
 * @brief Time RTC seconds against the core's cycle counter.
 *
 * Keeps the core busy for up to FIRST_ALARM_S + seconds, plus a margin if an alarm does not come.
 * The caller must make sure that is acceptable: no camera or NN work, and the watchdog fed or stopped.
 * Fails if the RTC read is stale (the first read after DPD returns the time the board went to sleep):
 * the first alarm is then in the past and never comes.
 *
 * @param seconds  RTC seconds to time, at least 1
 * @param ppm      output: the error. Negative means an RTC second is short (the oscillator is fast)
 * @return true if measured
 */
bool rtc_cal_measure(uint8_t seconds, int32_t *ppm) {
	rtc_time now;
	rtc_wkalrm alarm;
	uint32_t cycles1 = 0;
	uint32_t cycles2 = 0;
	int64_t expected;
	int64_t error;
	bool ok;

	if ((seconds == 0) || ((DWT->CTRL & DWT_CTRL_NOCYCCNT_Msk) != 0)) {
		return false;
	}
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;		// enable the DWT
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;	// may already be running (boot_timing.c)

	if (exif_utc_get_rtc_as_time(&now) != RTC_NO_ERROR) {
		return false;
	}

	ok = waitForAlarm(exif_utc_add_seconds_to_tm(now, FIRST_ALARM_S), &cycles1, FIRST_ALARM_S * 1000 + ALARM_MARGIN_MS);
	if (ok) {
		ok = waitForAlarm(exif_utc_add_seconds_to_tm(now, FIRST_ALARM_S + seconds), &cycles2,
				(uint32_t) seconds * 1000 + ALARM_MARGIN_MS);
	}

	// Leave the alarm disabled and its status clear
	alarm.enabled = 0;
	alarm.pending = 0;
	alarm.time = now;
	hx_drv_rtc_set_alarm(RTC_ID_0, &alarm, NULL);
	hx_drv_rtc_clear_alarm_int_status(RTC_ID_0);

	if (!ok) {
		return false;
	}

	expected = (int64_t) SystemCoreClock * seconds;
	error = (((int64_t) (uint32_t) (cycles2 - cycles1) - expected) * PPM_SCALE) / expected;
	if ((error > RTC_CAL_PPM_LIMIT) || (error < -RTC_CAL_PPM_LIMIT)) {
		return false;
	}
	*ppm = (int32_t) error;
	return true;
}

/**
 * @brief Measure now and save the result in the board record (the 'rtccal measure' command).
 *
 * Measures even if the calibration is off, but the result is used only when it is on.
 *
 * @param ppm  output: the measured error
 * @return true if measured and saved
 */
bool rtc_cal_measureAndSave(int32_t *ppm) {
	XipStateBoard_t board;

	xip_state_readBoard(&board);
	if (!measureAndStore(&board, ppm)) {
		return false;
	}
	if (board.rtcCalOff) {
		correctionValid = false;
	}
	return true;
}

/**
 * @brief Before sleep: measure if due, and load the correction for sleep_mode_enter_dpd().
 *
 * Call once per wake, just before DPD, when the camera and NN are idle (the board record is in the
 * flash) and the watchdog is stopped (a measurement keeps the core busy for 2 to 3 s).
 *
 * @param coldBoot  true if this wake is a cold boot
 */
void rtc_cal_beforeSleep(bool coldBoot) {
	XipStateBoard_t board;
	uint32_t now = 0;
	int32_t ppm;

	correctionValid = false;
	xip_state_readBoard(&board);
	if (board.rtcCalOff) {
		return;
	}

	exif_utc_get_rtc_as_seconds(&now);
	if (isDue(&board, coldBoot, now)) {
		measureAndStore(&board, &ppm);
	}
	if (board.rtcCalValid) {
		correctionPpm = board.rtcPpm;
		correctionValid = true;
	}
}

/**
 * @brief The correction for this sleep, if there is one.
 *
 * @param ppm  output: the measured error, before RTC_CAL_SLEEP_OFFSET_PPM is added
 * @return true if the next alarm will be corrected
 */
bool rtc_cal_getCorrection(int32_t *ppm) {
	if (correctionValid) {
		*ppm = correctionPpm;
	}
	return correctionValid;
}

/**
 * @brief The RTC seconds to ask for, so the sleep lasts 'seconds' real seconds.
 *
 * An RTC second lasts (1 + ppm / 10^6) real seconds, so the alarm is set to seconds / (1 + ppm / 10^6)
 * RTC seconds, rounded to the nearest. With no correction, 'seconds' is returned unchanged.
 *
 * @param seconds  the sleep wanted, in real seconds
 * @return the RTC seconds to set the alarm to, 1 to 65535 (0 if seconds is 0)
 */
uint16_t rtc_cal_correctAlarm(uint16_t seconds) {
	int64_t divisor;
	int64_t rtcSeconds;

	if (!correctionValid || (seconds == 0)) {
		return seconds;
	}
	divisor = (int64_t) PPM_SCALE + correctionPpm + RTC_CAL_SLEEP_OFFSET_PPM;
	if (divisor <= 0) {
		return seconds;
	}
	rtcSeconds = ((int64_t) seconds * PPM_SCALE + divisor / 2) / divisor;
	if (rtcSeconds < 1) {
		return 1;
	}
	if (rtcSeconds > UINT16_MAX) {
		return UINT16_MAX;
	}
	return (uint16_t) rtcSeconds;
}

/**
 * @brief Turn the calibration off (no correction, no measuring) or back on, in the board record.
 *
 * Off is for measuring the uncorrected sleep. Takes effect at the next sleep.
 *
 * @param off  true for off
 * @return 0 if saved or unchanged, negative on failure
 */
int rtc_cal_setOff(bool off) {
	XipStateBoard_t board;

	xip_state_readBoard(&board);
	board.rtcCalOff = off ? 1 : 0;
	return xip_state_writeBoard(&board);
}

/**
 * @brief Format ppm as a percentage with two decimals, e.g. -39106 as "-3.91".
 *
 * @param ppm  the value in ppm
 * @param str  output buffer
 * @param len  its size
 */
void rtc_cal_ppmToPercent(int32_t ppm, char *str, size_t len) {
	long absPpm = labs((long) ppm);
	long hundredths = (absPpm + 50) / 100;

	snprintf(str, len, "%s%ld.%02ld", (ppm < 0) ? "-" : "", hundredths / 100, hundredths % 100);
}
