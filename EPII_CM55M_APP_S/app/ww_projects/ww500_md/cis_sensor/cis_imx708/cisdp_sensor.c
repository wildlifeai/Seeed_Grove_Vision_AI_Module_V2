/*
 * cisdp_sensor.c
 *
 *  Created on: 20240122
 *      Author: 901912
 */

#include "cisdp_sensor.h"

#include "cisdp_cfg.h"
#include "WE2_debug.h"
#include "hx_drv_CIS_common.h"
#include "hx_drv_timer.h"
#include "hx_drv_hxautoi2c_mst.h"
#include "hx_drv_CIS_common.h"

#include "WE2_core.h"
#include "hx_drv_swreg_aon.h"
#include "hx_drv_scu_export.h"
#include "driver_interface.h"
#include "hx_drv_scu.h"
#include "math.h"

// FreeRTOS kernel includes.
#include "FreeRTOS.h"
#include "timers.h"
#include "task.h"

// Frame-timeout forensics (cisdp_dump_diag)
#include "hx_drv_csirx.h"
#include "hx_drv_inp.h"

//#define GROVE_VISION_AI

#ifdef TRUSTZONE_SEC
#ifdef IP_INST_NS_csirx
#define	CSIRX_REGS_BASE 				BASE_ADDR_MIPI_RX_CTRL
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY+0x48)
#else
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL_ALIAS
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x48)
#endif
#else
#ifndef TRUSTZONE
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL_ALIAS
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY_ALIAS+0x48)
#else
#define CSIRX_REGS_BASE             	BASE_ADDR_MIPI_RX_CTRL
#define CSIRX_DPHY_REG					(BASE_ADDR_APB_MIPI_RX_PHY+0x50)
#define CSIRX_DPHY_TUNCATE_REG			(BASE_ADDR_APB_MIPI_RX_PHY+0x48)
#endif
#endif

// CGP - the calculation below seems to be for a 'x10 Compress' but
//#define JPEG_BUFSIZE  (((623+ (IMX708_HW5x5_CROP_WIDTH/16)*(IMX708_HW5x5_CROP_HEIGHT/16)* 38 + 35) >>2 ) <<2)	//YUV420 x10 Compress = ((623+ (W/16)*(H/16)* 38 + 35) >>2 ) <<2  byte
__attribute__(( section(".bss.NoInit"))) uint8_t jpegbuf[JPEG_BUFSIZE] __ALIGNED(32);

//#define RAW_BUFSIZE  (IMX708_HW5x5_CROP_WIDTH*IMX708_HW5x5_CROP_HEIGHT*3/2)   //YUV420: Y= W*H byte, U = ((W*H)>>2) byte, V = ((W*H)>>2) byte
__attribute__(( section(".bss.NoInit"))) uint8_t demosbuf[RAW_BUFSIZE] __ALIGNED(32);

#define JPEG_HEADER_BUFSIZE 100
__attribute__(( section(".bss.NoInit"))) uint8_t jpegfilesizebuf[JPEG_HEADER_BUFSIZE] __ALIGNED(32);

static volatile uint32_t g_wdma1_baseaddr = (uint32_t)jpegbuf; // = (uint32_t)cdmbuf; // - no use
static volatile uint32_t g_wdma2_baseaddr = (uint32_t)jpegbuf;
static volatile uint32_t g_wdma3_baseaddr = (uint32_t)demosbuf;
static volatile uint32_t g_jpegautofill_addr = (uint32_t)jpegfilesizebuf;

static HX_CIS_SensorSetting_t IMX708_common_setting[] = {
#include "IMX708_common_setting.i"
};

static HX_CIS_SensorSetting_t IMX708_2304x1296_setting[] = {
#include "IMX708_mipi_2lane_2304x1296.i"
};

static HX_CIS_SensorSetting_t IMX708_stream_on[] = {
		{HX_CIS_I2C_Action_W, 0x0100, 0x01},
};

static HX_CIS_SensorSetting_t IMX708_stream_off[] = {
	    {HX_CIS_I2C_Action_W, 0x0100, 0x00},
};

/* 450MHz is the nominal "default" link frequency */
static HX_CIS_SensorSetting_t IMX708_link_450Mhz_regs[] = {
        { HX_CIS_I2C_Action_W, 0x030E, 0x01},
        { HX_CIS_I2C_Action_W, 0x030F, 0x2c},
};

static HX_CIS_SensorSetting_t  IMX708_exposure_setting[] = {
		{HX_CIS_I2C_Action_W, 0x0202, ((IMX708_EXPOSURE_SETTING>>8)&0xFF)},
		{HX_CIS_I2C_Action_W, 0x0203, (IMX708_EXPOSURE_SETTING&0xFF)},
};

static const uint8_t pdaf_gains[2][9] = {
        { 0x4c, 0x4c, 0x4c, 0x46, 0x3e, 0x38, 0x35, 0x35, 0x35 },
        { 0x35, 0x35, 0x35, 0x38, 0x3e, 0x46, 0x4c, 0x4c, 0x4c }
};

static HX_CIS_SensorSetting_t  IMX708_mirror_setting[] = {
		{HX_CIS_I2C_Action_W, 0x0101, (CIS_MIRROR_SETTING&0xFF)},
};

// High-resolution RAW capture override (see hires.c): when nonzero, the
// datapath is configured as sensor-windowed RAW -> WDMA2 at this address
// instead of the integrated HW5x5+JPEG flow.
#define HIRES_RAW_WIDTH		1280
#define HIRES_RAW_HEIGHT	960
static uint32_t g_hires_raw_addr = 0;

