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
#include "fatfs_task.h"


/*************************************** Defines **************************************/


/*************************************** Local Function Declarations ******************/

static void saveMainCameraConfig(void);
static void restoreMainCameraConfig(void);

static uint16_t calculateSleepTime(uint32_t interval);

/*************************************** Local variables ******************************/


// Somewhere to sore the I2C address of the main image sensor
static uint8_t mainCameraID;

static bool hm0360MainCamera = false;

static bool hm0360_present = false;

static HX_CIS_SensorSetting_t HM0360_md_init_setting[] = {
#include "../cis_hm0360/HM0360_OSC_Bayer_640x480_setA_VGA_setB_QVGA_md_8b_ParallelOutput_R2.i"
};

/*************************************** Local Function Definitions *******************/

/**
 * Read and save settings of the main camera before accessing the HM0360.
 *
 *	// TODO do we need some critical section or semaphore code here, in case we change task mid-stream?
 *
 * Configure the image sensor I2C address to be the HM0360.
 */
static void saveMainCameraConfig(void) {

	if (hm0360MainCamera) {
		// Don't waste time saving and restoring this
		return;
	}

	hx_drv_cis_get_slaveID(&mainCameraID);
    hx_drv_cis_set_slaveID(HM0360_SENSOR_I2CID);
}

/**
 * Restore settings of the main camera after accessing the HM0360
 */
static void restoreMainCameraConfig(void) {
	if (hm0360MainCamera) {
		// Don't waste time saving and restoring this
		return;
	}
	hx_drv_cis_set_slaveID(mainCameraID);
}


/**
 * Calculate values for the HM0360 sleep time registers.
 * This is the value used in Streaming 2 mode.
 *
 * Do this once per boot.
 * 0x0830 gives about 1s
 *
 * Should I check for a minimum value?
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

	xprintf("   Interval of %dms gives sleep count = 0x%04x\n", interval, sleepCount);

	return (uint16_t) sleepCount;
}

/********************************** Public Function Definitions ***********************/

/**
 * Tests whether _any_ sensor is present, by doing a read from the I2C address
 *
 * Side effect: if the sensor address is for the HM0360 then the variable
 * 'hm0360_present' will also be updated.
 *
 * If the sensor is present at the sensorAddress the I2C read will work.
 * Otherwise the driver code will print an error and the call will fail.
 *
 *@param sensorAddress = I2C address of target image sensor
 *@param @return = true if present
 */
bool hm0360_md_isSensorPresent(uint8_t sensorAddress) {
	IIC_ERR_CODE_E ret;
	uint8_t rBuffer;
	bool sensorPresent = false;

	/*    Usage-4: reads data from a specified I2C slave device using the I2C Master 0
	*      uint8_t rBuffer[2] = {0};
	*      uint8_t dataLen = 2;
	*/
	ret = hx_drv_i2cm_read_data(USE_DW_IIC_1, sensorAddress, &rBuffer, 1);

	sensorPresent = (ret == IIC_ERR_OK);

	if (sensorPresent  && (sensorAddress == HM0360_SENSOR_I2CID))  {
		hm0360_present = true;
	}

	return sensorPresent ;
}

/**
 * Returns whether the HM0360 is present or not
 *
 * NOTE: It is important to call hm0360_md_isSensorPresent(HM0360_SENSOR_I2CID) first
 *
 * @return true if present
 */
bool hm0360_md_isHM0360Present(void) {
	return hm0360_present;
}

/**
 * Explicitly set hm0360MainCamera
 *
 * @param hm0360IsMainCamera = true of the HM0360 is the main camera
 */
void hm0360_md_setIsMainCamera(bool hm0360IsMainCamera) {
	hm0360MainCamera = hm0360IsMainCamera;
}

/**
 * Checks if the HM0360 is the main camera
 *
 * @return true of the HM0360 is the main camera
 */
bool hm0360_md_getIsMainCamera(void) {
	return hm0360MainCamera;
}

/**
 * Initialises the HM0360 if it is used for motion detection while using a RP camera.
 * Called only at cold boot as the HM0360 retains its regsiter settings during DPD.
 *
 * NOTE: It is important to call hm0360_md_isSensorPresent(HM0360_SENSOR_I2CID) first
 * otherwise hm0360_present will be invalid (false).
 *
 * @param coldBoot - true if it is a cold boot.
 */
