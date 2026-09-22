# Task: Create an image with a minimal FreeRTOS port

#### File: CLAUDE_Minimal_FreeRTOS.md
#### Author: Charles Palmer
#### Date: 20 September 2026
#### Updated: 21 September 2026 (the Status section and typo fixes by Claude)


## Status (22 September 2026)

The tasks below have been done. The app is built and runs on the bench. Where to look:

* [README.md](README.md) (this folder): status, outcome and open items.
* [CLAUDE_Minimal_FreeRTOS_proposal.md](CLAUDE_Minimal_FreeRTOS_proposal.md): the proposal and how it was answered. Section 14 says where the app differs from it.
* `ww500_minimal/doc/README.md` (in the app folder): how the app works, its settings and CLI commands.
* `ww500_minimal/doc/power_investigation.md` (in the app folder): the power measurements, with a short summary of what was learnt and what is not known.

Step 7 (the FatFS task) is built and works on the bench (22 September 2026): the card mounts, the boot count increments and files can be written and read. Its design and the answers to the questions are recorded in
[README.md](README.md) and in `ww500_minimal/doc/README.md`. Step 8 (the HM0360 image task) is built and works on the bench (22 September 2026): the camera is found, `capture` saves a JPEG named from the boot count, and `cam` shows and sets the sensor mode. The current measurements (each HM0360 mode, DPD with the camera and card fitted, sensor state over DPD) are still to be done: see "Step 8: outstanding" in [README.md](README.md).

Charles committed the work at the end of 22 September 2026 (see `git log`); nothing has been pushed by Claude. `ww.mk` still selects `ww500_minimal` (set it back to `ww500_md` before a production build).

## Background

Power consumption measurements on a WW500 are higher than expected. I have a WW500_C00 
board that contains the MKL62BA (BLE processor) and almost nothing else - this shows
a very low sleep current. I want to create a second WW500_C00 board that contains 
the HX6538 (AI processor) and almost nothing else - so I can also measure sleep current 
(and operating current).

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
* If the existing code looks unnecessarily complex for this minimal build, ask for advice from me.
5.	We will review your proposal document together before you start coding.
6.	In all .c and .h files you create (or copy into `ww500_minimal`) obey the rules in [c_file_format.md](../../c_file_format.md)

## Further tasks

Add new objectives here, numbered from 7. Record the outcome of each in [README.md](README.md) (Status, Outcome, Open items) and keep the durable description of the app in
`ww500_minimal/doc/README.md`.

The work done by 21 September showed good DPD current (10uA) with minimal functionality. I want to 
extend the app to add other functionality present in ww500_md to confirm we can still get low DPD current. 
This is to add the HM0360 camera and save JPEG files to SD card. In turn this should be in two steps:

7.  Add a FatFS task. This should check for presence or absence of the SD card. If present it can be used to
save and restore state - initially only a counter for the number of boots. I have already soldered an SD card socket to the PCB. There is no
extra hardware, only code to initialise the SPI interface. Use the ww500_md fatfs task for guidance, but make this code light-weight:
I suspect the ww500_md has got bloated. All we need to do is keep track of boot count and write JPEG files. No 
need for directories at the moment.
8.	Once the FatFS task is incrementing a boot count file we will add an image task that takes pictures from
the HM0360 and saves the JPEG. Don't start this until step 7 is complete.
 


