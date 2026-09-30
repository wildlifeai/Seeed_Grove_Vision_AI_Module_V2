/*
 * pinmux_cfg.c
 *
 *  Created on: 27 Sep 2026
 *      Author: Charles Palmer
 *
 *  Pin multiplexing for the 'ww500_minimal' app. See pinmux_cfg.h for the pins and why they are set as they are.
 *
 *  The GPIO outputs are set up in the order ww500_md uses: direction and level first, then the pin function,
 *  then the level again, so the pin is never driven high on the way.
 */

/*********************************************** Includes ****************************************************/

#include "hx_drv_scu_export.h"
#include "hx_drv_scu.h"
#include "hx_drv_gpio.h"

#include "xprintf.h"

#include "pinmux_cfg.h"

/*********************************************** Local Defines ***********************************************/

/********************************************** Local Variables **********************************************/

/**************************************** Local Function Declarations ****************************************/

static void initGpioOutputs(void);

/**************************************** Local Function Definitions *****************************************/

/**
 * @brief Sets the GPIO output pins: both LEDs off and SENSOR_ENABLE low.
 *
 * PB7 is set low whatever camera is fitted. SCU_PB7_PINMUX_GPIO1_1 is the value ww500_md uses for
 * SENSOR_ENABLE in its RP camera builds, where it is known to work as an output. SCU_PB11_PINMUX_GPIO2 is the
 * only GPIO2 value for PB11, and the one the Himax SDK examples use.
 */
static void initGpioOutputs(void) {
	// PB7 = SENSOR_ENABLE, GPIO1, low
	hx_drv_gpio_set_output(GPIO1, GPIO_OUT_LOW);
	hx_drv_scu_set_PB7_pinmux(SCU_PB7_PINMUX_GPIO1_1, 1);
	hx_drv_gpio_set_out_value(GPIO1, GPIO_OUT_LOW);

	// PB11 = blue LED (LED2), GPIO2, active high, off
	hx_drv_gpio_set_output(GPIO2, GPIO_OUT_LOW);
	hx_drv_scu_set_PB11_pinmux(SCU_PB11_PINMUX_GPIO2, 1);
	hx_drv_gpio_set_out_value(GPIO2, GPIO_OUT_LOW);

	// PB9 = red LED (LED1), GPIO0, active high, off
	hx_drv_gpio_set_output(GPIO0, GPIO_OUT_LOW);
	hx_drv_scu_set_PB9_pinmux(SCU_PB9_PINMUX_GPIO0, 1);
	hx_drv_gpio_set_out_value(GPIO0, GPIO_OUT_LOW);
}

/**************************************** Global Function Definitions ****************************************/

/**
 * @brief Sets the function of every pin the app uses. Call once, early in app_main().
 *
 * The console UART (PB0, PB1) and the SPI master for the SD card (PB2 data out, PB3 data in, PB4 clock,
 * PB5 chip select) are set together, with PB8 set to SWDIO and PB10 set to an input (so that GPIO2 and GPIO1
 * appear only on PB11 and PB7), then the GPIO outputs (PB7, PB9, PB11). Every other pin is left as it was, so
 * that it draws no current.
 *
 * NOTE: there is a weak version of pinmux_init() in board/epii_evb/pinmux_init.c that just
 * initialises PB0 and PB1 for UART.
 */
void pinmux_cfg_init(void) {
	SCU_PINMUX_CFG_T pinmux_cfg;

	hx_drv_scu_get_all_pinmux_cfg(&pinmux_cfg);

	/* Init UART0 pin mux to PB0 and PB1 */
	pinmux_cfg.pin_pb0 = SCU_PB0_PINMUX_UART0_RX_1;
	pinmux_cfg.pin_pb1 = SCU_PB1_PINMUX_UART0_TX_1;

	/* Init the SPI master pin mux for the SD card. The SD card driver takes PB5 over as a GPIO while it needs it */
	pinmux_cfg.pin_pb2 = SCU_PB2_PINMUX_SPI_M_DO_1;
	pinmux_cfg.pin_pb3 = SCU_PB3_PINMUX_SPI_M_DI_1;
	pinmux_cfg.pin_pb4 = SCU_PB4_PINMUX_SPI_M_SCLK_1;
	pinmux_cfg.pin_pb5 = SCU_PB5_PINMUX_SPI_M_CS_1;

	/* PB10 (the blue LED's old pin) to function 0, an input (NA, or XSHUTDOWN in on later IC versions, so the
	 * number is used), so that GPIO1 is not also routed to it */
	pinmux_cfg.pin_pb10 = 0;

	/* PB8 (has a pull-up, so not used for the LED) to SWDIO, its function after reset, so that GPIO2 is not also
	 * routed to it */
	pinmux_cfg.pin_pb8 = SCU_PB8_PINMUX_SWDIO_0;

	hx_drv_scu_set_all_pinmux_cfg(&pinmux_cfg, 1);

	initGpioOutputs();
}

/**
 * @brief Drives SENSOR_ENABLE (PB7), which powers the RP camera. The equivalent of rp_sensor_enable() in ww500_md.
 *
 * PB7 is already a GPIO1 output (see pinmux_cfg_init()), so only the level is changed. The RP camera loses its
 * registers when this is low, so it must be initialised again after it is enabled. The HM0360 does not use it.
 *
 * @param enable True to power the RP camera (PB7 high), false to power it down (PB7 low).
 */
void pinmux_cfg_rpSensorEnable(bool enable) {
	xprintf("%s RP SENSOR_ENABLE\n", enable ? "Enabling" : "Disabling");
	hx_drv_gpio_set_out_value(GPIO1, enable ? GPIO_OUT_HIGH : GPIO_OUT_LOW);
}
