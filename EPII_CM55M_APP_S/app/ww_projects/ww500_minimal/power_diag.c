/*
 * power_diag.c
 *
 *  Created on: 21 Sep 2026
 *      Author: Charles Palmer
 *
 *  Read-only diagnostics for investigating the current the processor draws while awake.
 *  See power_diag.h.
 *
 *  How the idle measurement works: with configUSE_TICKLESS_IDLE the idle task runs, calls
 *  vApplicationIdleHook(), then (if every other task is blocked for at least 2 ticks) stops
 *  the tick and executes WFI until an interrupt or the SysTick reload expires. So while all
 *  tasks are blocked the idle hook runs about once per SysTick reload period, which is
 *  0xFFFFFF SysTick counts long. If tickless idle is NOT sleeping the idle task spins and the
 *  hook runs tens of thousands of times a second.
 */

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "system_WE2_ARMCM55.h"

#include "WE2_device.h"
#include "hx_drv_scu.h"
#include "hx_drv_swreg_aon.h"

#include "printf_x.h"
#include "xprintf.h"

#include "inactivity.h"
#include "power_diag.h"

/*********************************************** Local Defines ***********************************************/

// Time to wait after switching the PLL on before switching the clocks back to it
#define PLL_LOCK_DELAY_MS			5

// Time to wait after switching the 24 MHz crystal oscillator on before the PLL is switched on with it as input
#define CRYSTAL_START_DELAY_MS		20

// The SysTick counter is 24 bits
#define SYSTICK_MAX_COUNT			0xFFFFFF

// The idle hook rate, as a multiple of the expected rate, above which the idle task is spinning
#define SPINNING_FACTOR				20

/************************************************ Local Types ************************************************/

/******************************************** External Variables *********************************************/

// The FreeRTOS port sets up SysTick with this function (it is weak, so it can be called again)
extern void vPortSetupTimerInterrupt(void);

/********************************************** Local Variables **********************************************/

// Names of the clock sources, in the order of SCU_PLLCLKSRC_E and SCU_HSCCLKSRC_E / SCU_LSCCLKSRC_E
static const char * const pllSrcName[3] = { "RC24M1M", "RC96M48M", "XTAL24M" };
static const char * const clkSrcName[4] = { "RC24M1M", "XTAL24M", "RC96M48M", "PLL" };
static const char * const lscRefName[4] = { "RC24M1M", "RC96M48M", "XTAL24M", "PLL" };

// The clock enables as they were before the first power_diag_gateClocks()
static bool clocksSaved = false;
static SCU_PDHSC_CLKEN_CFG_T savedHsc;
static SCU_PDLSC_CLKEN_CFG_T savedLsc;
static SCU_PDSB_CLKEN_CFG_T savedSb;

// The clock sources and dividers as they were before the first power_diag_slowClock()
static bool speedSaved = false;
static SCU_PDHSC_HSCCLK_CFG_T savedHscClk;
static SCU_LSC_CLK_CFG_T savedLscClk;

// The LSC reference clock (source of the UART clock) as it was before the first power_diag_uartClock()
static bool lscRefSaved = false;
static SCU_PDLSC_LSCREF_CFG_T savedLscRef;

// True while the PLL has been switched off by power_diag_pll()
static bool pllOff = false;

// The crystal oscillator enables as they were before the first power_diag_crystal()
static bool crystalsSaved = false;
static uint8_t savedXtal24En = 0;
static uint8_t savedXtal32En = 0;

/**************************************** Local Function Declarations ****************************************/

static void printFreq(const char *name, SCU_CLK_FREQ_TYPE_E type);
static void printFlag(const char *name, uint8_t enabled);
static void saveClocks(void);
static void gateImage(SCU_PDHSC_CLKEN_CFG_T *hsc);
static void gateHsc(SCU_PDHSC_CLKEN_CFG_T *hsc);
static bool gateHscPart(SCU_PDHSC_CLKEN_CFG_T *hsc, const char *part);
static void gateFlash(SCU_PDHSC_CLKEN_CFG_T *hsc);
static void gateLsc(SCU_PDLSC_CLKEN_CFG_T *lsc);
static void gateSb(SCU_PDSB_CLKEN_CFG_T *sb);
static void speedChanged(void);
static bool pllInUse(void);
static bool xtal24InUse(void);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Prints the frequency of a clock.
 *
 * @param name Name of the clock.
 * @param type The clock to read.
 */
static void printFreq(const char *name, SCU_CLK_FREQ_TYPE_E type) {
	uint32_t freq = 0;

	if (hx_drv_scu_get_freq(type, &freq) == SCU_NO_ERROR) {
		xprintf("  %-12s %u Hz\n", name, (unsigned) freq);
	}
	else {
		xprintf("  %-12s error\n", name);
	}
}

/**
 * @brief Prints the name of a clock enable, if it is set.
 *
 * @param name    Name of the clock enable.
 * @param enabled The value of the enable.
 */
static void printFlag(const char *name, uint8_t enabled) {
	if (enabled) {
		xprintf(" %s", name);
	}
}

/**
 * @brief Saves the clock enables the first time it is called, so they can be restored.
 */
