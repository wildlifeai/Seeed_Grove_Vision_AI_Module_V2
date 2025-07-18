/*
 * hm0360_md.c
 *
 * Supports HM0360 for motion detection, when it is used with other cameras.
 *
 *  Created on: 8 Jul 2025
 *      Author: charl
 */

/********************************** Includes ******************************************/

#include <stdbool.h>
#include "xprintf.h"
#include "printf_x.h"	// Print colours
#include "hm0360_md.h"
#include "WE2_debug.h"
#include "hx_drv_CIS_common.h"
#include "hm0360_regs.h"


/*************************************** Defines **************************************/

// Default interval in ms between frame grabs in motion detect mode
#define DPDINTERVAL 1000

/*************************************** Local Function Declarations ******************/

static void saveMainCameraConfig(void);
static void restoreMainCameraConfig(void);
HX_CIS_ERROR_E hm0360_sensor_set_mode(uint8_t context, mode_select_t newMode, uint8_t numFrames, uint16_t sleepTime);

static uint16_t calculateSleepTime(uint32_t interval);

/*************************************** Local variables ******************************/

static uint8_t mainCameraID;

static HX_CIS_SensorSetting_t HM0360_md_init_setting[] = {
#include "../../../ww500_md/cis_sensor/cis_hm0360/HM0360_OSC_Bayer_640x480_setA_VGA_setB_QVGA_md_8b_ParallelOutput_R2.i"
};

// This is the interval in ms between frame grabs in motion detect mode
uint16_t sleepInterval;

/*************************************** Local Function Definitions *******************/

/**
 * Read and save settings of the main camera before accessing the HM0360
 */
static void saveMainCameraConfig(void) {
	hx_drv_cis_get_slaveID(&mainCameraID);
}

/**
 * Restore settings of the main camera after accessing the HM0360
 */
static void restoreMainCameraConfig(void) {
	hx_drv_cis_set_slaveID(mainCameraID);
}


/*
 * Change HM0360 operating mode
 *
 * @param context - bits to write to the context control register (PMU_CFG_3, 0x3024)
 * @param mode - one of 8 modes of MODE_SELECT register
 * @param numFrames - the number of frames to capture before sleeping
 * @param sleepTime - the time (in ms) to sleep before waking again
 * @return error code
 */
HX_CIS_ERROR_E hm0360_sensor_set_mode(uint8_t context, mode_select_t newMode, uint8_t numFrames, uint16_t sleepTime) {
	mode_select_t currentMode;
	HX_CIS_ERROR_E ret;
	uint16_t sleepCount;

	ret = hx_drv_cis_get_reg(MODE_SELECT , &currentMode);
	if (ret != HX_CIS_NO_ERROR) {
		return ret;
	}

	xprintf("  Changing mode from %d to %d with nFrames=%d and sleepTime=%d\r\n",
			currentMode, newMode, numFrames, sleepTime);

	// Disable before making changes
	ret = hx_drv_cis_set_reg(MODE_SELECT, MODE_SLEEP, 0);
	if (ret != HX_CIS_NO_ERROR) {
		return ret;
	}

	// Context control
	ret = hx_drv_cis_set_reg(PMU_CFG_3, context, 0);
	if (ret != HX_CIS_NO_ERROR) {
		return ret;
	}

	if (numFrames != 0) {
		// Applies to MODE_SW_NFRAMES_SLEEP, MODE_SW_NFRAMES_STANDBY and MODE_HW_NFRAMES_SLEEP
		// This is the number of frames to take continguously, after the sleep finishes
		// It is NOT the total number of frames
		ret = hx_drv_cis_set_reg(PMU_CFG_7, numFrames, 0);
		if (ret != HX_CIS_NO_ERROR) {
			return ret;
		}
	}

	if (sleepTime != 0) {
		// Applies to MODE_SW_NFRAMES_SLEEP and MODE_HW_NFRAMES_SLEEP
		// This is the period of time between groups of frames.
		// Convert this to regsiter values for PMU_CFG_8 and PMU_CFG_9
		sleepCount = calculateSleepTime(sleepTime);
		ret = hx_drv_cis_set_reg(PMU_CFG_8, (uint8_t) (sleepCount >> 8), 0);	// msb
		if (ret != HX_CIS_NO_ERROR) {
			return ret;
		}
		ret = hx_drv_cis_set_reg(PMU_CFG_9, (uint8_t) (sleepCount & 0xff), 0);	// lsb
		if (ret != HX_CIS_NO_ERROR) {
			return ret;
		}
	}

	if (currentMode == MODE_SW_CONTINUOUS) {
		// consider delaying to finish current image before changing mode
	}

	ret = hx_drv_cis_set_reg(MODE_SELECT, newMode, 0);

	return ret;
}

