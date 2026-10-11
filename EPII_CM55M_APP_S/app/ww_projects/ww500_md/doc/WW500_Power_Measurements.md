# WW500 Power Measurements
#### File: WW500_Power_Measurments.md
#### Author: Charles Palmer
#### Date: 18 September 2026

## Background

I had not measured power consumption of the WW500.C02 for some time. 
On 18/9/26 I made measurements of the standard config with the 'latest' software and saw
a higher than expected current. 

It is time we quantify the power consumption and optimise power for the mainstream board. 

Some baseline measurements with kit I have at hand follow. Unless otherwise noted:
* The power is the the 'sleepiest' state. 
* Meaurements made with multimeter and the FTDI connector board with both processors connected with FTDI cables. 

| Board Name | PCB & Rev | Configuration                    | Measured power | Notes |
|------------|-----------|----------------------------------|----------------|-------|
| WILD-XAES  | WW500.C00 | Cameras & SD card removed        | 18uA           | 1     |
|  "         |  "        | Added RP3 camera only            | 23uA  & 102uA  | 2     |
|  "         |  "        | Added both cameras (but HM0360 not seen) | 45uA & 124uA   | 3     |
|  "         |  "        | Added SD card                    | 45uA & 124uA   |       |
| WILD-BCM3  | WW500.C00 | BLE processor only. No mag sensor or LEDs | 9uA            | 4     |
|  "         |  "        | BLE advertising                  | c 350uA        |       |
|  "         |  "        | BLE connected                    | 9uA            |       |
| WILD-AMP7  | WW500.C02 | Cameras & SD card removed        | 97uA           |       |
|  "         |  "        | BLE advertising                  | c 500uA        | 5     |
|  "         |  "        | BLE connected                    | 1.24mA         | 5     |
|  "         |  "        | Added SD card                    | 102uA          | 6     |
|  "         |  "        | Added HM0360, MD inhibited       | 343uA          | 7     |
|  "         |  "        | Added HM0360, MD @ 1Hz           | c. 350uA       | 8     |
|  "         |  "        | As above, _not_in DPD            | 28.5mA         | 9     |
|  "         |  "        | HM0360 in DPD with MODE_SLEEP    | 940uA          | 10    |

#### Notes

1. AI s/w for RP3 camera. Built: 21:34:21 Dec 18 2025  \
BLE s/w Ver: 00.08.08 Built: 12:02:57 Nov 12 2025
2. Why different currents? It seems that I get 23uA between reset and the AI processor waking for the first time 
(after 1 minutes). Then it is 100uA
3. Different currents: same pattern. Lower curent till AI processor wakes. 
4. BLE s/w Ver: 00.30.50 Built: 08:41:27 Sep 17 2026 ('latest') \
Baseline for MKL62BA - it can operate OK (LoRa & BLE) at low currents.
5. Probably higher because the blue LED s active.
6. This shows the SD card draws neglible current in DPD. 
7. HM0360 mode reported at 2 - why not 0? Comment in hm0360_md_setMode() says mode 2 is lower power than 0!
8. Meter says c. 348uA but probably every 1s a brief increase.
9. By accident, entry to DP inhibited. This is possibly the FreeRTOS tickless idel (but very high). 
10. Excessive sleep current in MODE_SLEEP - see below.

## HM0360 MODE_SLEEP

The experiments in `ww500_minimal` show that mode 0 (`MODE_SLEEP`) gives the best power consumption.
Comments in the source code `hm0360_md_setMode()` show that this mode actually gives a high power and show be avoided.

I repeated this experiemnt on 4/10/26 wth the Power Profiler Lit II and this is confirmed. Wvaeforms captured are:

* [HM0360_MODE_SLEEP](HM0360_MODE_SLEEP.png) shows a high c. 800uA basline with regular 10ms spikes.
* [HM0360_MODE_SW_NFRAMES_SLEEP_1](HM0360_MODE_SW_NFRAMES_SLEEP_1.png) shows the slowest (1.9s) 
frame capture 
* [HM0360_MODE_SW_NFRAMES_SLEEP_2](HM0360_MODE_SW_NFRAMES_SLEEP_2.png) zooms into the frame capture. 

The baseline for the HM0360_MODE_SW_NFRAMES_SLEEP mode is still higher than I would like - c. 330uA.

_This remains an open issue to resolve later, perhpas with Himax help._