static void saveClocks(void) {
	if (!clocksSaved) {
		hx_drv_scu_get_pdhsc_clken_cfg(&savedHsc);
		hx_drv_scu_get_pdlsc_clken_cfg(&savedLsc);
		hx_drv_scu_get_pdsb_clken_cfg(&savedSb);
		clocksSaved = true;
	}
}

/**
 * @brief Clears the enables of the image data path (camera input, DMA, JPEG, MIPI).
 *
 * @param hsc The HSC clock enables to modify.
 */
static void gateImage(SCU_PDHSC_CLKEN_CFG_T *hsc) {
	memset(&hsc->imageclk_en, 0, sizeof(hsc->imageclk_en));
}

/**
 * @brief Clears the enables of the HSC blocks this app does not use.
 *
 * The neural network processor (U55), I3C host, PUF, the DMA controllers and SDIO.
 *
 * @param hsc The HSC clock enables to modify.
 */
static void gateHsc(SCU_PDHSC_CLKEN_CFG_T *hsc) {
	hsc->u55_clk_en = 0;
	hsc->i3c_hc_clk_en = 0;
	hsc->puf_clk_en = 0;
	hsc->dma0_clk_en = 0;
	hsc->dma1_clk_en = 0;
	hsc->sdio_clk_en = 0;
}

/**
 * @brief Clears the enable of one of the blocks in the "hsc" group.
 *
 * Used to find out which block matters: gating "hsc" as a whole stopped the processor resuming
 * from DPD (see doc/power_investigation.md).
 *
 * @param hsc  The HSC clock enables to modify.
 * @param part "u55", "i3c", "puf", "dma" (DMA0 and DMA1) or "sdio".
 * @return true if the part is known.
 */
static bool gateHscPart(SCU_PDHSC_CLKEN_CFG_T *hsc, const char *part) {
	if (strcmp(part, "u55") == 0) {
		hsc->u55_clk_en = 0;
	}
	else if (strcmp(part, "i3c") == 0) {
		hsc->i3c_hc_clk_en = 0;
	}
	else if (strcmp(part, "puf") == 0) {
		hsc->puf_clk_en = 0;
	}
	else if (strcmp(part, "dma") == 0) {
		hsc->dma0_clk_en = 0;
		hsc->dma1_clk_en = 0;
	}
	else if (strcmp(part, "sdio") == 0) {
		hsc->sdio_clk_en = 0;
	}
	else {
		return false;
	}

	return true;
}

/**
 * @brief Clears the enables of the flash interfaces.
 *
 * Separate from the other groups because it is the one most likely to cause a problem if
 * anything in the image executes from flash. The app runs from SRAM, so it should be safe.
 *
 * @param hsc The HSC clock enables to modify.
 */
static void gateFlash(SCU_PDHSC_CLKEN_CFG_T *hsc) {
	hsc->qspi_en = 0;
	hsc->ospi_en = 0;
	hsc->spi2ahb_en = 0;
	hsc->i2c2ahb_flash_w_clk_en = 0;
}

/**
 * @brief Clears the enables of the LSC peripherals this app does not use.
 *
 * Keeps the buses, SRAM2, UART0 (the console), GPIO, the SW (debug) clock and RO_PD. Since the SD card was added
 * it also keeps the SPI master (sspim) and DMA2 and DMA3, which the SD card driver may use; the power
 * measurements in doc/power_investigation.md were made before that, with these three switched off too.
 *
 * @param lsc The LSC clock enables to modify.
 */
static void gateLsc(SCU_PDLSC_CLKEN_CFG_T *lsc) {
	lsc->cm55s_clk_en = 0;
	lsc->i2s_host_sclk_en = 0;
	lsc->pdm_clk_en = 0;
	lsc->uart1_clk_en = 0;
	lsc->uart2_clk_en = 0;
	lsc->i3c_slv0_sys_clk_en = 0;
	lsc->i3c_slv1_sys_clk_en = 0;
	lsc->pwm012_clk_en = 0;
	lsc->i2s_slv_sclk_en = 0;
	lsc->i2c_slv0_ic_clk_en = 0;
	lsc->i2c_slv1_ic_clk_en = 0;
	lsc->i2c_mst_ic_clk_en = 0;
	lsc->i2c_mst_sen_ic_clk_en = 0;
	lsc->vad_d_clk_en = 0;
	lsc->adcck_en = 0;
	lsc->sspis_en = 0;
	lsc->ckmon_en = 0;
	lsc->imageclk_en.sc_clk_lsc_en = 0;
}

/**
 * @brief Clears the enables of the SB peripherals this app does not use.
 *
 * Keeps TIMER0-TIMER2 and WDT0 (used by the delay function and the watchdog reset) and SB GPIO.
 *
 * @param sb The SB clock enables to modify.
 */
static void gateSb(SCU_PDSB_CLKEN_CFG_T *sb) {
	sb->ts_clk_en = 0;
	sb->adc_lp_hv_clk_en = 0;
	sb->WDT1_en = 0;
	sb->TIMER3_en = 0;
	sb->TIMER4_en = 0;
	sb->TIMER5_en = 0;
	sb->TIMER6_en = 0;
	sb->TIMER7_en = 0;
	sb->TIMER8_en = 0;
	sb->hmxi2cm_en = 0;
}

