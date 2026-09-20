/*
 * sleep_mode.c
 *
 *  Created on: 2023/10/24
 *      Author: Himax
 *
 *  Deep power down (DPD) entry, and decoding of the reason for a wakeup.
 *  See sleep_mode.h.
 *
 *  Includes the DPD additions of 20/3/25, made for ww500_md. Copied into ww500_minimal
 *  from ww500_md and reformatted.
 */

/*********************************************** Includes ****************************************************/

#include <string.h>

#include "xprintf.h"
#include "printf_x.h"	// Print colours
#include "hx_drv_gpio.h"
#include "hx_drv_swreg_aon.h"
#include "hx_drv_pmu_export.h"
#include "hx_drv_rtc.h"
#include "hx_drv_scu.h"
#include "powermode.h"

#include "rtc_util.h"
#include "sleep_mode.h"

/*********************************************** Local Defines ***********************************************/

#define WAKEUPEVENTS 11
#define WAKEUPEVENTS1 4

/************************************************ Local Types ************************************************/

/********************************************** Local Variables **********************************************/

// Strings for expected events
static const char* wakeup_event_str[WAKEUPEVENTS] = {
		"ext_force_pmu_dc_sync",
		"RTC Timer",
		"Anti-tamp",
		"DC force",
		"External GPIO",
		"RTC_timer_int",
		"SB_timer_int",
		"CMP_int",
		"TS_int",
		"I2C_W_int",
		"SB Timer 0"
};

// Strings for expected events
static const char* wakeup_event1_str[WAKEUPEVENTS1] = {
		"WAKE signal",
		"PAD_VMUTE",
		"External Int",
		"Anti-tamp"
};

/**************************************** Local Function Declarations ****************************************/

/**************************************** Local Function Definitions *****************************************/

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Prints the reason for a wakeup.
 *
 * There are 2 sets of wakeup events, defined in PMU_WAKEUPEVENT_E and PMU_WAKEUPEVENT1_E in
 * hx_drv_export.h. These values are a bit map. Only one is expected to be active at a time.
 *
 * The code shifts through the event bit maps until one reason is found, then prints it.
 * If there is none it must be a cold boot.
 *
 * @param event  One of the events in PMU_WAKEUPEVENT_E.
 * @param event1 One of the events in PMU_WAKEUPEVENT1_E.
 */
void sleep_mode_print_event(uint32_t event, uint32_t event1) {
	uint8_t index;

    xprintf("Wakeup_event = 0x%04x, WakeupEvt1 = 0x%04x ", event, event1);

	for (index = 0; index < WAKEUPEVENTS; index++) {
		if ((event >> index) & 1) {
			// found it
			xprintf("%s\n", wakeup_event_str[index]);
			return;
		}
	}

	for (index = 0; index < WAKEUPEVENTS1; index++) {
		if ((event1 >> index) & 1) {
			// found it
			xprintf("%s\n", wakeup_event1_str[index]);
			return;
		}
	}

	// no wakeup event - must be cold boot.
	xprintf("Cold boot\n");
}

/**
 * @brief Puts the processor in deep power down (DPD).
 *
 * Does not return. Wakeup events cause a reset.
 *
 * @param wakeSource Bitmap of wake sources (SLEEPMODE_WAKE_SOURCE_E).
 * @param alarmDelay Time in seconds for the wake from the RTC.
 * @param verbose    If true more diagnostic messages are printed.
 */