However lower powers might be possible, as suggested in 
[this TinML talk](https://www.youtube.com/watch?v=7tuq-vz4aVk) - see  the HM0360 demo at 28:40)
which talks  about = "150uW average".

#### Reset

It looks like the AI processor does not reset properly at power up?

## Battery Board Measurements

I thought would measure the efficiency of the VBATT to 3V3 PSU on the battery board.
I did this with the battery board alone, with two cases:

1. No load at all.
2. Soldered a 10k resistor across the 3V3 rail.

Input power source is 4 x AA batteries (measured 5.54V)

Input current measured with a multimeter. Output current calculated by ohms law.

Results:

1. Input current nothing! 0.0uA (perhaps an occasional pulse - perhaps when the chip wakes up
to deliver a bit of current to the output). The input current is measureable if I put my finger across 
the 3V3 output capacitor.

2. Should draw 330uA. Input current = 222uA.
* Power in = 5.54 x 220 = 1218uW
* Power out = V^2 / R = 3.32^2 / 0.01 = 1102uW
* efficiency = 1102/1218 = 0.90

I suppose I should measure at higher currents...

## LED Flash currents

I used the PPK2 to capture waveforms as the flash LED operated. Setup as follows:

* Power source = 4 x AA batteries, but I deliberately inserted some cells that were slightly discharged so that the 
total voltage was c. 5.0V which is the max imum that the PPK2 can accept.
* I enabled the motion detection and set a 1s MD rate.
* Operational parameters initially set as:
 - OP_PARAMETER_LED_BRIGHTNESS_PERCENT (9) = 0 (minimal brightness)
 - OP_PARAMETER_FLASH_LED (13) = 1 (visible LED)
 - OP_PARAMETER_MD_FLASH_LED (21) = 1 (visible LED)
 - OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT (22) = 0
* PPK2 measured current from the battery to the WW500 battery board.
* PPK2 digital channel 2 monitored the STROBE signal - which aligns with the high current pulse.
 
 I let the device run. I waited till it entered DPD and took measurements with the PPK2:
 
 1. Peak current when the LED turned on. 
 2. Average current between the 1s pulses.
 3.	After taking measuremenst with the visible LED I swithec to the IR LED and took some spot measurements - the results were the same.
 4. Note the '0 (off)' value below is with OP_PARAMETER_MD_FLASH_LED =0 (no LED).
 
 Later, I compiled a version for the RP3 camera and took a screen shot when the flash LED turd on to take the picture.
 In this case the LED flash is controlled by a FLASHEN signal from the PCA9574 I/O extender chip - since there
 is no equivalent of the `strobe` signal when operating the RP3 camera. 
 
 I took tthree screen shots from the PPK2 :
 
 * [10% setting](LED_Flash_Current_10_percent.png)
 * [100% setting](LED_Flash_Current_100_percent.png)
 * [10%, RP3](LED_Flash_Current_10_percent_RP3.png)
 
__Results follow__

| Percentage setting | Ipk | Iavg | Percentage setting | Ipk | Iavg | 
|--------------------|-----|------|--------------------|-----|------|
| 0 (off)            |   4 | 0.3  |                    |     |      |
| 0 (low)            |  11 | 0.46 |                    |     |      |
| 5                  |  11 | 0.46 | 55                 | 540 | 13.0 |
| 10                 |  18 | 0.66 | 60                 | 540 | 13.0 |
| 15                 |  41 | 1.3  | 65                 | 580 | 13.7 |
| 20                 |  50 | 1.6  | 70                 | 580 | 13.7 |
| 25                 |  50 | 1.6  | 75                 | 580 | 13.7 |
| 30                 | 139 | 4.0  | 80                 | 580 | 13.7 |
| 35                 | 146 | 4.2  | 85                 | 580 | 13.7 |
| 40                 | 167 | 5.0  | 90                 | 580 | 13.7 |
| 45                 | 169 | 5.0  | 90                 | 580 | 13.7 |
| 50                 | 175 | 4.6  | 100                | 580 | 13.7 |

__Notes:__

1. The percentage values are approaximate and convert into 16-bit values which drive the 
LED circuit in a non-linear way. That goes some way to explaining the odd steps in the 
meausred currents.
2. Some limiting is happening at 65% - possibly a current limit in the PPK2 or
possibly a reflection of high internal resistance of the less-fully-charged AA cells.
This is indicated by the downwards-facing slope at the peak of the [100% setting](LED_Flash_Current_100_percent.png) screenshot. 