/**
 * @brief Tells the rest of the system that the CPU clock has changed.
 *
 * Updates SystemCoreClock and sets up SysTick again. The FreeRTOS port works out the tick period, and
 * the longest tickless sleep, from SystemCoreClock, so kernel time stays correct after the change.
 * Call it with the scheduler running but from a critical section.
 */
static void speedChanged(void) {
	uint32_t freq = 0;

	hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_CM55M, &freq);
	SystemCoreClockUpdate(freq);
	vPortSetupTimerInterrupt();
}

/**
 * @brief Checks whether the CPU or bus clocks are running from the PLL.
 *
 * @return true if the HSC or LSC clock uses the PLL as its source.
 */
static bool pllInUse(void) {
	SCU_PDHSC_HSCCLK_CFG_T hscClk;
	SCU_LSC_CLK_CFG_T lscClk;

	hx_drv_scu_get_pdhsc_hscclk_cfg(&hscClk);
	hx_drv_scu_get_pdlsc_lscclk_cfg(&lscClk);

	return (hscClk.hscclk.hscclksrc == SCU_HSCCLKSRC_PLL) || (lscClk.lscclksrc == SCU_LSCCLKSRC_PLL);
}

/**
 * @brief Checks whether anything we know of is using the 24 MHz crystal as a clock source.
 *
 * Looks at the PLL input (while the PLL is running), the HSC and LSC clock sources and the LSC reference
 * clock (which feeds the console UART, so switching the crystal off while the UART uses it stops the console).
 * Other users (the OSPI clock, the sensor clocks) are not checked. The PLL is fed from the crystal, so the crystal is
 * only free to be switched off after power_diag_pll() has switched the PLL off.
 *
 * @return true if the running PLL, the HSC clock, the LSC clock or the LSC reference clock is taken from the
 *         24 MHz crystal.
 */
