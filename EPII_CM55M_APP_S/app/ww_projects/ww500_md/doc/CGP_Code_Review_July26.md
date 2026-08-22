# Code Review By Charles
#### Starting 26 July 2026
#### Ended ? t.b.d.

## Background

Victor made many changes in 3 PRs (see his email of 12/07/2026 21:43).

1. __feat/camera-features-combined (PR #141)__

What it is: light sensor / AE work, dual-camera day/night switching (A/B firmware slots, slot labelling, switchslot), 
RP3 colour camera fixes, and OP20–27 operational parameters.

Where the detail is: the PR #141 description is the full write-up. 
In-repo docs that go with it: EPII_CM55M_APP_S/app/ww_projects/ww500_md/doc/ 
— especially slot_selector.md, 
STROBE_timing.md and the AE/light-sensor notes.

2. __feat/ble-fast-transfer (PR #142)__

What it is: the Himax half of the fast BLE file transfer (~0.24 KB/s → 6–8 KB/s bursts). 
SD write-path reliability (f_sync cadence, overwrite fix, 
fail-fast on short writes), the I2C slave re-arm moved into the TX-done interrupt, 
transfer-session log suppression, missing-master window 1 s → 4 s, slot-label robustness, 
and defaults: auto camera switching ON (OP26=1) and MD IR brightness 50% (OP22).

Where the detail is: PR #142 description. Related docs: doc/WW500_crc_checks.md 
and the firmware update/recovery guide in the same folder.

3. __feat/uart-live-preview (PR #140, still WIP)__

What it is: live camera preview over the console UART plus auto-exposure/white-balance 
tuning tools — built to sort the RP3 colour/quality issues. Functional but rough; 
review for direction rather than polish.

Where the detail is: PR #140 description.

I should look at the chnages and understand them, at minimum because I will have to make 
further changes and I need to understand what has changed.

## PR #141

Summarised by [REVIEW_PR141.md](../../../../../REVIEW_PR141.md)

Comments as follows:

__1. Dual-camera day/night switching (manual + automatic)__

First - good idea to allow different images support different cameras.

This stores metadata in the Firmware Image Slot Selector - the camera supported by each firmware image.

__Updated doc `ww500_md/doc/slot_selector.md`__

This explains a new field in the
Firmware Image Slot Selector which identifies the camera that the image 
expects to use.

___COMMENT___ The comments say HM0360 is to be used at night and RP3 in daylight: maybe sometimes
(for cost or power consumption etc) we might just want to use the HM0360 by itself. e.g. 
AI predator identification. Maybe we should retain the flexibility in docs and implementation?

__New files `camera_switch.h` & `camera_switch.c`__ has these public functions:

```
// The camera (HM0360, RP3 or unknown) this firmware was built for. Only used by CLI "slots" command.
uint8_t cameraSwitch_thisVariant(void);

// Human-readable name for the selected camera. Only used by CLI "slots" command.
const char * cameraSwitch_variantName(uint8_t variant);

// Potentially, update Firmware Image Slot Selector with meta data (the selected camera).
// This is done only once (maximum) when a new firmware image is booted for the first time.
// Only call when vImageTask() is initialised - i.e. at wake from DPD.
void cameraSwitch_labelBootSlot(void);

// Automatic switching check - call after each AE light-level decision
// called in image_task.c handleEventForCapturing()
// This is discussed in the AE section below.
bool cameraSwitch_autoSwitchCheck(void);
```

___COMMENT___ Perhaps these could be renamed more intuitively (for me):

* cameraSwitch_thisVariant() -> cameraSwitch_getCurrentCamera()
* cameraSwitch_variantName() -> cameraSwitch_getCameraName()
* cameraSwitch_labelBootSlot() -> cameraSwitch_setCurrentCamera()
* cameraSwitch_autoSwitchCheck() -> cameraSwitch_considerSwitching()

___COMMENT___ I am struggling to follow the sequence of events here. The AI generated comments
are not intuitive. I think the sequence is as follows:

1. cameraSwitch_labelBootSlot() called when Image Task initialises at DPD.
2. xip_get_active_slot() calls read_slot_selector() to read the Firmware Image Slot Selector to get the active slot
2. calls xip_set_slot_variant() with the slot (A or B) and camera type.
3. This calls read_slot_selector() __again__ to get the slot information and read_slot_meta() 
to get the meta information (two cameras in two slots)
4. If the meta data (selected camera) is correct then exits. 
5. Else calls write_selector_sector() which erases the Firmware Image Slot Selector and re-writes 
the slot selector (unchanged) and the new camera into the meta data

Note that xip_set_slot_variant() is also called when writing a new firmware image, to set the meta data
(camera type) for that image to UNKNOWN.

I have updated comments so they make more sense to me.

IMHO this could have been simpler and more efficient (fewer flash read operations).
I would have done the cameraSwitch_labelBootSlot() call before entering DPD (or similar) 
as I have been trying to keep activities at DPD to a minimum to reduce the time between leaving DPD
and taking the fisrt picture.

___LATER:___ I have made the cameraSwitch_labelBootSlot() call conditional on cold boot.
The code that changes the slot also calls `app_getResetRequest()` which causes the cold boot. 

__New CLI commands__

*  `"slots"` - Report the active firmware slot and the camera type in each slot
*  `"switchslot"` - Boot the firmware in the other slot

(`ble_commands.md` has been updated to include these.)

__New Operational Parameter__

`26 OP_PARAMETER_SLOT_SWITCH`

Automatic light-based camera image switching: 

* 0 = off (manual 'switchslot' only)
* 1 = automatic (PLANNED - see camera_switch.c)

__New code in `xip_manager.h` and `xip_manager.c`__ has these 4 new public functions:

```
//  Create the SPI mutex before the scheduler starts
void xip_manager_preinit(void);

// Report which firmware slot the bootloader will execute.
int xip_get_active_slot(void);

// Read the camera in use by a slot.
int xip_get_slot_variant(uint8_t slot);

// Updates Firmware Image Slot Selector with meta data
// (the camera supported by one firmware slot)
int xip_set_slot_variant(uint8_t slot, uint8_t variant);
```

___COMMENT___ I have moved the 3 new defined to the top of the .h file.
(And I have done this in other files as well: all defines must be together at the top).

__2. Camera field-tuning CLI (camreg / vcm)__

This is about fixing the RP3 camera appearance. 

Documented here:
* `_Documentation/camera-field-tuning-roadmap.md` 
* `_Documentation/camera-phase0-bench-runbook.md`\

The first of these explains the problems and a roadmap for making the changes. 
The aspect that interest me the most is how Claude figured out how where the problems were.
It seems that this document refers to ideas that might have been tried but are not seen in the 
final code - such as `cisdp_sensor_apply_params()` and `CAM_EXPOSURE`, `CAM_GAIN`, `CAM_WB_RED_GAIN`, `CAM_WB_BLUE_GAIN`.

The second doc `_Documentation/camera-phase0-bench-runbook.md` provides instruction on how to conduct experimenst into RP3 camera 
white balance and focus. The results are not recorded.

___COMMENT___ 
1. Because the above docs don't seem to contain material of on-going use (but are rather descriptions of a development process)
they should be removed from the _Documentation folder. On my own machine I have a `cluade` folder within the `ww500_md` folder
fir this use - I think this is the place for this kind of doc.
2. Much of the code here is about changing RP3 camera register changes -
I have not seen a table containing these changes - does it exist?
3. Looks like the docuemnts decribe a process of seeing if RP3 white balance can be 
fixed by writing to camera register. It looks like this failed. Has it?

__New CLI commands__

* `camreg` = reads and writes RP3 camera registers
* `vcm` = Set the focus lens position, for bench/field verification of manual focus.

(`ble_commands.md` has been updated to include these)

The `camreg` command does this:
1. read a camera register
2. write a camera regsiter
3. list register values updated
4. clear the list of updated values

The `vcm` command does this:
1. `probe` checks if there is focus motor available (yes!)
2. `vcm <value>` sets a focus distance (0= infinity, 1023 = close) - this works (I can see and hear the ens moving).

I have not yet looked at the effects of the focus mechanism.

If I set `vcm 1023` then the lens pops out (and clicks as it does). Then when the device enters DPD it pops back,
 presumably when the RP3 camera is powered off.

__Camera Focus__ is discussed in this [Arducam page on auto-focus](https://blog.arducam.com/raspberry-pi-camera/autofocus/)
The `vcm` command appears to attempt to control focus. Results don't seem to be presented. 

That suggests that (a) position will have to be set at every boot; (b) it might draw (extra) power if at a non-infinity position.

__New code in `cis_file.h` and `cis_file.c`__ has these 6 new public functions:

These are to manipulate a table of RP3 camera register settings in a table
`static HX_CIS_SensorSetting_t stagedSettings[CIS_FILE_MAX_STAGED];` with 24 entries

```
// Called when fastfs_task starts - loads from CAMERA_EXTRA_FILE (`RPV3_EX.BIN`)
void cis_file_loadStagedFromFile(void);

// True once the staged values are loaded from SD card
// Also if there is no file
bool cis_file_isStagedLoaded(void);

// Get the number of entries in stagedSettings[]
uint16_t cis_file_getStagedCount(void);

// Point to the table stagedSettings[]
// Used by CLI `camreg list` command and by camRegSaveTable[] - both in `CLI-commands.c`
HX_CIS_SensorSetting_t * cis_file_getStagedTable(void);

// Add an entry to stagedSettings[]
// Used by CLI `camreg <addr> <value>` command in `CLI-commands.c`
bool cis_file_stageReg(uint16_t addr, uint8_t val);

// clear the table
// Used by CLI `camreg clear` command in `CLI-commands.c`
void cis_file_clearStaged(void);

// Also this has been extended:
// the contents of CAMERA_EXTRA_FILE (`RPV3_EX.BIN`) are copied to camRegSaveTable[] 
HX_CIS_ERROR_E cis_file_process(const char *filename);
```

___COMMENTS___

1. The code that supports the `camreg` commmand is quite big, and also inefficient
(e.g. CAMERA_EXTRA_FILE is read twice at boot; this increases the time before the first
image can be captured). I can see that it can be useful while investigating
camera settings, but will not be required in production. I would be inclined to put all the code
inside `#ifdef STAGEDSETTINGS` and use this code only during development.
2. `camRegFileName` is almost certainly not required - CAMERA_EXTRA_FILE could be used instead.
4. Check out `APP_MSG_CLITASK_DISK_WRITE_COMPLETE`
5. It looks like CAMERA_EXTRA_FILE (`RPV3_EX.BIN`) is read twice - once when fastfs_task starts 
and once in configure_image_sensor() in the image task.
6. Looks like  CAMERA_EXTRA_FILE (`RPV3_EX.BIN`) is read in 2 different places and written by `camRegSaveTable()` in `CLI-commands.c` - it would be more elegant of they were in the same file.
7. Code using `camRegWritePending` can only have been added by AI: this prevents multiple writes to 
CAMERA_EXTRA_FILE (`RPV3_EX.BIN`) while the new regsiter settings are being written, but since the disk write
operation takes milliseconds there is no chance of a user entering new values from a keyboard...
8. I created the CAMERA_EXTRA_FILE idea to tweak camera registers, especially during development.
But the CAMERA_EXTRA_FILE  file is a binary file created by a Python script from a human-readable
text file that identified (to humans) the register address and value, and comments. 
See `scan_cis_settings.md`. 
This appears to be missing from the Claude code. So after the values have been tweaked there
does not seem to be a human-readble record of the register chnages. This should be corrected, I think, even if 
a human creates the text file and uses the Python script to recreate the CAMERA_EXTRA_FILE (this might
only need to be done once).

__3. AE light-sensor flash control__

This is about using the HM0360 as an automatic light sensor, to determine when to switch cameras
and turn on/off the flash.  

Documented here:
* `_Documentation/AE_Light_Sensor_Roadmap.md`

Several new Operational Parameters:

| Index | Name                                  | Default Value | Notes                                                |
| ----- | ------------------------------------- | ------------- | ---------------------------------------------------- |
|    21 |OP_PARAMETER_MD_FLASH_LED 			| 2             | LED used to illuminate motion-detection frames while asleep: 0 = none, 1 = visible, 2 = IR |
|    22 | OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT | 5          | Brightness of the motion-detection illumination (percent; 16 hardware levels) |
|    23 | OP_PARAMETER_AE_DARK_THRESHOLD 		| 65            | AE Mean (0-255) below this means the scene is dark and the flash is needed. See [AE_Light_Sensor_Roadmap.md](AE_Light_Sensor_Roadmap.md) |
|    24 | OP_PARAMETER_AE_CHECK_INTERVAL 		| 15            | Interval (minutes) between periodic AE light-level checks when the flash is in AE mode (op13) or auto camera switching is on (op26), and timelapse is disabled. 0 disables |
|    25 | OP_PARAMETER_AE_FLASH_STATE 			| 0             | Last AE flash decision (0/1). Runtime state persisted across DPD - not intended to be set by users |
|    26 | OP_PARAMETER_SLOT_SWITCH 				| 0             | Automatic light-based camera image switching: 0 = off (manual `switchslot` only), 1 = automatic - after each AE light check, if the dark/bright decision (op25) wants the other camera variant and the other slot holds it, the device switches boot slot and reboots at the next sleep. See `camera_switch.c`. Note: the light decision is computed when the flash is in AE mode (op13) **or** op26 = 1, so auto-switching works even with the flash off |

Looks like there are new Python tools to investigate this:

* `ae_monitor.py` = "Holds the device awake and repeatedly triggers an on-demand capture, printing
the firmware's own light-sensor decision each time"
* `ae_threshold_analysis.py` = "Analyses Auto-Exposure register data extracted from WW500 JPEG MakerNote EXIF
to determine optimal darkness thresholds for automatic flash control."
* Also `ww_serial.py` - what is this used for? 

I could not get `ae_monitor.py` to run - I ran a local Claude to analyse and fix the problem - the result is 
changes to the script both to change the logic to match C code changes, and to add more explanation
about how to run the program. The code now seems to run, and the `--help` command now 
produces more info, but I have not invested time to understand this output...

I have not run the other scripts.

DOC SAYS: "Decision path now lives in `ledFlashNewAEStats()`; `image_task.c` calls
`hm0360_md_getAEStats()` on each AE-check wake."
I see `hm0360_md_getAEStats()` called in handleEventForCapturing() in image_task.c - 

New code in `image_task.c`:

* In `image_sleepNow()` - if appropriate sets a timer to wake the AI processor after OP_PARAMETER_AE_CHECK_INTERVAL minutes.
* When the image task starts a new switch `aeCheckOnlyWake` is set to determine if this is the light-sensor wake.
* Image task then requests an image capture and handleEventForCapturing() inhibits NN processing and file writing.
* There is a call to `hm0360_md_getAEStats()` to get AE values 
* these values are then passed to `ledFlashNewAEStats()` to determine whether the flash should be used.
* Then `cameraSwitch_autoSwitchCheck()` is called to see if there should be a change of camera 
(if OP_PARAMETER_SLOT_SWITCH has been enabled). If so this is set up and takes effect after the next wake.


New code in `hm0360_md.h` and `hm0360_md.h`:

* HM0360_AE_STATS_T - structure to aggregate AE statistics over several successive frames. 
* New function `HX_CIS_ERROR_E hm0360_md_getAEStats(uint8_t nSamples, uint16_t gapMs, HM0360_AE_STATS_T * stats);
`

Claude has created a complex sampling operation in hm0360_md_getAEStats() which involves
running the HM0360 and taking multiple AE measurements to provide an 'average' value. 

New code in `ledFlash.c` & `ledFlash.h`:

* Old function `ledFlashNewAEValues()`retained as a legacy use.
* New function `void ledFlashNewAEStats(HM0360_AE_STATS_T * stats);` which accepts the
aggregated AE stats from `hm0360_md.c`, to decide flash state.

The `ledFlashNewAEStats()` function decides whether LED should be enabled or disabled. It adds hysteresis based .
and it also uses `OP_PARAMETER_AE_FLASH_STATE` to store the flash state across DPD periods.

There are two new Operational Parameters relating to LED flashing during motion detection 
(while in DPD). Explained in `AE_Light_Sensor_Roadmap.md` "the motion
detection illumination and the capture flash are independently configurable"

However it looks like MD flash is disabled if OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT == 0 
but this is not necessary as value 0 means "dim" not "off". 

| Index | New                                   | Old | Notes   |
| ----- | ------------------------------------- | ------------- | ------------- |
|    21 | OP_PARAMETER_MD_FLASH_LED 			| OP_PARAMETER_FLASH_LED |  |
|    22 | OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT | OP_PARAMETER_LED_BRIGHTNESS_PERCENT | |

___COMMENTS AND QUESTIONS___

1. Doc says: "On-hardware bench testing (WW500_C02, RP3 firmware, device sealed in a fully
dark box)" - who did these tests?
2. How good is the light sensor functionality? 
3. I changed the code and documentation relating to OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT
so the flash was not inhibited when this was 0.
4. TODO - consider merging the 15 minute wake for AE with a 15 minute LoRaWAN pin interval.  


__4. RP3 colour camera: software white balance (green-tinge fix)__

AI says this:

Because the Sony IMX708 sensor does not feature on-chip white-balance control, raw images taken straight from its hardware stream suffer from a severe green tinge.
## Why the Raw Image Has a Green Tinge

* Sensor Physics: Silicon image sensors are naturally more sensitive to green photons than to red or blue.
* Bayer Filter Mosaic: The IMX708 uses a standard color filter array that contains twice as many green-filtering pixels as red or blue ones.
* The Result: Without calibration, the raw data is heavily weighted toward green, resulting in a swampy, green-tinted image.

## How the Raspberry Pi Corrects It
The Raspberry Pi corrects this green tint immediately using its dedicated hardware Image Signal Processor (ISP) alongside software algorithms. [1] 

* The Tuning File: When a camera application starts, libcamera reads a calibration file called imx708.json.
* Static Baseline: This JSON file contains hardcoded values that act as a baseline multiplier to boost the weaker red and blue signals to match the dominant green levels.
* Active AWB Algorithms: For real-time changes (like moving from indoor yellow lighting to outdoor sunlight), the Pi runs an Auto White Balance (AWB) loop. The hardware ISP analyzes the incoming frame metadata and dynamically scales the red and blue gains on the fly to keep white objects looking truly white. [2, 3] 

## How Other Processors Must Deal With It
If you port the IMX708 to another host platform—such as an NVIDIA Jetson, an NXP i.MX processor, or a custom FPGA board—the system must handle the data in one of two ways:

* Hardware ISP Porting: If the processor has a built-in ISP, you must convert the metrics from the imx708.json file into that specific processor's proprietary tuning format to drive its internal AWB engine.
* Software Post-Processing: If the processor lacks a dedicated camera ISP, the raw, green-tinted stream is written directly into system memory. You will then have to use a software pipeline (like an OpenCV script or a GStreamer element) to manually multiply the red and blue channels for every single pixel, which can introduce heavy CPU overhead.

Claude has created `RP3_white_balance_reencode_issue.md` which (partly) explains the issue and 
its attempts at finding a solution. It looks like Claude must have made several attempts to run (undocumented) 
HX6538 functions to get hardware to do colour transformation
and then JPEG encoding. Looks like that failed. Difficult for me to understand what was done.

Claude also seems to have conducted experiements to find the desired what balance correction factors.
There is a `_Tools/wb_configs/` folder containing CONFIG.TXT files for testing colour balance,
but no explanation about what tests were run. 

New code follows:

New file `img_correct.c` implements 2 new functions:

```
// Colour-correct the current capture and re-encode it to JPEG.
bool img_correct_process(uint16_t rGainQ8, uint16_t bGainQ8);

// returns the address and size of the jpeg buffer.
void img_correct_get_jpeg(uint32_t *size, uint32_t *addr);
```

This includes `wb_apply_yuv420()` which uses magic numbers - where did they come from?

New file `sw_jpeg.c` implements 1 new function:

```
// Called from `img_correct_process()` in `img_correct.c`.
uint32_t sw_jpeg_encode_yuv420(const uint8_t *yuv, uint32_t w, uint32_t h,
                               uint8_t *out, uint32_t outCap, uint8_t quality)
```

There are two new Operational Parameters to adjust the white balance

| Index | Name                    | Default Value | Notes                                                |
| ----- | ------------------------ | ------------- | ---------------------------------------------------- |
|    27 | OP_PARAMETER_WB_RED_GAIN 	| 286  | Software white-balance RED gain, Q8.8 (256 = 1.0x, 0 = correction off). RP3 colour camera only |
|    28 | OP_PARAMETER_WB_BLUE_GAIN | 326| Software white-balance BLUE gain, Q8.8 (256 = 1.0x, 0 = correction off). RP3 colour camera only |

Looks like these values have been added to the EXIF data 'maker notes' tag. 

A typical console output reads:

```
Colour correction: R x286/256, B x326/256, SW-JPEG 5427 bytes in 222ms
// LATER:
Colour correction: R x286/256, B x326/256 in 44ms
SW JPEG 11600 bytes in 184ms
```

__Summary__ I think that Claude has concluded that colour balance/white balance has to be done
by software, in `img_correct.c`. It then attempted to get the HX6538 hardware to do the JPEG encoding of the
colour-corrected image, but failed, and so added its own software-only JPEG encoder.

___COMMENTS AND QUESTIONS___

1. Claude has done a lot of work and not documented it. Hopefully this work might be
recorded in memory by Claude on Victor's computer? If so can we ask for explanations and 
better documentation of this process? Some points follow:
2. Where do the magic numbers come from in the colour correction code? Convert these to 
#define constants.
3. There is a `_Tools/wb_configs/` folder containing CONFIG.TXT files for testing colour balance. 
How was the colour balance testing done and what do these files do? 
These are temporary files andprobably should be somewhere else (where?)
4. The colour correction and jpeg encoding time is quite long. I have modified the code to separate these:
Colour correction 44ms, jpeg encoding 183ms - can we reduce this?
5. There is already a JPEG encoder within the SDK - this might be more efficient, and a better
approach than re-inventing the wheel - see `library/JPEGENC`
6. Can we document what is happening and ask Himax if there is a better way to do things,
both for colour correcton and JPEG encoding?

__5. EXIF provenance & telemetry in every JPEG__

New fields are saved in the EXIF: red and blue colour balance values, 
and whether the flash was used (binary).

It would be more useful if the flash indication said which flash was used (none, visible or IR).
I have changed the code accordingly. 

Claude has misunderstood the issue of how NN output is to be saved in EXIF (and displayed on the console etc).
It has produced a whole document on the topic: `doc/NN_confidence_EXIF_not_written.md`

It has then messed with the code. I would prefer to restore it. 

__Previously:__

1. The NN output vector is an array of 8-bit values called 'logits' - these are explained in 
`WW500_Neural_Network_Operation.md`. These are placed in `outCategories[]` array and saved 
in EXIF using the `TAG_NN_DATA` tag. The user-readable category names were saved in the EXIF
`TAG_USER_COMMENT` tag. So when Claude writes "images carry no NN scores for the website to ingest"
it is wrong, AFAIK.
2. Tobyn had added a load of code that processed the NN output 'logits' and converted them to
percentages. I decided to comment this out as (a) it was quite complex, (b) used floating point
arithmetic (the only place in our code that used that and it involves new libraries), (c) the
percentages were derived from logits and could be created at the server at any time if they were
needed/desired. The logits were "native" TFLM output, simpler, more compact etc.
2. I bracketed the code that produced percentages with `#ifdef USE_PERCENTAGE` switches. I also
created a `#ifdef ENABLE_EXIF_CONFIDENCE` switch - it was always in the same state as `USE_PERCENTAGE`
(I know, redundant and a bit confusing.
3. My intention was at some later state to delete the percentage code entirely.
4. The EXIF data contained all the NN output data that was necessary, more succinctly.

__Claude's Changes:__

1. Uncommented `#define USE_PERCENTAGE` - this makes `ENABLE_EXIF_CONFIDENCE` true also. 
2. Consequently NN output was calculated as percentages.
3. And EXIF 'user comment' tag is filled with percentages rather than logits. 

___COMMENTS AND QUESTIONS___

1. Unless the app and webside require percentages, I would like to remove the precentage code.
Also correct commants that have been added.
2. Can we find some models that run on the board so that the model code can be tested - e.g. is there
somewhere for them in github?
3. Remove NN_confidence_EXIF_not_written.md
4. Note the flash LED value has been changed (0, 1, 2 instead of 0, 1) - this might require changes in app or website.
5. We discussed adding the VCM (focus) parameter, probably both as a Operational Parameter and in EXIF. For the future, perhaps.
6. See if we can move more of the EXIF code from image_task.c to exif_builder.c


__6. Firmware stability fixes (found on the bench)__

Three issues apparently found and fixed:
1. IF-task deadlock:
2. SPI flash init race: relates to xSPIMutex being created _after_ the scheduler has started.
3. Capture frame-timeout retry:

___COMMENTS AND QUESTIONS___

1. IF-task deadlock comment says "with a bounded 3 s wait" - I see no sign of this. It also refers to code
changes in image_taks.c - I see no sign of this. Is it worth asking Victor's Claude for its memory of this?
2. Regarding `xSPIMutex` - all the other mutexes have been created before the scheduler started.
I think this is Tobyn's work. Let's return to this when I have checked the other PRs 
and clean it up. // TODO - move xSPIMutex creation to ifTask_createTask()?
3. Regarding `Capture frame-timeout retry` - I think the fix is ineffective, as I have seen the 3 attempts
in the new code. I have placed this changed code inside `#ifdef WDTIMOUTFIX` with a view to removing it later.
Can we ask Victor's Claude for info and whether there is evidence that this works?

__7. Operational-parameter persistence__

This seems to save operation parameters which are set via the console (or app) `setop` command.
It creates a new `APP_MSG_FATFSTASK_SAVE_CONFIG` message, sent by the CLI task and received by the
fatfs task. This calls `save_configuration()` which saves the `CONFIG.TXT` file.

Unlike the inactivity timeout (or WDT fault) before entering DPD, which sends a APP_MSG_FATFSTASK_SAVE_STATE 
message, and also uses `save_configuration()`, this does not unmount the fatfs etc.

According to Claude, "Before this was added, op parameters were only written to CONFIG.TXT on
the capture/`SAVE_STATE` path, so a `setop` followed by a plain inactivity sleep
- with no capture - was silently lost on the next wake." I really don't understand this.
A 'plain inactivity sleep` kicks of the saving of the state file. 

Also - the Claude note refers to `SAVE_STATE` - I see no code for this.
My impression therefore is that the Claude notes are not accurate.

___COMMENTS AND QUESTIONS___

1. Can we ask Victor's Claude to justify this code.

__8. Firmware update & recovery guide__

This seems to be documentation only:

New or revised docs:
* `_Documentation/firmware_update_and_recovery.md` 
* `_Documentation/ble_commands.md` - adds the new commands added in the PR and some notes about them.
* `_Documentation/bootloader.md` - adds some cross-rererences to other docs.
* `MANIFEST/README.TXT` - New information about the naming of the image files.
* `config_file.md` - updated with new Operational Parameters and changed flash parameters.
* `Compile_and_flash.md` - Claude claims this is 'SUPERSEDED (July 2026)'

`firmware_update_and_recovery.md` is new and Claude-generated. It describes the two 
firmware images (one for each camera) and how they can be loaded. 
It describes a new file naming convention, which is fine. `VYMDDHMM.IMG`
( This is defined in `manifest/readme.txt`)

___COMMENTS AND QUESTIONS___

1. Where are the new names applied? Can I generate these files locally 
(e.g. a script to rename `output.img`?)
2. It seems the extar camear regsiter files have been moved from the SD card MANIFEST 
folder to the root of the SD card. This seems a retrograde step as it would complicate
provisioning of WW500s by copying and plugging in SD cards. Discuss. 
(Also - has this been tested?)


__9. CI & tooling: build both camera variants__

New doc:
* `_Documentation/building_firmware.md`
* Also some scripts etc that seem releavant to Victor's backend build system:
`.github/workflows/build_and_upload_firmware.yml, scripts/upload_firmware.js, _Tools/build_ww500.sh`

___COMMENTS AND QUESTIONS___

1. The `_Documentation/building_firmware.md` claims to be up do date and the earlier `Compile_and_flash.md` - is 'SUPERSEDED (July 2026)'
I don't think this is true. Revisit this.

__Further Notes__

The PR #141 doc then has other notes on verification steps, companion work in other repos etc.

