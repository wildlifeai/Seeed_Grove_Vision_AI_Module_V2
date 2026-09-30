/*
 * pinmux_cfg.h
 *
 *  Created on: 27 Sep 2026
 *      Author: Charles Palmer
 *
 *  Pin multiplexing for the 'ww500_minimal' app: sets the function of every pin the app uses, and the
 *  initial level of those that are GPIO outputs. Everything else is left alone so that it draws no current.
 *
 *  Pins (WW500_C00 / C02):
 *   - PB0, PB1:  UART0 (console)
 *   - PB2 - PB5: SPI master for the SD card (the SD card driver takes PB5 over as a GPIO while it needs it)
 *   - PB7:  SENSOR_ENABLE, GPIO1, output low (the RP camera enable; not needed by the HM0360)
 *   - PB8:  SWDIO (its function after reset). Not used for the LED because it has a pull-up
 *   - PB9:  red LED (LED1), GPIO0, active high, off
 *   - PB10: function 0, an input (was the blue LED)
 *   - PB11: blue LED (LED2), GPIO2, active high, off
 *
 *  GPIO0, GPIO1 and GPIO2 can each appear on two pins (PB6/PB9, PB7/PB10, PB8/PB11: HX6538 datasheet section
 *  4.5, note 3). They are one signal each, not two, so PB8 and PB10 must not also be set to GPIO2 or GPIO1.
 *  The blue LED was on PB10 as GPIO1, which tied it to SENSOR_ENABLE on PB7; it is now on PB11 as GPIO2.
 *
 *  PB7 and PB8 are also the SWD pins (SWCLK, SWDIO), which the bootloader sets up. PB8 is left as SWDIO but
 *  PB7 becomes a GPIO once pinmux_cfg_init() has run, so SWD can only connect in the short time between the
 *  bootloader and the app.
 */

#ifndef PINMUX_CFG_H_
#define PINMUX_CFG_H_

/*********************************************** Includes ****************************************************/

#include <stdbool.h>

/********************************************** Global Defines ***********************************************/

/*********************************************** Global Types ************************************************/

/********************************************* Global Variables **********************************************/

/*************************************** Global Function Declarations ****************************************/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sets the function of every pin the app uses. Call once, early in app_main().
 */
void pinmux_cfg_init(void);

/**
 * @brief Drives SENSOR_ENABLE (PB7), which powers the RP camera. The equivalent of rp_sensor_enable() in ww500_md.
 */
void pinmux_cfg_rpSensorEnable(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* PINMUX_CFG_H_ */
