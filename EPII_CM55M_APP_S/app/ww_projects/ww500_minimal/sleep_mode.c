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
#include "hx_drv_timer.h"
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

static void setCM55MTimerAlarmPMU(uint32_t timer_ms);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Starts the CM55M timer (Timer2) as a one-shot that the PMU can use as a wake source.
 *
 * @param timer_ms The delay in ms.
 */
static void setCM55MTimerAlarmPMU(uint32_t timer_ms) {
	TIMER_CFG_T timer_cfg;

	timer_cfg.period = timer_ms;
	timer_cfg.mode = TIMER_MODE_ONESHOT;
	timer_cfg.ctrl = TIMER_CTRL_PMU;
	timer_cfg.state = TIMER_STATE_PMU;

	hx_drv_timer_cm55m_start(&timer_cfg, NULL);
}

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
 * @brief Puts the processor in Power-down mode (PD), optionally keeping the memories. Does not return.
 *
 * This is the datasheet's "Power-down" mode: both cores are powered off and the wake is a warm boot.
 * With retention the TCM and HSC SRAM keep their contents and the bootloader skips reloading the
 * application from flash, so the wake is much faster than from DPD. Without retention the memories
 * are not kept.
 *
 * The wake sources are the CM55M timer (after timer_ms) and, if aon_gpio is 0 or 1, that AON GPIO
 * going high (0 = PA0, the WAKE pin).
 *
 * Copied from the unused function in ww500_md, which took it from a Himax example. The clock handling for the
 * wake was changed to match sleep_mode_enter_dpd(): the boot ROM wakes on the RC oscillator with the PLL disabled.
 *
 * @param timer_ms  Time in ms for the timer wake.
 * @param aon_gpio  0 for AON_GPIO0 (PA0), 1 for AON_GPIO1 (PA1), anything else for none.
 * @param retention 1 to retain the memories, 0 for none.
 */