// Hi-res sensor windowing selector. 0 = the sensor streams its stock
// 2304x1296 and the INP does the 1280x960 centre crop (subsample disabled);
// 1 = the sensor itself streams a 1280-wide window via the digital-crop
// stage and the INP is a pure pass-through.
//
// Bench 12 Jul 2026, with the CIS slave-ID bug fixed (see
// cisdp_stream_on()): INP crop mode (0) delivers frames but every one ends
// in XDMA_WDMA2STATUS_ERR_FE_COUNT_NOT_REACH - the INP under-delivers a
// 1280-wide crop, confirming the DS 5.6.3 cap (INP processed output max
// 640x640). The sensor window (1) is therefore required.
//
// The window is TALLER than the image (HIRES_SENSOR_WINDOW_HEIGHT vs
// HIRES_RAW_HEIGHT): the capture gate opens a fixed ~18.4 lines after
// frame start (measured constant to 32 bytes across boots), so a 960-line
// window only ever delivered ~941.6 lines and the WDMA2 byte count never
// completed. With 992 sensor lines the gate window contains ~973 lines
// and the DMA reaches its 1280x960 count mid-frame -> NORMAL_FINISH.
#define HIRES_SENSOR_WINDOW		1

#if HIRES_SENSOR_WINDOW
// Sensor streams 992 lines; the gated capture keeps 960 (see above).
// Offsets (512,152) are even, preserving the Bayer phase; the captured
// content starts ~18 lines in (sensor row ~170), matching the VGA INP
// crop's framing (row 168) almost exactly.
#define HIRES_SENSOR_WINDOW_HEIGHT	992
static HX_CIS_SensorSetting_t IMX708_hires_window_setting[] = {
		{ HX_CIS_I2C_Action_W, 0x0408, 0x02},	// digital crop X offset = 512
		{ HX_CIS_I2C_Action_W, 0x0409, 0x00},
		{ HX_CIS_I2C_Action_W, 0x040A, 0x00},	// digital crop Y offset = 152
		{ HX_CIS_I2C_Action_W, 0x040B, 0x98},
		{ HX_CIS_I2C_Action_W, 0x040C, 0x05},	// digital crop width = 1280
		{ HX_CIS_I2C_Action_W, 0x040D, 0x00},
		{ HX_CIS_I2C_Action_W, 0x040E, 0x03},	// digital crop height = 992
		{ HX_CIS_I2C_Action_W, 0x040F, 0xE0},
		{ HX_CIS_I2C_Action_W, 0x034C, 0x05},	// X output size = 1280
		{ HX_CIS_I2C_Action_W, 0x034D, 0x00},
		{ HX_CIS_I2C_Action_W, 0x034E, 0x03},	// Y output size = 992
		{ HX_CIS_I2C_Action_W, 0x034F, 0xE0},
};
#endif // HIRES_SENSOR_WINDOW

void cisdp_set_hires_raw(uint32_t rawAddr)
{
	g_hires_raw_addr = rawAddr;
}

// Point the shared CIS I2C at the main camera. Callers that write sensor
// registers outside this driver (e.g. the AE loop) must call this first -
// the HM0360 MD companion shares the bus and can leave the slave ID
// pointing at itself (see cisdp_stream_on()).
void cisdp_select_main_camera_i2c(void)
{
	hx_drv_cis_set_slaveID(CIS_I2C_ID);
}

// Frame-timeout forensics (called from the image task's capture-retry path,
// while the sensor rail and datapath are still up). One compact dump that
// splits the fault between the sensor (FRM_CNT movement), the MIPI link
// (CSI RX IRQ/lane state) and the INP configuration.
void cisdp_dump_diag(void)
{
	static const uint16_t regs[] = {
		0x0100, 0x0005, 0x0340, 0x0341, 0x0342, 0x0343,
		0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0x040D, 0x040E, 0x040F,
		0x034C, 0x034D, 0x034E, 0x034F,
	};
	uint8_t v8;
	unsigned i;

	// The HM0360 MD sensor shares this I2C bus and its polling leaves the
	// slave ID pointing at itself - explicitly select the IMX708 (bench
	// 11 Jul: a dump without this read the HM0360's register space)
	uint8_t prevId = 0;
	hx_drv_cis_get_slaveID(&prevId);
	hx_drv_cis_set_slaveID(CIS_I2C_ID);

	xprintf("DIAG sensor(id 0x%02x, was 0x%02x):", CIS_I2C_ID, prevId);
	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		if (hx_drv_cis_get_reg(regs[i], &v8) == HX_CIS_NO_ERROR) {
			xprintf(" %04x=%02x", regs[i], v8);
		}
		else {
			xprintf(" %04x=ERR", regs[i]);
		}
	}
	xprintf("\n");

	uint8_t f1 = 0xEE, f2 = 0xEE;
	hx_drv_cis_get_reg(0x0005, &f1);
	vTaskDelay(pdMS_TO_TICKS(250));
	hx_drv_cis_get_reg(0x0005, &f2);
	xprintf("DIAG FRM_CNT 0x%02x -> 0x%02x over 250ms (%s)\n", f1, f2,
			(f1 != f2) ? "sensor STREAMING" : "sensor NOT advancing");

	uint32_t info = 0, err = 0, dphy = 0;
	uint16_t fifo = 0;
	uint8_t dpp = 0;
	hx_drv_csirx_get_infoirq_state(&info);
	hx_drv_csirx_get_errirq_state(&err);
	hx_drv_csirx_get_dphyerrirq_state(&dphy);
	hx_drv_csirx_get_fifo_fill(&fifo);
	hx_drv_csirx_get_pixel_depth(&dpp);
	xprintf("DIAG csirx info=0x%08x err=0x%08x dphy=0x%08x stop(clk,l0,l1)=%d,%d,%d fifo=%d dpp=%d\n",
			(unsigned)info, (unsigned)err, (unsigned)dphy,
			(int)hx_drv_csirx_get_clkln_stopstate(),
			(int)hx_drv_csirx_get_ln0_stopstate(),
			(int)hx_drv_csirx_get_ln1_stopstate(),
			(int)fifo, (int)dpp);

	uint8_t inp_en = 0xEE;
	INP_SUBSAMPLE_E sub = INP_SUBSAMPLE_DISABLE;
	uint16_t hsize = 0;
	hx_drv_inp_get_enable(&inp_en);
	hx_drv_inp_get_subsample(&sub);
	hx_drv_inp_get_rxsub_hsize(&hsize);
	xprintf("DIAG inp en=%d sub=0x%x hsize=%d hires_addr=0x%08x\n",
			(int)inp_en, (unsigned)sub, (int)hsize, (unsigned)g_hires_raw_addr);

	// (The WDMA2 delivery high-water mark is printed per retry by
	// hires_dump_hwm_and_refill() - see the image task's retry path.)

	hx_drv_cis_set_slaveID(prevId);
}

