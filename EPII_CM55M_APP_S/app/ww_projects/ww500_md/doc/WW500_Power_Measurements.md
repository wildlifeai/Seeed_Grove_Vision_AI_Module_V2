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


#### Reset

It looks like the AI processor does not reset properly at power up?