void hm0360_md_init(void) {
	HX_CIS_ERROR_E ret;

	dbg_printf(DBG_LESS_INFO, "Initialising HM0360 at 0x%02x for MD only.\r\n", HM0360_SENSOR_I2CID);

	saveMainCameraConfig();

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		dbg_printf(DBG_LESS_INFO, "  HM0360 not present\n");
		restoreMainCameraConfig();
		return ;
	}

	// Set HM0360 mode to SLEEP before initialisation
	ret = hm0360_md_setMode(CONTEXT_A, MODE_SLEEP, 0, 0);

	if (ret != HX_CIS_NO_ERROR) {
		dbg_printf(DBG_LESS_INFO, "HM0360 initialisation failed. Error %d\r\n", ret);
		hm0360_present = false;
		restoreMainCameraConfig();
		return;
	}

	// Only at cold boot do we need to initialise all of the registers.
	// This is the long list...
	if(hx_drv_cis_setRegTable(HM0360_md_init_setting, HX_CIS_SIZE_N(HM0360_md_init_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR) {
		dbg_printf(DBG_LESS_INFO, "HM0360 Init fail \r\n");
		hm0360_present = false;
		restoreMainCameraConfig();
		return;
	}
	else {
		dbg_printf(DBG_LESS_INFO, "HM0360 registers initialised for MD.\n");
	}

	restoreMainCameraConfig();
}


/*
 * Change HM0360 operating mode
 *
 * Some extra code added to disable MD (dy disabling the MD interrupt).
 * See notes below as how I chose that way rather than using MODE_SLEEP.
 *
 * @param context - bits to write to the context control register (PMU_CFG_3, 0x3024)
 * @param mode - one of 8 modes of MODE_SELECT register
 * @param numFrames - the number of frames to capture before sleeping
 * @param sleepTime - the time (in ms) to sleep before waking again (0 = inhibit)
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_setMode(uint8_t context, mode_select_t newMode,
		uint8_t numFrames, uint16_t sleepTime) {
	mode_select_t currentMode;
	HX_CIS_ERROR_E ret;
	uint16_t sleepCount;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

    saveMainCameraConfig();
//
//	I wanted to allow for no MD, so I thought id sleepTime == 0 then switch to MODE_SLEEP
//	However it results in c. 700uA current vs c.270uA for MODE_SW_NFRAMES_SLEEP

//	mode_select_t newMode;
//	if ((mode == MODE_SW_NFRAMES_SLEEP) && (sleepTime == 0)) {
//		// This means inhibit MD, so override mode
//		//newMode = MODE_SLEEP;		// 700uA
//		//newMode = MODE_HW_TRIGGER;	// 520uA
//		//newMode = MODE_HW_NFRAMES_SLEEP;	// 520uA
//		newMode = MODE_SW_NFRAMES_SLEEP;	// 270uA - i.e. stay in MODE_SW_NFRAMES_SLEEP
//
//		// Plan B - disable MD instead
//		// TODO clean this up!
//		ret = hx_drv_cis_set_reg(MD_CTRL_B, 0, 0);
//		if (ret != HX_CIS_NO_ERROR) {
//		    restoreMainCameraConfig();
//			return ret;
//		}
//	}
//	else {
//		newMode = mode;
//	}


	ret = hx_drv_cis_get_reg(MODE_SELECT, &currentMode);
	if (ret != HX_CIS_NO_ERROR) {
	    restoreMainCameraConfig();
		return ret;
	}

	// Revised way of dealing with inhibiting MD:
	// Stay in MODE_SW_NFRAMES_SLEEP, set slowest possible frame rate, inhibit MD interrupts
	if (sleepTime == 0) {
		sleepCount = 0xffff;	// maximum value = slowest rate
		xprintf("   Inhibiting MD\n");

		// Disable MD interrupt
		hm0360_md_disableInterrupt();
	}
	else {
		// Applies to MODE_SW_NFRAMES_SLEEP and MODE_HW_NFRAMES_SLEEP
		// This is the period of time between groups of frames.
		// Convert this to register values for PMU_CFG_8 and PMU_CFG_9
		sleepCount = calculateSleepTime(sleepTime);

		// Enable MD interrupt
		hm0360_md_enableInterrupt();
	}

	xprintf("   Changing mode from %d to %d with nFrames=%d, sleepTime=%d sleepCount = 0x%04x\r\n",
			currentMode, newMode, numFrames, sleepTime, sleepCount);

	// Disable before making changes by going to MODE_SLEEP
	ret = hx_drv_cis_set_reg(MODE_SELECT, MODE_SLEEP, 0);
	if (ret != HX_CIS_NO_ERROR) {
	    restoreMainCameraConfig();
		return ret;
	}

	// Context control (CONTEXT_A or CONTEXT_B)
	ret = hx_drv_cis_set_reg(PMU_CFG_3, context, 0);
	if (ret != HX_CIS_NO_ERROR) {
	    restoreMainCameraConfig();
		return ret;
	}

	if (numFrames != 0) {
		// Applies to MODE_SW_NFRAMES_SLEEP, MODE_SW_NFRAMES_STANDBY and MODE_HW_NFRAMES_SLEEP
		// This is the number of frames to take continguously, after the sleep finishes
		// It is NOT the total number of frames
		ret = hx_drv_cis_set_reg(PMU_CFG_7, numFrames, 0);
		if (ret != HX_CIS_NO_ERROR) {
		    restoreMainCameraConfig();
			return ret;
		}
	}

	// Write the sleep count to determine the frame rate
	ret = hx_drv_cis_set_reg(PMU_CFG_8, (uint8_t) (sleepCount >> 8), 0);	// msb
	if (ret != HX_CIS_NO_ERROR) {
		restoreMainCameraConfig();
		return ret;
	}
	ret = hx_drv_cis_set_reg(PMU_CFG_9, (uint8_t) (sleepCount & 0xff), 0);	// lsb
	if (ret != HX_CIS_NO_ERROR) {
		restoreMainCameraConfig();
		return ret;
	}

	if (currentMode == MODE_SW_CONTINUOUS) {
		// consider delaying to finish current image before changing mode
	}

	ret = hx_drv_cis_set_reg(MODE_SELECT, newMode, 0);

    restoreMainCameraConfig();

	return ret;
}

/**
 * Read the HM0360 interrupt status register
 *
 * @param - pointer to byte to receive the status
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_getInterruptStatus(uint8_t * val) {
	uint8_t currentStatus;
	HX_CIS_ERROR_E ret;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

    saveMainCameraConfig();

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
HX_CIS_ERROR_E hm0360_md_clearInterrupt(uint8_t val) {
	HX_CIS_ERROR_E ret;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

    saveMainCameraConfig();

	ret = hx_drv_cis_set_reg(INT_CLEAR, val, 0);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Enable the MD interrupt.
 *
 * Do this just before entering DPD
 */
HX_CIS_ERROR_E hm0360_md_enableInterrupt(void) {
	HX_CIS_ERROR_E ret;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

	saveMainCameraConfig();

	ret = hx_drv_cis_set_reg(MD_CTRL1, 0x06, 0);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Inhibit the MD interrupt.
 *
 * Otherwise the MD interrupt signal continues to be asserted while the
 * device is awake taking a sequence of photos
 */
HX_CIS_ERROR_E hm0360_md_disableInterrupt(void) {
	HX_CIS_ERROR_E ret;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

	saveMainCameraConfig();

	ret = hx_drv_cis_set_reg(MD_CTRL1, 0, 0);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Reads status registers related to Automatic Exposure

 * @param val - pointer to a structure that accepts the results
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_getGainRegs(HM0360_GAIN_T * val) {
	HX_CIS_ERROR_E ret;
	uint8_t valueH;
	uint8_t valueL;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

    saveMainCameraConfig();

    // Integration registers 0x0202 & 0x0203 "course integration time in lines"
    // See data sheet section 9.3. Relates to "exposure time"
	ret = hx_drv_cis_get_reg(INTEGRATION_H, &valueH);
	ret |= hx_drv_cis_get_reg(INTEGRATION_L, &valueL);
	val->integration = (valueH << 8) + valueL;

	// Analog gain register 0x0205 (bits 4-6 only, hence the shift)
	ret |= hx_drv_cis_get_reg(ANALOG_GAIN, &valueL);
	val->analogGain = (valueL >> 4) & 0x07; // (bits 4-6 only, hence the shift)

	// Digital gain registers 0x020E & 0x020F (seems to be [1-0] from 020e and [7-2] from 020f
	ret |= hx_drv_cis_get_reg(DIGITAL_GAIN_H, &valueH);
	ret |= hx_drv_cis_get_reg(DIGITAL_GAIN_L, &valueL);
	val->digitalGain = ((valueH & 0x03) << 6) + ((valueL & 0xfa) >> 6);

	// AE Mean register 0x205d (RO)
	ret |= hx_drv_cis_get_reg(AE_MEAN, &valueL);
	val->aeMean = valueL;

	// AE converged register 0x2060 (RO) - bit 0 only
	ret |= hx_drv_cis_get_reg(AE_CONVERGE, &valueL);
	val->aeConverged = valueL;

	restoreMainCameraConfig();
	return ret;
}

/**
 * Reads the Motion Detection ROI registers
 *
 * There are 32 ROI_OUT registers 20A1-20C0 - see HM0360 data sheet section 4.1
 *
 * @param regTable - address of teh bufferto hold the data
 * @param length - number of bytes to read (must be 32)
 * @return number of blocks with motion detected
 */
uint16_t hm0360_md_getMDOutput(uint8_t * regTable, uint8_t length) {
	uint8_t val;
	uint16_t motionCount = 0;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return 0;
	}

	if (length != ROIOUTENTRIES) {
		return 0;
	}

	saveMainCameraConfig();

	for (uint8_t i=0; i < ROIOUTENTRIES; i++) {
		hx_drv_cis_get_reg(MD_ROI_OUT_0 + i, &val);
		regTable[i] = val;

        // Count bits set in this register
        while (val) {
            motionCount += (val & 1);
            val >>= 1;
        }
	}

	restoreMainCameraConfig();

	return motionCount;
}

/**
 * Format a string that can print a 16x16 array showing where motion bits are detected
 *
 * @param regTable - address of the buffer that holds the ROI register data
 * @param msg - address of the character array (string)
 * @param length - length of that array
 */
void hm0360_md_printGrid(uint8_t *roiOut, uint16_t numBlocks, char *msg, uint16_t msgLen) {
	uint16_t offset = 0;

	for (uint16_t block = 0; block < numBlocks; block++) {

		uint8_t reg = block >> 3;      // block / 8
		uint8_t bit = block & 7;       // block % 8

		char c = (roiOut[reg] & (1 << bit)) ? '#' : '.';

		if (offset < (msgLen - 2)) {
			msg[offset++] = c;
		}

		if ((block & 0x0F) == 15) {    // every 16 cells
			if (offset < (msgLen - 2)) {
				msg[offset++] = '\n';
			}
		}
	}

	msg[offset] = '\0';
}
/**
 * Sets the HM0360 STROBE_CFG register
 *
 * This controls the STROBE pin which activates the flash.
 * See data sheet 10.23
 *
 * The default register settings file leaves the strobe disabled:
 *
		{HX_CIS_I2C_Action_W, 0x3080, 0x00},	// STROBE_CFG disable
		or:
		{HX_CIS_I2C_Action_W, 0x3080, 0x03},	// STROBE_CFG Modes: Static=5; Dynamic 1=3; Dynamic 2=B; Multiple = 13

 * Bit 0 - 1 enables, 0 disables
 *
 * @param - value written to the register
 * @return error code
 */
HX_CIS_ERROR_E hm0360_md_configureStrobe(uint8_t val) {
	HX_CIS_ERROR_E ret;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

    saveMainCameraConfig();

    ret = hx_drv_cis_set_reg(STROBE_CFG, val, 0);

    restoreMainCameraConfig();

	return ret;

}


/**
 * Called when the AI processor is about to enter DPD.
 *
 * Get the HM0360 ready to detect motion
 *
 *  Select CONTEXT_B registers
 *
 *  @param cameraSystemEnabled - true if OP_PARAMETER_CAMERA_ENABLED is set
 */
HX_CIS_ERROR_E hm0360_md_prepare(bool cameraSystemEnabled, uint16_t mdFrameInterval) {
	HX_CIS_ERROR_E ret;
	uint16_t mdInterval;

	// Don't proceed if the HM0360 is missing or faulty
	if (!hm0360_present) {
		return HX_CIS_UNKNOWN_ERROR;
	}

	if (cameraSystemEnabled) {
		mdInterval = mdFrameInterval;
	}
	else {
		mdInterval = 0;
	}

	if (mdInterval > 0) {
		dbg_printf(DBG_LESS_INFO, "   HM0360 Motion Detection on! %dms frame interval\r\n", mdFrameInterval);
	}
	else {
		dbg_printf(DBG_LESS_INFO, "   HM0360 Motion Detection off.\r\n");
	}

	saveMainCameraConfig();

	// This writes to register 0x2065
	// MD interrupt will be enabled in hm0360_md_setMode() if MD is enabled
	hm0360_md_clearInterrupt(0xff);		// clear all bits

	// Don't use MODE_SLEEP even if camera system is disabled, as it draws more power!
	ret = hm0360_md_setMode(CONTEXT_B, MODE_SW_NFRAMES_SLEEP, 1, mdInterval);

	restoreMainCameraConfig();

	return ret;
}

/**
 * Re-program the HM0360 with the long register list
 */
HX_CIS_ERROR_E hm0360_md_reInitialise(void) {
	HX_CIS_ERROR_E ret;

	saveMainCameraConfig();

	if(hx_drv_cis_setRegTable(HM0360_md_init_setting, HX_CIS_SIZE_N(HM0360_md_init_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR) {
		dbg_printf(DBG_LESS_INFO, "HM0360 Reinit fail \r\n");
		hm0360_present = false;
		ret = HX_CIS_UNKNOWN_ERROR;
	}
	else {
		dbg_printf(DBG_LESS_INFO, "HM0360 registers reinitialised\n");
		ret = HX_CIS_NO_ERROR;
	}

	restoreMainCameraConfig();

	return ret;
}