/**
 * Calculate values for the HM0360 sleep time registers.
 * This is the value used in Streaming 2 mode.
 *
 * Do this once per boot.
 * 0x0830 gives about 1s
 *
 * @param interval - in ms
 * @param value for HM0360 registers
 */
static uint16_t calculateSleepTime(uint32_t interval) {
	uint32_t sleepCount;

	sleepCount = interval * 0x8030 / 1000;

	// Make sure this does not exceed 16 bits
	if (sleepCount > 0xffff) {
		sleepCount = 0xffff;
	}

	xprintf("Interval of %dms gives sleep count = 0x%04x\n", interval, sleepCount);

	return (uint16_t) sleepCount;
}

/********************************** Public Function Definitions ***********************/

/**
 * Tests whether the HM0360 is present, by doing a read from the I2C address
 *
 * If the HM0360 is present the I2C read will work. Otherwise the driver code will
 * print an error and teh call will fail.
 *
 */
bool hm0360_md_present(void) {
	IIC_ERR_CODE_E ret;
	uint8_t rBuffer;

    //saveMainCameraConfig();
    //hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);

	/*    Usage-4: reads data from a specified I2C slave device using the I2C Master 0
	*      uint8_t rBuffer[2] = {0};
	*      uint8_t dataLen = 2;
	*/
	ret = hx_drv_i2cm_read_data(USE_DW_IIC_1, HM0360_SENSOR_I2CID, &rBuffer, 1);

	//restoreMainCameraConfig();

	if (ret == IIC_ERR_OK) {
		return true;
	}
	else {
		return false;
	}
}

void hm0360_md_init(bool sensor_init) {
	HX_CIS_ERROR_E ret;

    dbg_printf(DBG_LESS_INFO, "Initialising HM0360 at 0x%02x\r\n", HM0360_SENSOR_I2CID);

    sleepInterval = DPDINTERVAL;

    saveMainCameraConfig();
    hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);

    // Set HM0360 mode to SLEEP before initialisation
    ret = hm0360_sensor_set_mode(CONTEXT_A, MODE_SLEEP, 0, 0);

    if (ret != HX_CIS_NO_ERROR) {
    	dbg_printf(DBG_LESS_INFO, "HM0360 initialisation failed %d\r\n", ret);
    	restoreMainCameraConfig();
        return;
    }

	if (sensor_init == true) {
		// This is the long list of registers
		if(hx_drv_cis_setRegTable(HM0360_md_init_setting, HX_CIS_SIZE_N(HM0360_md_init_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR) {
			dbg_printf(DBG_LESS_INFO, "HM0360 Init fail \r\n");
			restoreMainCameraConfig();
			return;
		}
		else {
			dbg_printf(DBG_LESS_INFO, "HM0360 registers initialised\n");
		}
	}
	restoreMainCameraConfig();
}

/**
 * Read the HM0360 interrupt status register
 *
 * @param - pointer to byte to receive the status
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_get_int_status(uint8_t * val) {
	uint8_t currentStatus;
	HX_CIS_ERROR_E ret;

    saveMainCameraConfig();
    hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);

	ret = hx_drv_cis_get_reg(INT_INDIC , &currentStatus);
	if (ret == HX_CIS_NO_ERROR) {
		*val = currentStatus;
	}

	restoreMainCameraConfig();

	return ret;
}


/**
 * Clear the HM0360 interrupt bits
 *
 * @param - mask for the bits to clear
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_clear_interrupt(uint8_t val) {
	HX_CIS_ERROR_E ret;

    saveMainCameraConfig();
    hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);

	ret = hx_drv_cis_set_reg(INT_CLEAR, val, 0);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Called when the AI processor is about to enter DPD.
 *
 * Get the HM0360 ready to detect motion
 *
 *  Select CONTEXT_B registers
 */
HX_CIS_ERROR_E hm0360_md_prepare(void) {
	HX_CIS_ERROR_E ret;

    saveMainCameraConfig();
    hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);

	ret = hm0360_sensor_set_mode(CONTEXT_B, MODE_SW_NFRAMES_SLEEP, 1, sleepInterval);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Set the interval between frames in MD mode
 */
void hm0360_md_setFrameInterval(uint16_t interval) {
	sleepInterval = interval;
}

uint16_t hm0360_md_getFrameInterval(void) {
	return sleepInterval;

}
