/*
 * sleep_mode.h
 *
 *  Created on: 2023/10/24
 *      Author: Himax
 */

#ifndef APP_SCENARIO_APP_FREERTOS_SLEEP_MODE_H_
#define APP_SCENARIO_APP_FREERTOS_SLEEP_MODE_H_

/*
CGP - the following might be needed:
#include <stdint.h>
#include "hx_drv_rtc.h"

void sleep_mode_print_event(uint32_t event, uint32_t event1);

*/

//Not used
//static void print_wakeup_event(uint32_t event, uint32_t event1);

void app_clk_enable();
void app_clk_disable();
void setCM55MTimerAlarmPMU(uint32_t timer_ms);
/* CGP - I renamed these:
void sleep_mode_enter_sleep(uint32_t timer_ms, uint32_t aon_gpio, uint32_t retention);
void sleep_mode_enter_dpd();
*/
void app_pmu_enter_sleep(uint32_t timer_ms, uint32_t aon_gpio, uint32_t retention);
void app_pmu_enter_dpd(void);
RTC_ERROR_E DPD_RTC_GetTime(rtc_time *tm);
RTC_ERROR_E RTC_GetTime(rtc_time *tm);
RTC_ERROR_E RTC_SetTime(rtc_time *tm);
#endif  /* APP_SCENARIO_APP_FREERTOS_SLEEP_MODE_H_ */