uint8_t cisdp_get_demos_pattern(void)
{
	// Order matches demosaic_pattern_t (demosaic.h). DP_HW5X5_DEMOS_PATTERN
	// expands to an enum constant, so this folds at compile time.
	switch (DP_HW5X5_DEMOS_PATTERN) {
	case DEMOS_PATTENMODE_RGGB: return 0;
	case DEMOS_PATTENMODE_GRBG: return 1;
	case DEMOS_PATTENMODE_GBRG: return 2;
	default:                    return 3;	// BGGR
	}
}

static void cisdp_wdma_addr_init(APP_DP_INP_SUBSAMPLE_E subs)
{
	if (g_hires_raw_addr != 0) {
		// WDMA2 carries the RAW Bayer frame for the CPU pipeline
		sensordplib_set_xDMA_baseaddrbyapp(g_wdma1_baseaddr, g_hires_raw_addr, g_wdma3_baseaddr);
	}
	else {
		sensordplib_set_xDMA_baseaddrbyapp(g_wdma1_baseaddr, g_wdma2_baseaddr, g_wdma3_baseaddr);
	}
    sensordplib_set_jpegfilesize_addrbyapp(g_jpegautofill_addr);

	xprintf("WD1[%x], WD2_J[%x], WD3_RAW[%x], JPAuto[%x]\n",g_wdma1_baseaddr,
			(g_hires_raw_addr != 0) ? g_hires_raw_addr : g_wdma2_baseaddr,
			g_wdma3_baseaddr, g_jpegautofill_addr);
}

void IMX708_set_pll200()
{
    SCU_PDHSC_DPCLK_CFG_T cfg;

	hx_drv_scu_get_pdhsc_dpclk_cfg(&cfg);

	uint32_t pllfreq;
	hx_drv_swreg_aon_get_pllfreq(&pllfreq);

	if(pllfreq == 400000000)
	{
		cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
		cfg.mipiclk.hscmipiclkdiv = 1;
	}
	else
	{
		cfg.mipiclk.hscmipiclksrc = SCU_HSCMIPICLKSRC_PLL;
		cfg.mipiclk.hscmipiclkdiv = 0;
	}

    cfg.dpclk = SCU_HSCDPCLKSRC_RC96M48M;
	hx_drv_scu_set_pdhsc_dpclk_cfg(cfg, 0, 1);

	uint32_t mipi_pixel_clk = 96;
	hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK, &mipi_pixel_clk);
	mipi_pixel_clk = mipi_pixel_clk / 1000000;

    dbg_printf(DBG_LESS_INFO, "MIPI CLK change to PLL freq:(%d / %d)\n", pllfreq, (cfg.mipiclk.hscmipiclkdiv+1));
	dbg_printf(DBG_LESS_INFO, "MIPI TX CLK: %dM\n", mipi_pixel_clk);
}


