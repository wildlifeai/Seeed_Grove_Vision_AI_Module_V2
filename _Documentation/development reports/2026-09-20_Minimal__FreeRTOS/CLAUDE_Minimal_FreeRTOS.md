# Task: Create an image with a minimal FreeRTOS port

#### File: CLAUDE_Minimal_FreeRTOS.md
#### Author: Charles Palmer
#### Date: 20 September 2026

## Background

Power consumption measurements on a WW500 are highr than expected. I have a WW500_C00 
board that contains the MKL62BA (BLE processor) and almost nothing else - this shows
a very low sleep current. I want to create a second WW500_C00 board that contains 
the HX6538 (AI processor) and almost nothing else - so I can also measure sleep current 
(and opearing current).

Therefore I want a FreeRTOS port that is quite minimal:

* Boots from one or more sources.
* Initialises a minimum set of peripherals.
* Executes some simple FreeRTOS tasks (blinky, CLI probably) and after
a certain time enters DPD.
* Excludes the camera, FatFS, BLE interface, neural network processing.

## Structure

The work will require a new folder at this level:

```
Seeed_Grove_Vision_AI_Module_V2\EPII_CM55M_APP_S\app\ww_projects\ww500_minimal
```

and this will mean a new entry in `ww.mk` and matching changes elsewhere as necessary to build the new image:
```
APP_TYPE = ww500_minimal
```

As with the `ww500_md` pattern, I expect there will be files:

* `ww500_minimal.mk`
* `ww500_minimal.ld`
* `ww500_minimal.c`
* `ww500_minimal.h`


## Wire Links

I will add wire links etc as follows. These connect the HX6538 pins to LEDs and switch normally connected to the 
BLE processor module, which normally drives the LEDs and listens for the switch SW1. 

1. **Red LED** Make use of the Red LED components LED1 & R22. Wire link from U1 pin 21 to HX6538 PB9
at U2 pin 4 (PB9 = PDM_CLK).
2. **Blue LED** Make use of the Blue LED components LED2 & R20. Wire link from U1 pin 23 to HX6538 PB10
at U2 pin 1 (PB10 = PDM_DATA).
3. **Green LED** (possible future addition) Make use of the Green LED components LED3 & R40. Wire link from U1 pin 10 to HX6538 (t.b.d.)
4. **WAKE Switch** Fit SW1 and use it instead of /BLE_WAKE to HX6538 pin PA0. Wire link from U1 pin 12 to pin 24.


## Tasks for Claude

1.	Ask questions where this is useful.
2.	Create a markdown file (separate from this file) that proposes an approach to generate the source code.
3.	Use `ww500_md.c` as a starting point. Follow the model there which places every task in its own .c & .h files.
4.	Where code exists it should be re-used, but:
* Take care not to include code that could have power consumption implications.
* If the existing code looks unnecessarily complex fro thsi minimal build, ask for advice from me.
5.	We will review your proposal document together before you start coding.
6.	In all .c and .h files you create (or copy into `ww1500_minimal` obey the rules in [c_file_format.md](../../c_file_format.md)


