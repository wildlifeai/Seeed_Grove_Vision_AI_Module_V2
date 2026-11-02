/*
 * exif_utc.h
 *
 * Supports EXIF storage of UTC data.
 *
 * This also provides a set of functions to set and get the time, for other purposes,
 * such as adding dates to file names.
 *
 *  There is a need to accept an ASCII string (Uses a UTC standard format) from the app
 *  and produce a differently formatted ASCII string for saving in the EXIF section of the JPEG file.
 *
 *  The functions here perform these operations (which can be cascaded as necessary)
 *  	get rtc as time		- reads the RTC hardware to produce a rtc_time object
		set rtc from time	- sets the RTC hardware from a rtc_time object

		utc string to time	- converts an ISO UTC string to a rtc_time object
		time to utc string	- converts a rtc_time object to an ISO UTC string
		time to exif string	- converts a rtc_time object to an EXIF timestamp string

		get rtc as exif string - reads the RTC hardware to produce an EXIF timestamp string

		There are two test functions
 *
 * BTW - linus 32-bit representation of time as a 32-bit value won't work beyond 2038"
 * 	https://pub.huizhou92.com/a-time-bomb-laid-50-years-ago-is-now-headed-for-detonation-1d3b2fa357be
 *
 *  Created on: 21 Mar 2025
 *      Author: charl
 */

#ifndef EXIF_UTC_H_
#define EXIF_UTC_H_

/********************************** Includes ******************************************/

#include "hx_drv_rtc.h"
#include "time.h"
#include "ff.h"  // FatFs types
#include "hx_drv_rtc.h"

/**************************************** Global Defines  *************************************/

// Length of UTC timestamp "YYYY-MM-DDTHH:MM:SSZ", plus trailing '\0'
#define UTCSTRINGLENGTH 21

// Length of EXIF timestamp "YYYY:MM:DD HH:MM:SS", plus trailing '\0'
#define EXIFSTRINGLENGTH 20

/**************************************** Global routine declarations  *************************************/

// Initialises clocks and sets the time
RTC_ERROR_E exif_utc_init(const char *str);

// Determines if the device has a plausible time
bool exif_utc_has_time(void);

// reads the RTC hardware to produce a rtc_time object - use after DPD
RTC_ERROR_E exif_utc_get_rtc_as_time_dpd(rtc_time * tm);

// reads the RTC hardware to produce a rtc_time object
RTC_ERROR_E exif_utc_get_rtc_as_time(rtc_time * tm);

// reads the RTC hardware to produce time in seconds
RTC_ERROR_E exif_utc_get_rtc_as_seconds(uint32_t * tm);

// sets the RTC hardware from a rtc_time object
RTC_ERROR_E exif_utc_set_rtc_from_time(rtc_time * tm);

// converts an ISO UTC string to a rtc_time object
RTC_ERROR_E exif_utc_utc_string_to_time(const char *str, rtc_time * tm);

// converts a rtc_time object to an ISO UTC string
RTC_ERROR_E exif_utc_time_to_utc_string(rtc_time * tm, char *str, uint8_t length);

// converts a rtc_time object to an EXIF timestamp string
RTC_ERROR_E exif_utc_time_to_exif_string(rtc_time * tm, char *str, uint8_t length);

// reads the RTC hardware to produce an EXIF timestamp string
RTC_ERROR_E exif_utc_get_rtc_as_exif_string(char *str, uint8_t length);

// reads the RTC hardware to produce an ISO timestamp string
RTC_ERROR_E exif_utc_get_rtc_as_utc_string(char *str, uint8_t length);

rtc_time exif_utc_add_seconds_to_tm(rtc_time input_rtc, time_t seconds_to_add);


// Tests these functions
void exif_utc_test_get_rtc(void);

void exif_utc_test_set_rtc(char *str);

void exif_utc_clk_enable(void);
void exif_utc_clk_disable(void);

// For FatFs timestamps
DWORD get_fattime(void);

// The time for this wake (#56): a reference time and the tick count at it, from setutc or, failing that, the RTC
// Keeps the time from setutc without writing the RTC (the write blocks interrupts for about 1.4 s)
RTC_ERROR_E exif_utc_set_reference(const char *str, int32_t *rtcOutBy, bool *rtcOutByKnown);
// The time now: from the reference if there is one, else from the RTC
RTC_ERROR_E exif_utc_get_time(rtc_time * tm);
// The time at a tick count (e.g. when a frame was ready): from the reference if there is one, else the RTC now
RTC_ERROR_E exif_utc_get_time_at_tick(uint32_t tick, rtc_time * tm);
// Converts a time to seconds since 1970 (e.g. for the image file name)
uint32_t exif_utc_time_to_seconds(const rtc_time * tm);
// Notes the RTC's first read after a DPD wake, which may still hold the time at sleep
void exif_utc_note_boot_time(rtc_time * tm);
// Before the first photo of a wake: waits for setutc, then falls back to the RTC once it has ticked
void exif_utc_wait_for_reference(uint32_t waitMs, bool setutcExpected);
// At sleep: writes the RTC from the setutc reference if it is far out
void exif_utc_before_sleep(void);

#endif /* EXIF_UTC_H_ */