void set_mipi_csirx_enable()
{
	uint32_t bitrate_1lane = IMX708_MIPI_CLOCK_FEQ*2;
	uint32_t mipi_lnno = IMX708_MIPI_LANE_CNT;
	uint32_t pixel_dpp = IMX708_MIPI_DPP;
#if HIRES_SENSOR_WINDOW
	// Hi-res: the sensor's digital crop narrows the stream on the wire, so
	// the RX FIFO fill and CSI TX are computed from the windowed geometry
	uint32_t line_length = (g_hires_raw_addr != 0) ? HIRES_RAW_WIDTH : IMX708_SENSOR_WIDTH;
	uint32_t frame_length = (g_hires_raw_addr != 0) ? HIRES_SENSOR_WINDOW_HEIGHT : IMX708_SENSOR_HEIGHT;
#else
	// The sensor always streams its stock 2304x1296 (hi-res crops in the INP)
	uint32_t line_length = IMX708_SENSOR_WIDTH;
	uint32_t frame_length = IMX708_SENSOR_HEIGHT;
#endif
	uint32_t byte_clk = bitrate_1lane/8;
	uint32_t continuousout = IMX708_MIPITX_CNTCLK_EN;
	uint32_t deskew_en = 0;
	uint32_t mipi_pixel_clk = 96;

	IMX708_set_pll200();

	hx_drv_scu_get_freq(SCU_CLK_FREQ_TYPE_HSC_MIPI_RXCLK, &mipi_pixel_clk);
	mipi_pixel_clk = mipi_pixel_clk / 1000000;

    uint32_t n_preload = 15;
	uint32_t l_header = 4;
	uint32_t l_footer = 2;

	double t_input = (double)(l_header+line_length*pixel_dpp/8+l_footer)/(mipi_lnno*byte_clk)+0.06;
	double t_output = (double)line_length/mipi_pixel_clk;
	double t_preload = (double)(7+(n_preload*4)/mipi_lnno)/mipi_pixel_clk;
	double delta_t = t_input - t_output - t_preload;

	dbg_printf(DBG_LESS_INFO, "t_input: %dns\n", (uint32_t)(t_input*1000));
	dbg_printf(DBG_LESS_INFO, "t_output: %dns\n", (uint32_t)(t_output*1000));
	dbg_printf(DBG_LESS_INFO, "t_preload: %dns\n", (uint32_t)(t_preload*1000));

	uint16_t rx_fifo_fill = 0;
	uint16_t tx_fifo_fill = 0;

	if(delta_t <= 0)
	{
		delta_t = 0 - delta_t;
		tx_fifo_fill = ceil(delta_t*byte_clk*mipi_lnno/4/(pixel_dpp/2))*(pixel_dpp/2);
		rx_fifo_fill = 0;
	}
	else
	{
		rx_fifo_fill = ceil(delta_t*byte_clk*mipi_lnno/4/(pixel_dpp/2))*(pixel_dpp/2);
		tx_fifo_fill = 0;
	}
#if HIRES_SENSOR_WINDOW
	if (g_hires_raw_addr != 0) {
		// Empirical (bench 12 Jul 2026, rawdump stride analysis): with the
		// computed fill (40) the INP drain outruns the shorter 1280-wide
		// RAW10 line burst and every stored line loses ~25.5 bytes
		// (stride 1254.5 instead of 1280 -> full-frame Bayer-phase tear).
		// A larger fill delays the per-line drain start so the burst stays
		// ahead; generous margin, still tiny versus the line period.
		if (rx_fifo_fill < 100) {
			rx_fifo_fill = 100;
		}
	}
#endif // HIRES_SENSOR_WINDOW
	dbg_printf(DBG_LESS_INFO, "MIPI RX FIFO FILL: %d\n", rx_fifo_fill);
	dbg_printf(DBG_LESS_INFO, "MIPI TX FIFO FILL: %d\n", tx_fifo_fill);

	/*
	 * Reset CSI RX/TX
	 */
	dbg_printf(DBG_LESS_INFO, "RESET MIPI CSI RX/TX\n");
	SCU_DP_SWRESET_T dp_swrst;
	drv_interface_get_dp_swreset(&dp_swrst);
	dp_swrst.HSC_MIPIRX = 0;
	dp_swrst.HSC_MIPITX = 0;

	hx_drv_scu_set_DP_SWReset(dp_swrst);
	hx_drv_timer_cm55x_delay_us(50, TIMER_STATE_DC);

	dp_swrst.HSC_MIPIRX = 1;
	dp_swrst.HSC_MIPITX = 1;
	hx_drv_scu_set_DP_SWReset(dp_swrst);

    MIPIRX_DPHYHSCNT_CFG_T hscnt_cfg;
    hscnt_cfg.mipirx_dphy_hscnt_clk_en = 0;
    hscnt_cfg.mipirx_dphy_hscnt_ln0_en = 1;
    hscnt_cfg.mipirx_dphy_hscnt_ln1_en = 1;

    if(mipi_pixel_clk == 200) //pll200
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x10;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x10;
    }
    else if(mipi_pixel_clk == 300) //pll300
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x18;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x18;
    }
    else //rc96
    {
		hscnt_cfg.mipirx_dphy_hscnt_clk_val = 0x03;
		hscnt_cfg.mipirx_dphy_hscnt_ln0_val = 0x06;
		hscnt_cfg.mipirx_dphy_hscnt_ln1_val = 0x06;
    }

    sensordplib_csirx_set_hscnt(hscnt_cfg);

    if(pixel_dpp == 10 || pixel_dpp == 8)
    {
    	sensordplib_csirx_set_pixel_depth(pixel_dpp);
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "PIXEL DEPTH fail %d\n", pixel_dpp);
        return;
    }

	sensordplib_csirx_set_deskew(deskew_en);
	sensordplib_csirx_set_fifo_fill(rx_fifo_fill);
    sensordplib_csirx_enable(mipi_lnno);

    CSITX_DPHYCLKMODE_E clkmode;
    if(continuousout)
    {
    	clkmode = CSITX_DPHYCLOCK_CONT;
    }
    else
    {
    	clkmode = CSITX_DPHYCLOCK_NON_CONT;
    }
    sensordplib_csitx_set_dphy_clkmode(clkmode);

    if(pixel_dpp == 10 || pixel_dpp == 8)
    {
    	sensordplib_csitx_set_pixel_depth(pixel_dpp);
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "PIXEL DEPTH fail %d\n", pixel_dpp);
        return;
    }

	sensordplib_csitx_set_deskew(deskew_en);
    sensordplib_csitx_set_fifo_fill(tx_fifo_fill);
    sensordplib_csitx_enable(mipi_lnno, bitrate_1lane, line_length, frame_length);

    /*
     * //VMUTE setting: Enable VMUTE
     * W:0x52001408:0x0000000D:4:4
     */
    SCU_VMUTE_CFG_T ctrl;
    ctrl.timingsrc = SCU_VMUTE_CTRL_TIMING_SRC_VMUTE;
    ctrl.txphypwr = SCU_VMUTE_CTRL_TXPHY_PWR_DISABLE;
    ctrl.ctrlsrc = SCU_VMUTE_CTRL_SRC_SW;
    ctrl.swctrl = SCU_VMUTE_CTRL_SW_ENABLE;
    hx_drv_scu_set_vmute(&ctrl);

// CGP reduce output
//    dbg_printf(DBG_LESS_INFO, "VMUTE: 0x%08X\n", *(uint32_t*)(SCU_LSC_ADDR+0x408));
//    dbg_printf(DBG_LESS_INFO, "0x53061000: 0x%08X\n", *(uint32_t*)(CSITX_REGS_BASE+0x1000));
//    dbg_printf(DBG_LESS_INFO, "0x53061004: 0x%08X\n", *(uint32_t*)(CSITX_REGS_BASE+0x1004));
//    dbg_printf(DBG_LESS_INFO, "0x53061008: 0x%08X\n", *(uint32_t*)(CSITX_REGS_BASE+0x1008));
//    dbg_printf(DBG_LESS_INFO, "0x5306100C: 0x%08X\n", *(uint32_t*)(CSITX_REGS_BASE+0x100C));
//    dbg_printf(DBG_LESS_INFO, "0x53061010: 0x%08X\n", *(uint32_t*)(CSITX_REGS_BASE+0x1010));
}


void set_mipi_csirx_disable()
{
    sensordplib_csirx_disable();
}