void sleep_mode_enter_sleep(uint32_t timer_ms, uint32_t aon_gpio, uint32_t retention) {
	uint8_t  gpio_value;
	PM_PD_NOVIDPRE_CFG_T cfg;
	uint32_t freq;
	SCU_LSC_CLK_CFG_T lsc_cfg;
	SCU_PDHSC_HSCCLK_CFG_T hsc_cfg;
	PM_CFG_PWR_MODE_E mode;
	SCU_PLL_FREQ_E pmuwakeup_pll_freq;
	SCU_HSCCLKDIV_E pmuwakeup_cm55m_div;
	SCU_LSCCLKDIV_E pmuwakeup_cm55s_div;

	/*Clear PMU Wakeup Event*/
	hx_lib_pm_clear_event();

	/*Clear Wakeup related IP Interrupt*/
	hx_drv_gpio_clr_int_status(AON_GPIO0);
	hx_drv_gpio_clr_int_status(AON_GPIO1);
	hx_drv_timer_ClearIRQ(TIMER_ID_2);

	if ( aon_gpio == 0 )
	{
		hx_drv_gpio_set_input(AON_GPIO0);
		hx_drv_gpio_set_int_type(AON_GPIO0, GPIO_IRQ_TRIG_TYPE_LEVEL_HIGH);
		hx_drv_scu_set_PA0_pinmux(SCU_PA0_PINMUX_AON_GPIO0_0, 1);
		hx_drv_gpio_set_int_enable(AON_GPIO0, 1);
		hx_drv_gpio_get_in_value(AON_GPIO0, &gpio_value);
		xprintf("AON_GPIO0 = %d\n", gpio_value);
	}
	else if ( aon_gpio == 1 )
	{
		hx_drv_gpio_set_input(AON_GPIO1);
		hx_drv_gpio_set_int_type(AON_GPIO1, GPIO_IRQ_TRIG_TYPE_LEVEL_HIGH);
		hx_drv_scu_set_PA1_pinmux(SCU_PA1_PINMUX_AON_GPIO1, 1);
		hx_drv_gpio_set_int_enable(AON_GPIO1, 1);
		hx_drv_gpio_get_in_value(AON_GPIO1, &gpio_value);
		xprintf("AON_GPIO1 = %d\n", gpio_value);
	}

	/*Get System Current Clock*/
	hx_drv_swreg_aon_set_bl_warmbootclk(SWREG_AON_WARMBOOTDISPLL_BLCHG_YES);
	hx_drv_swreg_aon_get_pmuwakeup_freq(&pmuwakeup_pll_freq, &pmuwakeup_cm55m_div, &pmuwakeup_cm55s_div);
	hx_drv_swreg_aon_get_pllfreq(&freq);
	xprintf("pmuwakeup_freq_type=%d, pmuwakeup_cm55m_div=%d, pmuwakeup_cm55s_div=%d\n", pmuwakeup_pll_freq, pmuwakeup_cm55m_div, pmuwakeup_cm55s_div);
	xprintf("pmuwakeup_run_freq=%d\n", freq);

	// As in sleep_mode_enter_dpd(): remember the running clocks for the bootloader, then have the boot ROM
	// wake on the 24 MHz RC oscillator with the PLL disabled. The application note says the clock source
	// must be the RC oscillator before entering PD or DPD. The Himax example this function was copied from
	// woke the boot ROM at the current PLL frequency (400 MHz), and the wake did not resume.
	hx_drv_swreg_aon_set_bl_pmuwakeup_freq(pmuwakeup_pll_freq, pmuwakeup_cm55m_div, pmuwakeup_cm55s_div);
	hx_drv_swreg_aon_set_bl_pllfreq(freq);

	pmuwakeup_pll_freq = SCU_PLL_FREQ_DISABLE;
	freq = 0;
	pmuwakeup_cm55m_div = SCU_HSCCLKDIV_1;
	pmuwakeup_cm55s_div = SCU_LSCCLKDIV_1;
	xprintf("Bootrom wake clock: RC oscillator, PLL disabled\n");

	mode = PM_MODE_PS_NOVID_PREROLLING;
	hx_lib_pm_get_defcfg_bymode(&cfg, mode);

	/*Setup bootrom clock speed when PMU Warm boot wakeup*/
	cfg.bootromspeed.bootromclkfreq = pmuwakeup_pll_freq;
	cfg.bootromspeed.pll_freq = freq;
	cfg.bootromspeed.cm55m_div = pmuwakeup_cm55m_div;
	cfg.bootromspeed.cm55s_div = pmuwakeup_cm55s_div;

	/*Setup CM55 Small can be reset*/
	cfg.cm55s_reset = SWREG_AON_PMUWAKE_CM55S_RERESET_YES;
	/*Mask RTC Interrupt for PMU*/
	cfg.pmu_rtc_mask = PM_RTC_INT_MASK_ALLMASK;
	/*Mask PA23 Interrupt for PMU*/
	cfg.pmu_pad_pa23_mask = PM_IP_INT_MASK;
	/*Mask I2CWakeup Interrupt for PMU*/
	cfg.pmu_i2cw_mask = PM_IP_INT_MASK;
	/*Mask CMP Interrupt for PMU*/
	cfg.pmu_cmp_mask = PM_IP_INT_MASK;
	/*Mask TS Interrupt for PMU*/
	cfg.pmu_ts_mask = PM_IP_INT_MASK;
	/*Mask ANTI TAMPER Interrupt for PMU*/
	cfg.pmu_anti_mask = PM_IP_INT_MASK;
	/*No Debug Dump message*/
	cfg.support_debugdump = 0;

	/*UnMask PA01 Interrupt for PMU*/
	cfg.pmu_pad_pa01_mask = PM_IP_INT_MASK_ALL_UNMASK;

	/*UnMask Timer2 Interrupt others timer interrupt are mask for PMU*/
	cfg.pmu_timer_mask = 0x1FB;

	if ( retention == 1 )
	{
		/*Setup Memory retention*/
		XP_LT_BLUE
		xprintf("Sleeping soon - with memory retention\n\n");
		XP_LT_GREY
		cfg.tcm_retention = PM_MEM_RET_YES;			/**< CM55M TCM Retention**/
		cfg.hscsram_retention[0] = PM_MEM_RET_YES;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[1] = PM_MEM_RET_YES;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[2] = PM_MEM_RET_YES;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[3] = PM_MEM_RET_YES;	/**< HSC SRAM Retention**/
		cfg.lscsram_retention = PM_MEM_RET_NO;		/**< LSC SRAM Retention**/
		cfg.skip_bootflow.sec_mem_flag = SWREG_AON_RETENTION;			/**< Skip Boot Flow**/
		cfg.skip_bootflow.first_bl_flag = SWREG_AON_RETENTION; /*!< First BL Retention */
		cfg.skip_bootflow.cm55m_s_app_flag = SWREG_AON_RETENTION; /*!< cm55m_s_app Retention */
		cfg.skip_bootflow.cm55m_ns_app_flag = SWREG_AON_RETENTION; /*!< cm55m_ns_app Retention */
		cfg.skip_bootflow.cm55s_s_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55s_s_app Retention */
		cfg.skip_bootflow.cm55s_ns_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55s_ns_app Retention */
		cfg.skip_bootflow.cm55m_model_flag = SWREG_AON_RETENTION; /*!< cm55m model Retention */
		cfg.skip_bootflow.cm55s_model_flag = SWREG_AON_NO_RETENTION; /*!< cm55s model Retention */
		cfg.skip_bootflow.cm55m_appcfg_flag = SWREG_AON_RETENTION; /*!< cm55m appcfg Retention */
		cfg.skip_bootflow.cm55s_appcfg_flag = SWREG_AON_NO_RETENTION; /*!< cm55s appcfg Retention */
		cfg.skip_bootflow.cm55m_s_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55m_s_app RW Data Retention */
		cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55m_ns_app RW Data Retention */
		cfg.skip_bootflow.cm55s_s_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55s_s_app RW Data Retention */
		cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55s_ns_app RW Data Retention */
		cfg.skip_bootflow.secure_debug_flag = SWREG_AON_RETENTION;
	}
	else
	{
		/*Setup Memory no retention*/
		XP_LT_BLUE
		xprintf("Sleeping soon - no memory retention\n\n");
		XP_LT_GREY
		cfg.tcm_retention = PM_MEM_RET_NO;			/**< CM55M TCM Retention**/
		cfg.hscsram_retention[0] = PM_MEM_RET_NO;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[1] = PM_MEM_RET_NO;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[2] = PM_MEM_RET_NO;	/**< HSC SRAM Retention**/
		cfg.hscsram_retention[3] = PM_MEM_RET_NO;	/**< HSC SRAM Retention**/
		cfg.lscsram_retention = PM_MEM_RET_NO;		/**< LSC SRAM Retention**/
		cfg.skip_bootflow.sec_mem_flag = SWREG_AON_NO_RETENTION;			/**< Skip Boot Flow**/
		cfg.skip_bootflow.first_bl_flag = SWREG_AON_NO_RETENTION; /*!< First BL Retention */
		cfg.skip_bootflow.cm55m_s_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55m_s_app Retention */
		cfg.skip_bootflow.cm55m_ns_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55m_ns_app Retention */
		cfg.skip_bootflow.cm55s_s_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55s_s_app Retention */
		cfg.skip_bootflow.cm55s_ns_app_flag = SWREG_AON_NO_RETENTION; /*!< cm55s_ns_app Retention */
		cfg.skip_bootflow.cm55m_model_flag = SWREG_AON_NO_RETENTION; /*!< cm55m model Retention */
		cfg.skip_bootflow.cm55s_model_flag = SWREG_AON_NO_RETENTION; /*!< cm55s model Retention */
		cfg.skip_bootflow.cm55m_appcfg_flag = SWREG_AON_NO_RETENTION; /*!< cm55m appcfg Retention */
		cfg.skip_bootflow.cm55s_appcfg_flag = SWREG_AON_NO_RETENTION; /*!< cm55s appcfg Retention */
		cfg.skip_bootflow.cm55m_s_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55m_s_app RW Data Retention */
		cfg.skip_bootflow.cm55m_ns_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55m_ns_app RW Data Retention */
		cfg.skip_bootflow.cm55s_s_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55s_s_app RW Data Retention */
		cfg.skip_bootflow.cm55s_ns_app_rwdata_flag = SWREG_AON_NO_RETENTION;/*!< cm55s_ns_app RW Data Retention */
		cfg.skip_bootflow.secure_debug_flag = SWREG_AON_NO_RETENTION;
	}

	/**No Pre-capture when boot up**/
	cfg.support_bootwithcap = PM_BOOTWITHCAP_NO;

	/*Not DCDC pin output*/
	cfg.pmu_dcdc_outpin = PM_CFG_DCDC_MODE_OFF;
	/** No Pre-capture when boot up**/
	cfg.ioret = PM_CFG_PD_IORET_ON;

	cfg.sensor_type = PM_SENSOR_TIMING_FVLDLVLD_CON;
	/*SIMO on in PD*/
	cfg.simo_pd_onoff = PM_SIMO_PD_ONOFF_ON;

	hx_lib_pm_cfg_set(&cfg, NULL, mode);

	/* Setup CM55M Timer(Timer2) Wakeup */
	setCM55MTimerAlarmPMU(timer_ms);

	/* Use PMU lib to control HSC_CLK and LSC_CLK so set those parameter to 0 */
	memset(&hsc_cfg, 0, sizeof(SCU_PDHSC_HSCCLK_CFG_T));
	memset(&lsc_cfg, 0, sizeof(SCU_LSC_CLK_CFG_T));

	/* Trigger to PMU mode */
	hx_lib_pm_trigger(hsc_cfg, lsc_cfg, PM_CLK_PARA_CTRL_BYPMLIB);
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