static bool xtal24InUse(void) {
	SCU_PLLCLKSRC_E pllSrc = SCU_PLLCLKSRC_RC24M1M;
	SCU_PDHSC_HSCCLK_CFG_T hscClk;
	SCU_LSC_CLK_CFG_T lscClk;
	SCU_PDLSC_LSCREF_CFG_T lscRef;

	hx_drv_scu_get_pll_src(&pllSrc);
	hx_drv_scu_get_pdhsc_hscclk_cfg(&hscClk);
	hx_drv_scu_get_pdlsc_lscclk_cfg(&lscClk);
	hx_drv_scu_get_pdlsc_lscrefclk_cfg(&lscRef);

	return ((pllSrc == SCU_PLLCLKSRC_XTAL24M) && !pllOff) || (hscClk.hscclk.hscclksrc == SCU_HSCCLKSRC_XTAL24M) ||
			(lscClk.lscclksrc == SCU_LSCCLKSRC_XTAL24M) || (lscRef.lscref == SCU_LSCREFCLKSRC_XTAL24M);
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Measures how often the idle loop runs, to show whether tickless idle is sleeping.
 *
 * Blocks the calling task for the duration, so call it from a task (such as the CLI) and make sure
 * every other task is blocked. Prints the result and a verdict.
 *
 * @param durationMs How long to measure, in ms.
 */
void power_diag_measureIdle(uint32_t durationMs) {
	uint32_t startCount;
	uint32_t hookCalls;
	TickType_t startTick;
	uint32_t elapsedMs;
	uint32_t countsPerTick;
	uint32_t maxTicks;
	uint32_t expectedPerSecond;
	uint32_t actualPerSecond;

	startCount = inactivity_getIdleHookCount();
	startTick = xTaskGetTickCount();

	vTaskDelay(pdMS_TO_TICKS(durationMs));

	hookCalls = inactivity_getIdleHookCount() - startCount;
	elapsedMs = ((xTaskGetTickCount() - startTick) * 1000) / configTICK_RATE_HZ;

	countsPerTick = SystemCoreClock / configTICK_RATE_HZ;
	maxTicks = (countsPerTick != 0) ? (SYSTICK_MAX_COUNT / countsPerTick) : 0;
	expectedPerSecond = (maxTicks != 0) ? (1000 / maxTicks) : 0;
	actualPerSecond = (elapsedMs != 0) ? ((hookCalls * 1000) / elapsedMs) : 0;

	XP_LT_CYAN;
	xprintf("Tickless idle check\n");
	XP_WHITE;
	xprintf("  configUSE_TICKLESS_IDLE = %d, tick rate %d Hz, SystemCoreClock = %u Hz\n",
			configUSE_TICKLESS_IDLE, configTICK_RATE_HZ, (unsigned) SystemCoreClock);
	xprintf("  Longest sleep per SysTick reload = %u ticks, so about %u idle-hook calls/s when sleeping\n",
			(unsigned) maxTicks, (unsigned) expectedPerSecond);
	xprintf("  Measured: %u idle-hook calls in %u ms = %u/s\n",
			(unsigned) hookCalls, (unsigned) elapsedMs, (unsigned) actualPerSecond);

	if (actualPerSecond > ((expectedPerSecond + 1) * SPINNING_FACTOR)) {
		XP_LT_RED;
		xprintf("  Verdict: the idle task is SPINNING - tickless idle is not putting the CPU to sleep\n");
	}
	else {
		XP_LT_GREEN;
		xprintf("  Verdict: consistent with the CPU sleeping (WFI) between wakeups\n");
	}
	XP_WHITE;
}

/**
 * @brief Prints the clock frequencies and which clock enables are set.
 *
 * WFI stops only the CPU clock. Everything listed as enabled here still has its clock
 * running while the CPU sleeps, which is where the awake-and-idle current comes from.
 */
void power_diag_printClocks(void) {
	SCU_PDHSC_CLKEN_CFG_T hsc;
	SCU_PDLSC_CLKEN_CFG_T lsc;
	SCU_PDSB_CLKEN_CFG_T sb;
	SCU_PDAON_CLKEN_CFG_T aon;
	uint32_t pllFreq = 0;
	SCU_PLLCLKSRC_E pllSrc;
	SCU_PDHSC_HSCCLK_CFG_T hscClk;
	SCU_LSC_CLK_CFG_T lscClk;
	SCU_PDLSC_LSCREF_CFG_T lscRef;
	uint8_t xtal24En = 0;
	uint8_t xtal32En = 0;
	SCU_XTAL24MSEL_E xtal24Sel = SCU_XTAL24MSEL_1M_4M;
	SCU_HSC32KCLKSRC_E hsc32k = SCU_HSC32KCLKSRC_RC32K1K;
	SCU_LSC32KCLKSRC_E lsc32k = SCU_LSC32KCLKSRC_RC32K1K;

	XP_LT_CYAN;
	xprintf("Clock frequencies\n");
	XP_WHITE;
	printFreq("PLL", SCU_CLK_FREQ_TYPE_PLL);
	printFreq("HSC_CLK", SCU_CLK_FREQ_TYPE_HSC_CLK);
	printFreq("CM55M", SCU_CLK_FREQ_TYPE_HSC_CM55M);
	printFreq("U55", SCU_CLK_FREQ_TYPE_HSC_U55);
	printFreq("AXI", SCU_CLK_FREQ_TYPE_HSC_AXI);
	printFreq("LSC_CLK", SCU_CLK_FREQ_TYPE_LSC_CLK);
	printFreq("CM55S", SCU_CLK_FREQ_TYPE_LSC_CM55S);
	printFreq("LSC_REF_CLK", SCU_CLK_FREQ_TYPE_LSC_REF_CLK);
	printFreq("UART", SCU_CLK_FREQ_TYPE_LSC_UART);
	hx_drv_swreg_aon_get_pllfreq(&pllFreq);
	xprintf("  AON PLL setting %u Hz\n", (unsigned) pllFreq);
	xprintf("  SystemCoreClock %u Hz\n", (unsigned) SystemCoreClock);

	XP_LT_CYAN;
	xprintf("Clock sources\n");
	XP_WHITE;
	if ((hx_drv_scu_get_pll_src(&pllSrc) == SCU_NO_ERROR) && (pllSrc < 3)) {
		xprintf("  PLL is fed from %s\n", pllSrcName[pllSrc]);
	}
	if ((hx_drv_scu_get_pdhsc_hscclk_cfg(&hscClk) == SCU_NO_ERROR) && (hscClk.hscclk.hscclksrc < 4)) {
		xprintf("  HSC_CLK source %s, divider %d\n", clkSrcName[hscClk.hscclk.hscclksrc], (int) hscClk.hscclk.hscclkdiv + 1);
	}
	if ((hx_drv_scu_get_pdlsc_lscclk_cfg(&lscClk) == SCU_NO_ERROR) && (lscClk.lscclksrc < 4)) {
		xprintf("  LSC_CLK source %s, divider %d\n", clkSrcName[lscClk.lscclksrc], (int) lscClk.lscclkdiv + 1);
	}
	if ((hx_drv_scu_get_pdlsc_lscrefclk_cfg(&lscRef) == SCU_NO_ERROR) && (lscRef.lscref < 4)) {
		xprintf("  LSC_REF_CLK (feeds the UART, divider %d) source %s\n", (int) lscRef.uart_div, lscRefName[lscRef.lscref]);
	}

	XP_LT_CYAN;
	xprintf("Oscillators\n");
	XP_WHITE;
	if ((hx_drv_scu_get_xtal24m_en(&xtal24En) == SCU_NO_ERROR) && (hx_drv_scu_get_xtal24m_sel(&xtal24Sel) == SCU_NO_ERROR)) {
		xprintf("  24 MHz crystal oscillator %s (frequency range selection %d)\n", xtal24En ? "ENABLED" : "disabled", (int) xtal24Sel);
	}
	if (hx_drv_scu_get_xtal32k_en(&xtal32En) == SCU_NO_ERROR) {
		xprintf("  32.768 kHz crystal oscillator %s\n", xtal32En ? "ENABLED" : "disabled");
	}
	if ((hx_drv_scu_get_pdhsc_hsc32kclk_cfg(&hsc32k) == SCU_NO_ERROR) && (hsc32k < 2)) {
		xprintf("  HSC 32 kHz clock from %s\n", (hsc32k == SCU_HSC32KCLKSRC_XTAL32K) ? "XTAL32K" : "RC32K1K");
	}
	if ((hx_drv_scu_get_pdlsc_32k_cfg(&lsc32k) == SCU_NO_ERROR) && (lsc32k < 2)) {
		xprintf("  LSC 32 kHz clock from %s\n", (lsc32k == SCU_LSC32KCLKSRC_XTAL32K) ? "XTAL32K" : "RC32K1K");
	}

	XP_LT_CYAN;
	xprintf("Clock enables that are set\n");
	XP_WHITE;

	if (hx_drv_scu_get_pdhsc_clken_cfg(&hsc) == SCU_NO_ERROR) {
		xprintf("  HSC:");
		printFlag("cm55m", hsc.cm55m_clk_en);
		printFlag("u55", hsc.u55_clk_en);
		printFlag("axi", hsc.axi_clk_en);
		printFlag("ahb0", hsc.ahb0_clk_en);
		printFlag("ahb5", hsc.ahb5_clk_en);
		printFlag("ahb1", hsc.ahb1_clk_en);
		printFlag("apb2", hsc.apb2_clk_en);
		printFlag("rom", hsc.rom_clk_en);
		printFlag("sram0", hsc.sram0_clk_en);
		printFlag("sram1", hsc.sram1_clk_en);
		printFlag("i3c_hc", hsc.i3c_hc_clk_en);
		printFlag("puf", hsc.puf_clk_en);
		printFlag("dma0", hsc.dma0_clk_en);
		printFlag("dma1", hsc.dma1_clk_en);
		printFlag("sdio", hsc.sdio_clk_en);
		printFlag("i2c2ahb_flash_w", hsc.i2c2ahb_flash_w_clk_en);
		printFlag("qspi", hsc.qspi_en);
		printFlag("ospi", hsc.ospi_en);
		printFlag("spi2ahb", hsc.spi2ahb_en);
		xprintf("\n  HSC image:");
		printFlag("xdma_w1", hsc.imageclk_en.xdma_w1_clk_en);
		printFlag("xdma_w2", hsc.imageclk_en.xdma_w2_clk_en);
		printFlag("xdma_w3", hsc.imageclk_en.xdma_w3_clk_en);
		printFlag("xdma_r", hsc.imageclk_en.xdma_r_clk_en);
		printFlag("sc", hsc.imageclk_en.scclk_clk_en);
		printFlag("inp", hsc.imageclk_en.inp_clk_en);
		printFlag("dp", hsc.imageclk_en.dp_clk_en);
		printFlag("2x2", hsc.imageclk_en.dp_2x2_clk_en);
		printFlag("5x5", hsc.imageclk_en.dp_5x5_clk_en);
		printFlag("cdm", hsc.imageclk_en.dp_cdm_clk_en);
		printFlag("jpeg", hsc.imageclk_en.dp_jpeg_clk_en);
		printFlag("tpg", hsc.imageclk_en.dp_tpg_clk_en);
		printFlag("edm", hsc.imageclk_en.dp_edm_clk_en);
		printFlag("rgb2yuv", hsc.imageclk_en.dp_rgb2yuv_pclk_en);
		printFlag("csc", hsc.imageclk_en.dp_csc_pclk_en);
		printFlag("mipirx", hsc.imageclk_en.mipirx_clk_en);
		printFlag("mipitx", hsc.imageclk_en.mipitx_clk_en);
		xprintf("\n");
	}

	if (hx_drv_scu_get_pdlsc_clken_cfg(&lsc) == SCU_NO_ERROR) {
		xprintf("  LSC:");
		printFlag("cm55s", lsc.cm55s_clk_en);
		printFlag("ahb_m", lsc.ahb_m_hclk_en);
		printFlag("ahb_2", lsc.ahb_2_hclk_en);
		printFlag("ahb_3", lsc.ahb_3_hclk_en);
		printFlag("apb_0", lsc.apb_0_hclk_en);
		printFlag("sram2", lsc.sram2_clk_en);
		printFlag("dma2", lsc.dma2_clk_en);
		printFlag("dma3", lsc.dma3_clk_en);
		printFlag("i2s_host", lsc.i2s_host_sclk_en);
		printFlag("pdm", lsc.pdm_clk_en);
		printFlag("uart0", lsc.uart0_clk_en);
		printFlag("uart1", lsc.uart1_clk_en);
		printFlag("uart2", lsc.uart2_clk_en);
		printFlag("i3c_slv0", lsc.i3c_slv0_sys_clk_en);
		printFlag("i3c_slv1", lsc.i3c_slv1_sys_clk_en);
		printFlag("pwm012", lsc.pwm012_clk_en);
		printFlag("i2s_slv", lsc.i2s_slv_sclk_en);
		printFlag("ro_pd", lsc.ro_pd_clk_en);
		printFlag("i2c_slv0", lsc.i2c_slv0_ic_clk_en);
		printFlag("i2c_slv1", lsc.i2c_slv1_ic_clk_en);
		printFlag("i2c_mst", lsc.i2c_mst_ic_clk_en);
		printFlag("i2c_mst_sen", lsc.i2c_mst_sen_ic_clk_en);
		printFlag("sw", lsc.sw_clk_en);
		printFlag("vad_d", lsc.vad_d_clk_en);
		printFlag("adcck", lsc.adcck_en);
		printFlag("gpio", lsc.gpio_en);
		printFlag("sspim", lsc.sspim_en);
		printFlag("sspis", lsc.sspis_en);
		printFlag("ckmon", lsc.ckmon_en);
		printFlag("sc", lsc.imageclk_en.sc_clk_lsc_en);
		xprintf("\n");
	}

	if (hx_drv_scu_get_pdsb_clken_cfg(&sb) == SCU_NO_ERROR) {
		xprintf("  SB:");
		printFlag("apb1_ahb4", sb.apb1_ahb4_pclk_en);
		printFlag("ts", sb.ts_clk_en);
		printFlag("adc_lp_hv", sb.adc_lp_hv_clk_en);
		printFlag("i2c2ahb_dbg", sb.I2C2AHB_DBG_en);
		printFlag("wdt0", sb.WDT0_en);
		printFlag("wdt1", sb.WDT1_en);
		printFlag("timer0", sb.TIMER0_en);
		printFlag("timer1", sb.TIMER1_en);
		printFlag("timer2", sb.TIMER2_en);
		printFlag("timer3", sb.TIMER3_en);
		printFlag("timer4", sb.TIMER4_en);
		printFlag("timer5", sb.TIMER5_en);
		printFlag("timer6", sb.TIMER6_en);
		printFlag("timer7", sb.TIMER7_en);
		printFlag("timer8", sb.TIMER8_en);
		printFlag("sb_gpio", sb.sb_gpio_en);
		printFlag("hmxi2cm", sb.hmxi2cm_en);
		xprintf("\n");
	}

	if (hx_drv_scu_get_pdaon_clken_cfg(&aon) == SCU_NO_ERROR) {
		xprintf("  AON:");
		printFlag("rtc0", aon.rtc0_clk_en);
		printFlag("rtc1", aon.rtc1_clk_en);
		printFlag("rtc2", aon.rtc2_clk_en);
		printFlag("pmu", aon.pmu_clk_en);
		printFlag("aon_gpio", aon.aon_gpio_clk_en);
		printFlag("aon_swreg", aon.aon_swreg_clk_en);
		printFlag("antitamper", aon.antitamper_clk_en);
		xprintf("\n");
	}
}

/**
 * @brief Switches off the clock enables of a group of unused peripherals. EXPERIMENT.
 *
 * The groups are cumulative if called repeatedly. The enables are saved first, so
 * power_diag_restoreClocks() can put them back. A reset or DPD wake also restores them.
 *
 * Groups: "image" (camera data path), "hsc" (U55, I3C host, PUF, DMA0/1, SDIO), "flash" (QSPI,
 * OSPI, SPI2AHB, I2C2AHB flash write), "lsc" (unused LSC peripherals and the CM55S core),
 * "sb" (unused timers, WDT1, temperature and ADC), "all" (image, hsc, lsc and sb, not flash).
 * The blocks of "hsc" can also be gated one at a time: "u55", "i3c", "puf", "dma" and "sdio".
 *
 * @param group The name of the group.
 * @return true if the group is known.
 */
bool power_diag_gateClocks(const char *group) {
	SCU_PDHSC_CLKEN_CFG_T hsc;
	SCU_PDLSC_CLKEN_CFG_T lsc;
	SCU_PDSB_CLKEN_CFG_T sb;
	bool all = (strcmp(group, "all") == 0);
	bool hscPart = (strcmp(group, "u55") == 0) || (strcmp(group, "i3c") == 0) || (strcmp(group, "puf") == 0) ||
			(strcmp(group, "dma") == 0) || (strcmp(group, "sdio") == 0);

	if (!all && !hscPart && (strcmp(group, "image") != 0) && (strcmp(group, "hsc") != 0) && (strcmp(group, "flash") != 0) &&
			(strcmp(group, "lsc") != 0) && (strcmp(group, "sb") != 0)) {
		return false;
	}

	saveClocks();

	hx_drv_scu_get_pdhsc_clken_cfg(&hsc);
	hx_drv_scu_get_pdlsc_clken_cfg(&lsc);
	hx_drv_scu_get_pdsb_clken_cfg(&sb);

	if (all || (strcmp(group, "image") == 0)) {
		gateImage(&hsc);
	}
	if (all || (strcmp(group, "hsc") == 0)) {
		gateHsc(&hsc);
	}
	if (hscPart) {
		gateHscPart(&hsc, group);
	}
	if (strcmp(group, "flash") == 0) {
		gateFlash(&hsc);
	}
	if (all || (strcmp(group, "lsc") == 0)) {
		gateLsc(&lsc);
	}
	if (all || (strcmp(group, "sb") == 0)) {
		gateSb(&sb);
	}

	hx_drv_scu_set_pdhsc_clken_cfg(hsc);
	hx_drv_scu_set_pdlsc_clken_cfg(lsc);
	hx_drv_scu_set_pdsb_clken_cfg(sb);

	return true;
}

/**
 * @brief Switches the CPU and bus clocks to a 24 MHz oscillator. EXPERIMENT.
 *
 * Both the HSC clock (CM55M, U55, AXI, the AHB/APB buses) and the LSC clock change, with dividers of 1.
 * SystemCoreClock and the FreeRTOS tick are then set up again, so kernel time stays correct but the
 * longest tickless sleep becomes much longer (about 699 ms at 24 MHz).
 *
 * The PLL is left running: use power_diag_pll() to switch it off. The console UART has its own 24 MHz
 * clock and should be unaffected.
 *
 * @param source "rc" for the 24 MHz RC oscillator or "xtal" for the 24 MHz crystal. Do not use "xtal"
 *               unless a crystal is fitted: without one the CPU clock stops and the board hangs.
 * @return true if the source is known.
 */
bool power_diag_slowClock(const char *source) {
	SCU_PDHSC_HSCCLK_CFG_T hscClk;
	SCU_LSC_CLK_CFG_T lscClk;
	SCU_HSCCLKSRC_E hscSrc;
	SCU_LSCCLKSRC_E lscSrc;

	if (strcmp(source, "rc") == 0) {
		hscSrc = SCU_HSCCLKSRC_RC24M1M;
		lscSrc = SCU_LSCCLKSRC_RC24M1M;
	}
	else if (strcmp(source, "xtal") == 0) {
		hscSrc = SCU_HSCCLKSRC_XTAL24M;
		lscSrc = SCU_LSCCLKSRC_XTAL24M;
	}
	else {
		return false;
	}

	if (!speedSaved) {
		hx_drv_scu_get_pdhsc_hscclk_cfg(&savedHscClk);
		hx_drv_scu_get_pdlsc_lscclk_cfg(&savedLscClk);
		speedSaved = true;
	}

	hscClk = savedHscClk;
	hscClk.hscclk.hscclksrc = hscSrc;
	hscClk.hscclk.hscclkdiv = SCU_HSCCLKDIV_1;

	lscClk = savedLscClk;
	lscClk.lscclksrc = lscSrc;
	lscClk.lscclkdiv = SCU_LSCCLKDIV_1;

	taskENTER_CRITICAL();
	hx_drv_scu_set_pdhsc_hscclk_cfg(hscClk);
	hx_drv_scu_set_pdlsc_lscclk_cfg(lscClk);
	speedChanged();
	taskEXIT_CRITICAL();

	return true;
}

/**
 * @brief Moves the LSC reference clock, which the console UART runs from, to the RC oscillator or crystal. EXPERIMENT.
 *
 * The UART clock is the LSC reference clock divided by uart_div. It is normally taken from the 24 MHz crystal,
 * so switching the crystal off stops the console. Moving it to the 24 MHz RC oscillator first lets the crystal
 * be switched off with the console still working. The RC oscillator is factory trimmed but less accurate than
 * the crystal, so the console baud rate may be slightly off: if the text is garbled, power-cycle.
 * power_diag_fastClock() puts the original source back.
 *
 * @param source "rc" for the 24 MHz RC oscillator or "xtal" for the crystal.
 * @return true if the source is known.
 */
bool power_diag_uartClock(const char *source) {
	SCU_PDLSC_LSCREF_CFG_T lscRef;
	SCU_LSCREFCLKSRC_E newSource;

	if (strcmp(source, "rc") == 0) {
		newSource = SCU_LSCREFCLKSRC_RC24M1M;
	}
	else if (strcmp(source, "xtal") == 0) {
		newSource = SCU_LSCREFCLKSRC_XTAL24M;
	}
	else {
		return false;
	}

	hx_drv_scu_get_pdlsc_lscrefclk_cfg(&lscRef);

	if (!lscRefSaved) {
		savedLscRef = lscRef;
		lscRefSaved = true;
	}

	lscRef.lscref = newSource;
	hx_drv_scu_set_pdlsc_lscrefclk_cfg(lscRef);

	return true;
}

/**
 * @brief Divides the slow CPU and bus clock further. EXPERIMENT.
 *
 * Sets the HSC and LSC dividers (1 to 16) while they are running from an oscillator. It is refused if
 * either clock is still taken from the PLL, so use power_diag_slowClock() first. SystemCoreClock and the
 * FreeRTOS tick are set up again, as for power_diag_slowClock(). The 24 MHz oscillator divided by 16 is
 * 1.5 MHz, so the tick interrupt takes a noticeable share of the CPU time at the high dividers.
 *
 * @param divider 1 to 16.
 * @return true if the divider was applied.
 */
bool power_diag_divideClock(uint32_t divider) {
	SCU_PDHSC_HSCCLK_CFG_T hscClk;
	SCU_LSC_CLK_CFG_T lscClk;

	if ((divider < 1) || (divider > 16) || !speedSaved || pllInUse()) {
		return false;
	}

	hx_drv_scu_get_pdhsc_hscclk_cfg(&hscClk);
	hx_drv_scu_get_pdlsc_lscclk_cfg(&lscClk);

	hscClk.hscclk.hscclkdiv = (SCU_HSCCLKDIV_E) (SCU_HSCCLKDIV_1 + (divider - 1));
	lscClk.lscclkdiv = (SCU_LSCCLKDIV_E) (SCU_LSCCLKDIV_1 + (divider - 1));

	taskENTER_CRITICAL();
	hx_drv_scu_set_pdhsc_hscclk_cfg(hscClk);
	hx_drv_scu_set_pdlsc_lscclk_cfg(lscClk);
	speedChanged();
	taskEXIT_CRITICAL();

	return true;
}

/**
 * @brief Switches the PLL off or on. EXPERIMENT.
 *
 * The PLL is only switched off if the HSC and LSC clocks are not using it (use power_diag_slowClock()
 * first). Switching it on first switches the 24 MHz crystal oscillator back on if the PLL is fed from it
 * and power_diag_crystal() has switched it off, and waits for it to start, then waits PLL_LOCK_DELAY_MS.
 *
 * @param enable true to switch the PLL on, false to switch it off.
 * @return true if the PLL is now in the requested state, false if it was refused.
 */
bool power_diag_pll(bool enable) {
	if (!enable) {
		if (pllInUse()) {
			return false;
		}
		hx_drv_scu_set_pll_enable(0);
		pllOff = true;
	}
	else {
		SCU_PLLCLKSRC_E pllSrc = SCU_PLLCLKSRC_RC24M1M;
		uint8_t xtal24En = 1;

		// The PLL cannot lock without its input, so restart the crystal first if it was switched off
		hx_drv_scu_get_pll_src(&pllSrc);
		hx_drv_scu_get_xtal24m_en(&xtal24En);
		if ((pllSrc == SCU_PLLCLKSRC_XTAL24M) && (xtal24En == 0)) {
			hx_drv_scu_set_xtal24m_en(1);
			vTaskDelay(pdMS_TO_TICKS(CRYSTAL_START_DELAY_MS));
		}

		hx_drv_scu_set_pll_enable(1);
		vTaskDelay(pdMS_TO_TICKS(PLL_LOCK_DELAY_MS));
		pllOff = false;
	}

	return true;
}

/**
 * @brief Returns the CPU and bus clocks to how they were before power_diag_slowClock().
 *
 * Switches the PLL back on first if power_diag_pll() switched it off, restarting the 24 MHz crystal if it was
 * switched off, and puts the LSC reference clock (the UART clock) back if power_diag_uartClock() moved it.
 * Does nothing if none of the experiments has been used.
 */
void power_diag_fastClock(void) {
	if (pllOff) {
		power_diag_pll(true);
	}

	if (speedSaved) {
		taskENTER_CRITICAL();
		hx_drv_scu_set_pdhsc_hscclk_cfg(savedHscClk);
		hx_drv_scu_set_pdlsc_lscclk_cfg(savedLscClk);
		speedChanged();
		taskEXIT_CRITICAL();
		speedSaved = false;
	}

	if (lscRefSaved) {
		hx_drv_scu_set_pdlsc_lscrefclk_cfg(savedLscRef);
		lscRefSaved = false;
	}
}

/**
 * @brief Switches the 24 MHz or the 32.768 kHz crystal oscillator off or on. EXPERIMENT.
 *
 * Both are enabled by default and nothing in this application changes that. The 24 MHz oscillator is not
 * switched off while the PLL, the HSC clock or the LSC clock is using it (so use power_diag_slowClock()
 * with "rc" first if it is). The 32.768 kHz oscillator has no crystal on the WW500, so switching it off
 * should only save the current it draws.
 *
 * @param which  24 or 32.
 * @param enable true to enable the oscillator, false to disable it.
 * @return true if it is now in the requested state, false if the request was refused or is unknown.
 */
bool power_diag_crystal(uint32_t which, bool enable) {
	if ((which != 24) && (which != 32)) {
		return false;
	}

	if ((which == 24) && !enable && xtal24InUse()) {
		return false;
	}

	if (!crystalsSaved) {
		hx_drv_scu_get_xtal24m_en(&savedXtal24En);
		hx_drv_scu_get_xtal32k_en(&savedXtal32En);
		crystalsSaved = true;
	}

	if (which == 24) {
		hx_drv_scu_set_xtal24m_en(enable ? 1 : 0);
	}
	else {
		hx_drv_scu_set_xtal32k_en(enable ? 1 : 0);
	}

	return true;
}

/**
 * @brief Undoes the experiments: restores the clock speed, the PLL, the oscillators and the clock enables.
 *
 * Does nothing for an experiment that has not been used. Call it before entering DPD or Power-down.
 */
void power_diag_restoreClocks(void) {
	power_diag_fastClock();

	if (crystalsSaved) {
		hx_drv_scu_set_xtal24m_en(savedXtal24En);
		hx_drv_scu_set_xtal32k_en(savedXtal32En);
		crystalsSaved = false;
	}

	if (clocksSaved) {
		hx_drv_scu_set_pdhsc_clken_cfg(savedHsc);
		hx_drv_scu_set_pdlsc_clken_cfg(savedLsc);
		hx_drv_scu_set_pdsb_clken_cfg(savedSb);
	}
}