void sleep_mode_enter_dpd(SLEEPMODE_WAKE_SOURCE_E wakeSource, uint16_t alarmDelay, bool verbose) {
	PM_DPD_CFG_T cfg;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;
	PM_CFG_PWR_MODE_E mode;
	uint32_t freq;
	SCU_PLL_FREQ_E pmuwakeup_pll_freq;
	SCU_HSCCLKDIV_E pmuwakeup_cm55m_div;
	SCU_LSCCLKDIV_E pmuwakeup_cm55s_div;

	// The rtc_time structure is the same as the C struct tm (in time.h)
	// except that it lacks the tm_isdst member
	rtc_time time = {0};
	char timeString[RTC_UTIL_UTC_STRING_LENGTH];

	rtc_util_getTime(&time);
	rtc_util_timeToString(&time, timeString, sizeof(timeString));

	XP_LT_RED;
	xprintf(">>> Entering DPD at %s\n\n", timeString);
	XP_LT_GREY;	// Grey so the bootloader messages are printed in grey, on exit from DPD

	/*Get System Current Clock*/
	//BOOTROM_DISPLL_BL_PLL
	SWREG_AON_WARMBOOTDISPLL_BLCHG_E warmbootclk = SWREG_AON_WARMBOOTDISPLL_BLCHG_YES;
	hx_drv_swreg_aon_set_bl_warmbootclk(warmbootclk);
	hx_drv_swreg_aon_get_pmuwakeup_freq(&pmuwakeup_pll_freq, &pmuwakeup_cm55m_div, &pmuwakeup_cm55s_div);
	hx_drv_swreg_aon_get_pllfreq(&freq);

	if (verbose) {
		xprintf("pmuwakeup_freq_type=%d, pmuwakeup_cm55m_div=%d, pmuwakeup_cm55s_div=%d\n", pmuwakeup_pll_freq, pmuwakeup_cm55m_div, pmuwakeup_cm55s_div);
		xprintf("pmuwakeup_run_freq=%d\n", freq);
	}

	hx_drv_swreg_aon_set_bl_pmuwakeup_freq(pmuwakeup_pll_freq, pmuwakeup_cm55m_div, pmuwakeup_cm55s_div);
	hx_drv_swreg_aon_set_bl_pllfreq(freq);

	pmuwakeup_pll_freq = SCU_PLL_FREQ_DISABLE;
	freq = 0;
	pmuwakeup_cm55m_div = SCU_HSCCLKDIV_1;
	pmuwakeup_cm55s_div = SCU_LSCCLKDIV_1;

	if (verbose) {
		xprintf("Bootrom pmuwakeup_freq_type=%d, pmuwakeup_cm55m_div=%d, pmuwakeup_cm55s_div=%d\n", pmuwakeup_pll_freq, pmuwakeup_cm55m_div, pmuwakeup_cm55s_div);
		xprintf("Bootrom pmuwakeup_run_freq=%d\n", freq);
	}

	mode = PM_MODE_PS_DPD;
	hx_lib_pm_get_defcfg_bymode(&cfg, mode);

	/*Setup bootrom clock speed when PMU Warm boot wakeup*/
	cfg.bootromspeed.bootromclkfreq = pmuwakeup_pll_freq;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = pmuwakeup_cm55m_div;
	cfg.bootromspeed.cm55s_div = pmuwakeup_cm55s_div;

	/*Setup CM55 Small can be reset*/
	cfg.cm55s_reset = SWREG_AON_PMUWAKE_CM55S_RERESET_YES;

	/*Mask ANTI Tamper Interrupt for PMU*/
	cfg.pmu_anti_mask = PM_IP_INT_MASK;
	/*No Debug Dump message*/
	cfg.support_debugdump = 0;
	/*Not DCDC pin output*/
	cfg.pmu_dcdc_outpin = PM_CFG_DCDC_MODE_VMUTE;


	/*Mask PA1 Interrupt for PMU*/
	cfg.pmu_pad_pa0_mask = PM_IP_INT_MASK;
	cfg.pmu_pad_pa1_mask = PM_IP_INT_MASK;
	cfg.pmu_rtc_mask = PM_RTC_INT_MASK_ALLMASK;

	// We need to clear the RTC interrupt or it might trigger immediately
	hx_drv_rtc_clear_alarm_int_status(RTC_ID_0);

	if (wakeSource & SLEEPMODE_WAKE_SOURCE_WAKE_PIN) {
		/*UnMask PA0 Interrupt for PMU*/
		cfg.pmu_pad_pa0_mask = PM_IP_INT_MASK_ALL_UNMASK;
		/*Set PA0 Pinmux that PMU can detect level high trigger wakeup*/
		hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_PMU_SINT0, 1);
		/**< PMU GPIO Wakeup Polarity **/
		cfg.gpio_wakeup_pol = PMU_DPD_PA01_GPIO_POL_WAKEUP_HIGH;
	}

	if (wakeSource & SLEEPMODE_WAKE_SOURCE_RTC) {
		rtc_wkalrm alarm;

		// Add some time:
		time = rtc_util_addSeconds(time, alarmDelay);

		rtc_util_timeToString(&time, timeString, sizeof(timeString));
		xprintf("Will wake at %s\n\n", timeString);

		alarm.enabled = 1;
		alarm.pending = 0;
		alarm.time = time;

		hx_drv_rtc_set_alarm(RTC_ID_0, &alarm, NULL);

		/*UnMask RTC IP Interrupt fo PMU*/
		cfg.pmu_rtc_mask = PM_RTC_INT_MASK_ALLUNMASK;
	}

	if (verbose) {
		xprintf("speed=%d,reset=%d\n", cfg.bootromspeed.bootromclkfreq, cfg.cm55s_reset);
		xprintf("pmu_rtc_mask=0x%x, pmu_pad_pa0_mask=0x%x, pmu_pad_pa1_mask=0x%x\n",
				cfg.pmu_rtc_mask, cfg.pmu_pad_pa0_mask, cfg.pmu_pad_pa1_mask);
		xprintf("debug=%d, reset=%d, mode=%d\n", cfg.support_debugdump, cfg.cm55s_reset, mode);
		xprintf("dcdcpin=%d, pmu_anti_mask=0x%x\n", cfg.pmu_dcdc_outpin, cfg.pmu_anti_mask);
		xprintf("freq=%d, cm55mdiv=%d,cm55sdiv=%d\n", freq, cfg.bootromspeed.cm55m_div, cfg.bootromspeed.cm55s_div);
	}

	/*Set PMU DPD configuration*/
	hx_lib_pm_cfg_set(&cfg, NULL, mode);

	rtc_util_clkDisable();

	/*Use PMU lib to control HSC_CLK and LSC_CLK so set those parameters to 0*/
	memset(&hsc_cfg, 0, sizeof(SCU_PDHSC_HSCCLK_CFG_T));
	memset(&lsc_cfg, 0, sizeof(SCU_LSC_CLK_CFG_T));

	/*Trigger to PMU mode*/
	hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYPMLIB);
}
