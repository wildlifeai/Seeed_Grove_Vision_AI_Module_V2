# LED Flash: Timing of STROBE vs VSYNC
#### CGP 26 Feb 2026

The challenge: what register settings ensure that the STROBE pin is asserted soon enough
so the LED is on in time for the image capture - i.e. some time before VSYNC.

## Original settings

Prior to the investigations of Feb 2026 these were the STROBE register settings (with STROBE_CFG
set to 0x0B to make the pulses):
```
		{HX_CIS_I2C_Action_W, 0x3080, 0x00},	// STROBE_CFG 		Modes: Static=5; Dynamic 1=3; Dynamic 2=B; Multiple = 13
		{HX_CIS_I2C_Action_W, 0x3081, 0x00},	// STROBE_SEL		0: align to start of reset field 1: align to end of reset field	
		{HX_CIS_I2C_Action_W, 0x3082, 0x00},	// STROBE_FRONT_H 	= front porch (in PCLKO clocks)
		{HX_CIS_I2C_Action_W, 0x3083, 0x20},	// STROBE_FRONT_L
		{HX_CIS_I2C_Action_W, 0x3084, 0x00},	// STROBE_END_H		= back porch
		{HX_CIS_I2C_Action_W, 0x3085, 0x40},	// STROBE_END_L
		{HX_CIS_I2C_Action_W, 0x3086, 0x00},	// STROBE_LINE_H	= line (in rows)
		{HX_CIS_I2C_Action_W, 0x3087, 0x20},	// STROBE_LINE_L
		{HX_CIS_I2C_Action_W, 0x3088, 0x00},	// STROBE_FRAME_H	= number of strobes
		{HX_CIS_I2C_Action_W, 0x3089, 0x04},	// STROBE_FRAME_L

```
As of this date it looks like the following produces adequate pulses:
```
		{HX_CIS_I2C_Action_W, 0x3080, 0x03},	// STROBE_CFG Modes: Static=5; Dynamic 1=3; Dynamic 2=B; Multiple = 13
		{HX_CIS_I2C_Action_W, 0x3081, 0x00},	// STROBE_SEL		0: align to start of reset field 1: align to end of reset field	
		{HX_CIS_I2C_Action_W, 0x3082, 0x00},	// STROBE_FRONT_H 	= front porch (in PCLKO clocks)
		{HX_CIS_I2C_Action_W, 0x3083, 0x20},	// STROBE_FRONT_L
		{HX_CIS_I2C_Action_W, 0x3084, 0x00},	// STROBE_END_H		= back porch
		{HX_CIS_I2C_Action_W, 0x3085, 0x40},	// STROBE_END_L
		{HX_CIS_I2C_Action_W, 0x3086, 0x00},	// STROBE_LINE_H	= line (in rows)
		{HX_CIS_I2C_Action_W, 0x3087, 0x20},	// STROBE_LINE_L
		{HX_CIS_I2C_Action_W, 0x3088, 0x00},	// STROBE_FRAME_H	= number of strobes
		{HX_CIS_I2C_Action_W, 0x3089, 0x04},	// STROBE_FRAME_L
		{HX_CIS_I2C_Action_W, 0x356A, 0x01},	// FRAME_OUTPUT_EN_B - Enable output in Context B (so view VSYNC)
```
__IMPORTANT: the FRAME_OUTPUT_EN_B entry is _only_ to make the VSYNC signal visible
during the DPD state, when otherwise is is not enabled.__

That gives the following results:

`Timing with Mode 3 - timing in ms`
```
                | 6.6|  9     |                         | 6.6|  9     |
                
                +----+------------------+               +----+--------------------------+      
STROBE          |    |                  |               |    |                          |     
        +-------+    +                  +-----//+-------+    +                          +----

                              |  10     |                             |     21          |    
                                              
                              +---------+                             +-----------------+     
VSYNC                         |         |                             |                 |   
        ----------------------+         +-----//----------------------+                 +-----
```
### Notes:
1. The first pair of pulses are during the DPD with regular pulses (typically at 1s intrvals). 
VSYNC is normally not enabled.
2. The second set of pulses are after MD wakes the processor from DPD.
3. The strobe includes a short glitch to 0 after 6.6ms
4. The difference in VSYNC pulse width is explained by the fact that the DPD (Context B) settings
have 0x3561 and 0x3562 set to sub-sample 2. The HSYNC pulses are also shorter by a factor of 2.
5.	In due course it might be possible to reduce the VSYNC in DPD to reduce the power consumption...