int cisdp_sensor_init(bool sensor_init) {
    dbg_printf(DBG_LESS_INFO, "Initialising IMX708 at 0x%02x (p.u. delay %dms)\r\n", CIS_I2C_ID, CIS_POWERUP_DELAY);
    dbg_printf(DBG_LESS_INFO, "Memory allocated: %ld for raw buffer, %d for JPEG, %d for JPEG header\n",
            			sizeof(demosbuf), sizeof(jpegbuf), sizeof(jpegfilesizebuf));

     hx_drv_cis_set_slaveID(CIS_I2C_ID);

    /*
     * common CIS init
     */
    hx_drv_cis_init((CIS_XHSHUTDOWN_INDEX_E)DEAULT_XHSUTDOWN_PIN, SENSORCTRL_MCLK_DIV3);
    dbg_printf(DBG_LESS_INFO, "mclk DIV3, xshutdown_pin=%d\n",DEAULT_XHSUTDOWN_PIN);

#if defined  (WW500)
#pragma message "WW500 in IMX708 driver"     // Need a delay here for the power to come on!
    vTaskDelay(pdMS_TO_TICKS(CIS_POWERUP_DELAY));

#elif defined(GROVE_VISION_AI)
	//IMX708 Enable
    hx_drv_gpio_set_output(AON_GPIO1, GPIO_OUT_LOW);
    hx_drv_scu_set_PA1_pinmux(SCU_PA1_PINMUX_AON_GPIO1, 1);
	hx_drv_gpio_set_out_value(AON_GPIO1, GPIO_OUT_LOW);
    hx_drv_timer_cm55x_delay_ms(10, TIMER_STATE_DC);
    hx_drv_gpio_set_out_value(AON_GPIO1, GPIO_OUT_HIGH);
    hx_drv_timer_cm55x_delay_ms(CIS_POWERUP_DELAY, TIMER_STATE_DC);
	dbg_printf(DBG_LESS_INFO, "Set PA1(AON_GPIO1) to High\n");
#else
    hx_drv_sensorctrl_set_xSleepCtrl(SENSORCTRL_XSLEEP_BY_CPU);
    hx_drv_sensorctrl_set_xSleep(0);
    hx_drv_timer_cm55x_delay_ms(100, TIMER_STATE_DC);
    hx_drv_sensorctrl_set_xSleep(1);
    dbg_printf(DBG_LESS_INFO, "hx_drv_sensorctrl_set_xSleep(1)\n");
    hx_drv_timer_cm55x_delay_ms(300, TIMER_STATE_DC);
#endif

    /*
     * off stream before init sensor
     */
    if(hx_drv_cis_setRegTable(IMX708_stream_off, HX_CIS_SIZE_N(IMX708_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 off by app fail\r\n");
        return -1;
    }

    if(hx_drv_cis_setRegTable(IMX708_common_setting, HX_CIS_SIZE_N(IMX708_common_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX708 Init by app fail (IMX708_common_setting)\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 Init by app (IMX708_common_setting)\n");
    }

    uint8_t val = 0;
    HX_CIS_ERROR_E ret = hx_drv_cis_get_reg(IMX708_REG_BASE_SPC_GAINS_L, &val);
    dbg_printf(DBG_LESS_INFO, "Get IMX708_REG_BASE_SPC_GAINS_L = 0x%02X\n", val);

	if (ret == HX_CIS_NO_ERROR && val == 0x40) {
	    dbg_printf(DBG_LESS_INFO, "Init IMX708 Default PDAF pixel correction gains \n");
		for (uint32_t i = 0; i < 54 && ret == 0; i++) {
			ret = hx_drv_cis_set_reg(IMX708_REG_BASE_SPC_GAINS_L + i, pdaf_gains[0][i % 9], 0);
		}
		for (uint32_t i = 0; i < 54 && ret == 0; i++) {
			ret = hx_drv_cis_set_reg(IMX708_REG_BASE_SPC_GAINS_R + i, pdaf_gains[1][i % 9], 0);
		}
	}

    if(hx_drv_cis_setRegTable(IMX708_2304x1296_setting, HX_CIS_SIZE_N(IMX708_2304x1296_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX708 Init by app fail (IMX708_2304x1296_setting)\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 Init by app (IMX708_2304x1296_setting)\n");
    }

#if HIRES_SENSOR_WINDOW
    if (g_hires_raw_addr != 0)
    {
    	// Hi-res capture (op32, hires.c): the sensor streams the 1280x960
    	// window itself. Staged before sensor init by image_task
    	// (hires_stage()).
    	if(hx_drv_cis_setRegTable(IMX708_hires_window_setting, HX_CIS_SIZE_N(IMX708_hires_window_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    	{
    		dbg_printf(DBG_LESS_INFO, "IMX708 hi-res window fail (IMX708_hires_window_setting)\n");
    		return -1;
    	}
    	dbg_printf(DBG_LESS_INFO, "IMX708 hi-res window: %dx%d out (digital crop @512,168)\n",
    			HIRES_RAW_WIDTH, HIRES_RAW_HEIGHT);
    }
#endif // HIRES_SENSOR_WINDOW

    if(hx_drv_cis_setRegTable(IMX708_exposure_setting, HX_CIS_SIZE_N(IMX708_exposure_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX708 Init by app fail (IMX708_exposure_setting)\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 EXPOSURE(0x%04X) by app (IMX708_exposure_setting)\n", IMX708_EXPOSURE_SETTING);
    }

    if(hx_drv_cis_setRegTable(IMX708_link_450Mhz_regs, HX_CIS_SIZE_N(IMX708_link_450Mhz_regs, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX708 Init by app fail (IMX708_link_450Mhz_regs)\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 Init by app (IMX708_link_450Mhz_regs)\n");
    }

	/* Quad Bayer re-mosaic adjustments (for full-resolution mode only) */
	if (IMX708_QBC_ADJUST > 0) {
		dbg_printf(DBG_LESS_INFO, "Quad Bayer re-mosaic adjustments ON\n");
		hx_drv_cis_set_reg(IMX708_LPF_INTENSITY, IMX708_QBC_ADJUST, 0);
		hx_drv_cis_set_reg(IMX708_LPF_INTENSITY_EN, IMX708_LPF_INTENSITY_ENABLED, 0);
	} else {
		dbg_printf(DBG_LESS_INFO, "Quad Bayer re-mosaic adjustments OFF\n");
		hx_drv_cis_set_reg(IMX708_LPF_INTENSITY_EN, IMX708_LPF_INTENSITY_DISABLED, 0);
	}

    //IMX708_set_mirror
    if(hx_drv_cis_setRegTable(IMX708_mirror_setting, HX_CIS_SIZE_N(IMX708_mirror_setting, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
        dbg_printf(DBG_LESS_INFO, "IMX708 Init by app fail (IMX708_mirror_setting)\n");
		return -1;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 Init by app (IMX708_mirror_setting)\n\n");
    }

    return 0;
}


int cisdp_dp_init(bool inp_init, SENSORDPLIB_PATH_E dp_type, sensordplib_CBEvent_t cb_event, uint32_t jpg_ratio, APP_DP_INP_SUBSAMPLE_E subs)
{
    HW2x2_CFG_T hw2x2_cfg;
    CDM_CFG_T cdm_cfg;
    HW5x5_CFG_T hw5x5_cfg;
    JPEG_CFG_T jpeg_cfg;

    //HW2x2 Cfg
    hw2x2_cfg.hw2x2_path = DP_HW2X2_PATH;
    hw2x2_cfg.hw_22_process_mode = DP_HW2X2_PROCESS_MODE;
    hw2x2_cfg.hw_22_crop_stx = DP_HW2X2_CROP_START_X;
    hw2x2_cfg.hw_22_crop_sty = DP_HW2X2_CROP_START_Y;
    hw2x2_cfg.hw_22_in_width = DP_HW2X2_CROP_WIDTH;
    hw2x2_cfg.hw_22_in_height = DP_HW2X2_CROP_HEIGHT;
    hw2x2_cfg.hw_22_mono_round_mode = DP_HW2X2_ROUND_MODE;

    //CDM Cfg
    cdm_cfg.cdm_enable = DP_CDM_ENABLE;
    cdm_cfg.cdm_crop_stx = DP_CDM_IN_START_X;
    cdm_cfg.cdm_crop_sty = DP_CDM_IN_START_Y;
    cdm_cfg.cdm_in_width = DP_CDM_IN_WIDTH;
    cdm_cfg.cdm_in_height = DP_CDM_IN_HEIGHT;
    cdm_cfg.meta_dump = DP_CDM_META_DUMP;
    cdm_cfg.ht_packing = DP_CDM_HT_PACKING;
    cdm_cfg.cdm_min_allow_dis = DP_CDM_MIN_ALLOW_DIS;
    cdm_cfg.cdm_tolerance = DP_CDM_TOLERANCE;
    cdm_cfg.cdm_reactance = DP_CDM_REACTANCE;
    cdm_cfg.cdm_relaxation = DP_CDM_RELAXATION;
    cdm_cfg.cdm_eros_th = DP_CDM_EROS_TH;
    cdm_cfg.cdm_num_ht_th = DP_CDM_NUM_HT_TH;
    cdm_cfg.cdm_num_ht_vect_th_x = DP_CDM_NUM_HT_VECT_TH_X;
    cdm_cfg.cdm_num_ht_vect_th_y = DP_CDM_NUM_HT_VECT_TH_X;
    cdm_cfg.cdm_num_cons_ht_bin_th_x = DP_CDM_NUM_CONS_HT_BIN_TH_X;
    cdm_cfg.cdm_num_cons_ht_bin_th_y = DP_CDM_NUM_CONS_HT_BIN_TH_Y;
    cdm_cfg.cpu_activeflag = DP_CDM_CPU_ACTIVEFLAG;
    cdm_cfg.init_map_flag = DP_CDM_INIT_MAP_FLAG;

    //HW5x5 Cfg
    hw5x5_cfg.hw5x5_path = DP_HW5X5_PATH;
    hw5x5_cfg.demos_bndmode = DP_HW5X5_DEMOS_BNDMODE;
    hw5x5_cfg.demos_color_mode = DP_HW5X5_DEMOS_COLORMODE;
    hw5x5_cfg.demos_pattern_mode = DP_HW5X5_DEMOS_PATTERN;
    hw5x5_cfg.demoslpf_roundmode = DP_HW5X5_DEMOSLPF_ROUNDMODE;
    hw5x5_cfg.hw55_crop_stx = DP_HW5X5_CROP_START_X;
    hw5x5_cfg.hw55_crop_sty = DP_HW5X5_CROP_START_X;
    hw5x5_cfg.hw55_in_width = DP_HW5X5_CROP_WIDTH;
    hw5x5_cfg.hw55_in_height = DP_HW5X5_CROP_HEIGHT;

    //JPEG Cfg
    jpeg_cfg.jpeg_path = DP_JPEG_PATH;
    jpeg_cfg.enc_width = DP_JPEG_ENC_WIDTH;
    jpeg_cfg.enc_height = DP_JPEG_ENC_HEIGHT;
    jpeg_cfg.jpeg_enctype = DP_JPEG_ENCTYPE;
    //jpeg_cfg.jpeg_encqtable = DP_JPEG_ENCQTABLE;

    if(jpg_ratio == 4) {
    	jpeg_cfg.jpeg_encqtable = JPEG_ENC_QTABLE_4X;
    }
    else {
    	jpeg_cfg.jpeg_encqtable = JPEG_ENC_QTABLE_10X;
    }

    cisdp_wdma_addr_init(subs);

    //setup MIPI RX
	set_mipi_csirx_enable();

    INP_CROP_T crop;

    if (g_hires_raw_addr != 0) {
#if HIRES_SENSOR_WINDOW
    	// High-resolution RAW capture (hires.c): the sensor streams the
    	// 1280-wide window itself (IMX708_hires_window_setting), and the INP
    	// runs as a pure pass-through - full-frame crop, no subsample -
    	// mirroring the proven allon_jpeg_encode shape. The WDMA2 byte
    	// count (1280x960) selects the image out of the taller window.
    	crop.start_x = 0;
    	crop.start_y = 0;
    	crop.last_x = HIRES_RAW_WIDTH - 1;
    	crop.last_y = HIRES_SENSOR_WINDOW_HEIGHT - 1;
    	sensordplib_set_sensorctrl_inp_wi_crop(SENCTRL_SENSOR_TYPE, SENCTRL_STREAM_TYPE,
    			HIRES_RAW_WIDTH, HIRES_SENSOR_WINDOW_HEIGHT,
    			INP_SUBSAMPLE_DISABLE,
    			crop);
#else
    	// High-resolution RAW capture (hires.c): the sensor streams its
    	// stock 2304x1296 and the INP takes the same 1280x960 centre crop
    	// the VGA flow uses (identical field of view) - just without the
    	// 2:1 subsample, so the full-resolution window reaches WDMA2.
    	// This matches Himax's high-resolution capture diagram
    	// (INP -> 1280x960 RAW image to memory).
    	crop.start_x = DP_INP_CROP_START_X;
    	crop.start_y = DP_INP_CROP_START_Y;
    	crop.last_x = DP_INP_CROP_START_X + HIRES_RAW_WIDTH - 1;
    	crop.last_y = DP_INP_CROP_START_Y + HIRES_RAW_HEIGHT - 1;
    	sensordplib_set_sensorctrl_inp_wi_crop(SENCTRL_SENSOR_TYPE, SENCTRL_STREAM_TYPE,
    			SENCTRL_SENSOR_WIDTH, SENCTRL_SENSOR_HEIGHT,
    			INP_SUBSAMPLE_DISABLE,
    			crop);
#endif // HIRES_SENSOR_WINDOW

    	// The datapath is pinned to RAW -> WDMA2 regardless of the
    	// requested dp_type: CAMERA_CONFIG_RUN/CONTINUE re-init the datapath
    	// with the VGA HW5x5+JPEG arguments before every capture start,
    	// which would otherwise clobber the RAW configuration mid-mode.
    	dp_type = SENSORDPLIB_PATH_INP_WDMA2;
    }
    else {
    	crop.start_x = DP_INP_CROP_START_X;
    	crop.start_y = DP_INP_CROP_START_Y;

    	if(DP_INP_CROP_WIDTH >= 1) {
    		crop.last_x = DP_INP_CROP_START_X + DP_INP_CROP_WIDTH - 1;
    	}
    	else {
    		crop.last_x = 0;
    	}
    	if(DP_INP_CROP_HEIGHT >= 1) {
    		crop.last_y = DP_INP_CROP_START_Y + DP_INP_CROP_HEIGHT - 1;
    	}
    	else {
    		crop.last_y = 0;
    	}

    	sensordplib_set_sensorctrl_inp_wi_crop(SENCTRL_SENSOR_TYPE, SENCTRL_STREAM_TYPE,
    			SENCTRL_SENSOR_WIDTH, SENCTRL_SENSOR_HEIGHT,
    			DP_INP_SUBSAMPLE,
    			crop);
    }

	uint8_t cyclic_buffer_cnt = 1;

	int32_t non_support = 0;
	switch (dp_type)
	{
	case SENSORDPLIB_PATH_INP_WDMA2:
		if (g_hires_raw_addr != 0) {
			// Byte count = the image (1280x960). The sensor window is
			// taller (HIRES_SENSOR_WINDOW_HEIGHT): the capture gate opens
			// ~18.4 lines into the frame (constant, measured), so the count
			// completes mid-frame with 960 contiguous lines ->
			// NORMAL_FINISH -> FRAME_READY.
			sensordplib_set_raw_wdma2(HIRES_RAW_WIDTH, HIRES_RAW_HEIGHT,
					NULL);
		}
		else {
			sensordplib_set_raw_wdma2(DP_INP_OUT_WIDTH, DP_INP_OUT_HEIGHT,
					NULL);
		}
	    break;
	case SENSORDPLIB_PATH_INP_HW2x2_CDM:
	    sensordplib_set_HW2x2_CDM(hw2x2_cfg, cdm_cfg,
	    		NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW5x5:
	    sensordplib_set_hw5x5_wdma3(hw5x5_cfg,
	    		NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW5x5_JPEG:
	    sensordplib_set_hw5x5_jpeg_wdma2(hw5x5_cfg
	            , jpeg_cfg,
				cyclic_buffer_cnt,
				NULL);
	    break;
	case SENSORDPLIB_PATH_INP_HW2x2:
		sensordplib_set_HW2x2_wdma1(hw2x2_cfg, NULL);
		break;
	case SENSORDPLIB_PATH_INP_CDM:
		sensordplib_set_CDM(cdm_cfg, NULL);
		break;
	case SENSORDPLIB_PATH_INT1:
	    sensordplib_set_INT1_HWACC(hw2x2_cfg,
	            cdm_cfg, hw5x5_cfg,jpeg_cfg,
				cyclic_buffer_cnt,
	            NULL);
	    break;
	case SENSORDPLIB_PATH_INTNOJPEG:
		sensordplib_set_INTNoJPEG_HWACC(hw2x2_cfg,
	            cdm_cfg, hw5x5_cfg,
	            NULL);
		break;
	case SENSORDPLIB_PATH_INT3:
		sensordplib_set_int_raw_hw5x5_wdma23(DP_INP_OUT_WIDTH,
				DP_INP_OUT_HEIGHT,
				hw5x5_cfg,
				NULL);
		break;
	case SENSORDPLIB_PATH_INT_INP_HW5X5_JPEG:
		if(hw5x5_cfg.demos_color_mode == DEMOS_COLORMODE_RGB)
		{
			sensordplib_set_int_hw5x5rgb_jpeg_wdma23(hw5x5_cfg,jpeg_cfg,
					cyclic_buffer_cnt,
		            NULL);
		}
		else
		{
			sensordplib_set_int_hw5x5_jpeg_wdma23(hw5x5_cfg,jpeg_cfg,
					cyclic_buffer_cnt,
					NULL);
		}
		break;
	case SENSORDPLIB_PATH_INT_INP_HW2x2_HW5x5_JPEG:
		sensordplib_set_int_hw2x2_hw5x5_jpeg_wdma12(hw2x2_cfg,
	            hw5x5_cfg,jpeg_cfg,
				cyclic_buffer_cnt,
	            NULL);
		break;
	case SENSORDPLIB_PATH_JPEGDEC:
	case SENSORDPLIB_PATH_TPG_JPEGENC:
	case SENSORDPLIB_PATH_TPG_HW2x2:
	case SENSORDPLIB_PATH_INP_HXCSC_CDM:
	case SENSORDPLIB_PATH_INP_HXCSC:
	case SENSORDPLIB_PATH_INP_HXCSC_JPEG:
	case SENSORDPLIB_PATH_INT1_CSC:
	case SENSORDPLIB_PATH_INTNOJPEG_CSC:
	case SENSORDPLIB_PATH_INT3_CSC:
	case SENSORDPLIB_PATH_INT_INP_HXCSC_JPEG:
	case SENSORDPLIB_PATH_NO:
	default:
		dbg_printf(DBG_LESS_INFO, "Not support case \r\n");
		non_support = 1;
		break;
	}

	if(non_support == 1)
		return -1;

    if(cb_event != NULL)
    {
	    hx_dplib_register_cb(cb_event, SENSORDPLIB_CB_FUNTYPE_DP);
    }

	return 0;
}


void cisdp_stream_on()
{
    // The HM0360 MD companion shares the CIS I2C bus; make sure the stream
    // command reaches the IMX708 whatever the bus was last pointed at
    // (bench 12 Jul 2026: a stale slave ID left the sensor in standby and
    // every capture timed out)
    hx_drv_cis_set_slaveID(CIS_I2C_ID);

    /*
     * Stream On
     */
    if(hx_drv_cis_setRegTable(IMX708_stream_on, HX_CIS_SIZE_N(IMX708_stream_on, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 on by app fail\r\n");
        return;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 on by app done\r\n");
    }
}


void cisdp_stream_off()
{
    hx_drv_cis_set_slaveID(CIS_I2C_ID);	// see cisdp_stream_on()

    /*
     * Stream Off
     */
    if(hx_drv_cis_setRegTable(IMX708_stream_off, HX_CIS_SIZE_N(IMX708_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 off by app fail\r\n");
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 off by app \n");
    }
}


void cisdp_sensor_start()
{
    hx_drv_cis_set_slaveID(CIS_I2C_ID);	// see cisdp_stream_on()

    /*
     * Stream On
     */
    if(hx_drv_cis_setRegTable(IMX708_stream_on, HX_CIS_SIZE_N(IMX708_stream_on, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR)
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 on by app fail\r\n");
        return;
    }
    else
    {
    	dbg_printf(DBG_LESS_INFO, "IMX708 on by app done\r\n");
    }

    sensordplib_set_mclkctrl_xsleepctrl_bySCMode();

    sensordplib_set_sensorctrl_start();
}


void cisdp_sensor_stop() {
    sensordplib_stop_capture();
    sensordplib_start_swreset();
    sensordplib_stop_swreset_WoSensorCtrl();

    hx_drv_cis_set_slaveID(CIS_I2C_ID);	// see cisdp_stream_on()

    /*
     * Stream Off
     */
    if(hx_drv_cis_setRegTable(IMX708_stream_off, HX_CIS_SIZE_N(IMX708_stream_off, HX_CIS_SensorSetting_t))!= HX_CIS_NO_ERROR){
    	dbg_printf(DBG_LESS_INFO, "IMX708 off by app fail\r\n");
    }
    else {
    	dbg_printf(DBG_LESS_INFO, "IMX708 off by app \n");
    }

    set_mipi_csirx_disable();
}


void cisdp_mipi_reset()
{
    cisdp_stream_off();
    set_mipi_csirx_disable();
    set_mipi_csirx_enable();
    cisdp_stream_on();
}


void cisdp_get_jpginfo(uint32_t *jpeg_enc_filesize, uint32_t *jpeg_enc_addr)
{
    uint8_t frame_no;
    uint8_t buffer_no = 0;
    uint32_t jpeg_enc_filesize_real;

    hx_drv_xdma_get_WDMA2_bufferNo(&buffer_no);
    hx_drv_xdma_get_WDMA2NextFrameIdx(&frame_no);
    if(frame_no == 0)
    {
        frame_no = buffer_no - 1;
    }else{
        frame_no = frame_no - 1;
    }
    hx_drv_jpeg_get_EncOutRealMEMSize(&jpeg_enc_filesize_real);

    //dbg_printf(DBG_LESS_INFO, "current jpeg_size=0x%x\n", jpeg_enc_filesize_real);

    hx_drv_jpeg_get_FillFileSizeToMem(frame_no, g_jpegautofill_addr, jpeg_enc_filesize);
    hx_drv_jpeg_get_MemAddrByFrameNo(frame_no, g_wdma2_baseaddr, jpeg_enc_addr);

    if( jpeg_enc_filesize_real != *jpeg_enc_filesize)
    {
        dbg_printf(DBG_LESS_INFO, "*jpeg_enc_filesize_real(0x%08X) != *jpeg_enc_filesize(0x%08X)\n"
        		, jpeg_enc_filesize_real, *jpeg_enc_filesize);

        //change value
        *jpeg_enc_filesize = jpeg_enc_filesize_real;
    }

    //dbg_printf(DBG_LESS_INFO, "g_jpegautofill_addr: 0x%08X\n" "g_wdma2_baseaddr: 0x%08X\n", g_jpegautofill_addr, g_wdma2_baseaddr);
    //dbg_printf(DBG_LESS_INFO, "current frame_no=%d, jpeg_size=0x%x,addr=0x%x\n",frame_no,*jpeg_enc_filesize,*jpeg_enc_addr);
}

uint32_t app_get_jpeg_addr()
{
	return g_wdma2_baseaddr;
}

// Capacity of the JPEG output buffer (jpegbuf) in bytes. Used by the software
// JPEG encoder (img_correct.c / sw_jpeg.c) to bound its output.
uint32_t app_get_jpeg_sz()
{
	return JPEG_BUFSIZE;
}

uint32_t app_get_raw_addr()
{
	return g_wdma3_baseaddr;
}

uint32_t app_get_raw_sz()
{
	return (IMX708_HW5x5_CROP_WIDTH*IMX708_HW5x5_CROP_HEIGHT*3/2);  //YUV420
}

uint32_t app_get_raw_width()
{
	return IMX708_HW5x5_CROP_WIDTH;
}

uint32_t app_get_raw_height()
{
	return IMX708_HW5x5_CROP_HEIGHT;
}

uint32_t app_get_raw_channels() {
	return SENCTRL_SENSOR_CH;
}

