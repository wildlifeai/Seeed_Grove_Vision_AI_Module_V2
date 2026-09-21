/*
 * power_diag.h
 *
 *  Created on: 21 Sep 2026
 *      Author: Charles Palmer
 *
 *  Read-only diagnostics for investigating the current the processor draws while awake:
 *   - whether FreeRTOS tickless idle really puts the CPU to sleep between events, and
 *   - which clocks are running and how fast.
 *
 *  power_diag_measureIdle() and power_diag_printClocks() change nothing. power_diag_gateClocks()
 *  and power_diag_restoreClocks() are an EXPERIMENT for finding out what each group of clocks
 *  costs: they switch clock enables off and back on at run time. Nothing is gated unless the
 *  'clkoff' CLI command is used, and a reset or DPD wake restores the defaults.
 *
 *  power_diag_slowClock(), power_diag_pll() and power_diag_fastClock() are a second EXPERIMENT:
 *  they switch the CPU and bus clock from the 400 MHz PLL to a 24 MHz oscillator, and optionally
 *  switch the PLL off, to find out what the remaining awake-idle current costs. The FreeRTOS tick is
 *  retuned so that kernel time stays correct. power_diag_restoreClocks() undoes both experiments.
 *
 *  power_diag_crystal() is a third EXPERIMENT: the datasheet says both crystal oscillators are enabled by
 *  default and nothing in this application switches them off, but the board has no 32.768 kHz crystal.
 *
 *  Used by the 'idle', 'clocks', 'clkoff', 'clkon', 'clkslow', 'clkpll', 'clkfast' and 'xtal' CLI commands.
 */

#ifndef POWER_DIAG_H_
#define POWER_DIAG_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>
#include <stdint.h>

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Switches off the clock enables of a group of unused peripherals. EXPERIMENT.
 */
bool power_diag_gateClocks(const char *group);

/**
 * @brief Switches the CPU and bus clocks to a 24 MHz oscillator. EXPERIMENT.
 */
bool power_diag_slowClock(const char *source);

/**
 * @brief Moves the LSC reference clock, which the console UART runs from, to the RC oscillator or crystal. EXPERIMENT.
 */
bool power_diag_uartClock(const char *source);

/**
 * @brief Divides the slow CPU and bus clock further (1 to 16). EXPERIMENT.
 */
bool power_diag_divideClock(uint32_t divider);

/**
 * @brief Switches the PLL off or on. It can only be switched off while nothing uses it. EXPERIMENT.
 */
bool power_diag_pll(bool enable);

/**
 * @brief Returns the CPU and bus clocks to how they were before power_diag_slowClock().
 */
void power_diag_fastClock(void);

/**
 * @brief Switches the 24 MHz or the 32.768 kHz crystal oscillator off or on. EXPERIMENT.
 */
bool power_diag_crystal(uint32_t which, bool enable);

/**
 * @brief Undoes the experiments: restores the clock speed, the PLL and the clock enables.
 */
void power_diag_restoreClocks(void);

/**
 * @brief Measures how often the idle loop runs, to show whether tickless idle is sleeping.
 */
void power_diag_measureIdle(uint32_t durationMs);

/**
 * @brief Prints the clock frequencies and which clock enables are set.
 */
void power_diag_printClocks(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_DIAG_H_ */
