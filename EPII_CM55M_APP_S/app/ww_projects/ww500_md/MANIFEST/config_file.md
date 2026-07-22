# Description of the CONFIG.TXT File
#### CGP - 25 April 2026 (updated 13 July 2026: parameter 33 - MD global-motion rejection; 11 July 2026: parameters 29-32 - RP camera auto-exposure, white-balance mode and resolution)

The CONFIG.TXT file contains "Operational Parameters" for the WW500.

## File Format

Entries in CONFIG.TXT are mainly lines with two numbers:
* Index
* Value

Lines starting with '#' are treated as comments and are ignored.

For example two lines might be:
```
5 2
6 500
```

In this example:
* '5' is the index and '2' is the value
* '6' is the index and '500' is the value

ALSO - a different format for GPS location string - see below.

From the table below, 5 is the index for `OP_PARAMETER_NUM_PICTURES` and the value 2 means the WW500 takes 2 
pictures when triggered. 

From the table below, 6 is the index for `OP_PARAMETER_PICTURE_INTERVAL` and the value 500 means 
the pictured are taken at 500ms intervals.

Operational Parameters which are not present in CONFIG.TXT are given their default values.

Parameters changed from the mobile app or the console (`setop`) are written back to
CONFIG.TXT immediately, so a change survives sleep and power-off even if no image is
captured first.

## Operational Parameters Table

| Index | Name                                  | Default Value | Notes                                                |
| ----- | ------------------------------------- | ------------- | ---------------------------------------------------- |
|     0 | OP_PARAMETER_SEQUENCE_NUMBER          | 1             | Image file number. Increments when the file is written.  |
|     1 | OP_PARAMETER_NUM_NN_ANALYSES          | 0             | The number of times the neural network model has run. |
|     2 | OP_PARAMETER_NUM_POSITIVE_NN_ANALYSES | 0             | The number of times the neural network model detects the target. |
|     3 | OP_PARAMETER_NUM_COLD_BOOTS           | 0             | The number of AI processor cold boots.  |
|     4 | OP_PARAMETER_NUM_WARM_BOOTS           | 0             | The number of AI processor warm boots. |
|     5 | OP_PARAMETER_NUM_PICTURES             | 1             | The number of images to capture each time the processor receives a motion detect event or a time lapse event. |
|     6 | OP_PARAMETER_PICTURE_INTERVAL         | 500           | The interval (in ms) between each of the above images. Limited to about 2000 for HM0360. Must be less than OP_PARAMETER_INTERVAL_BEFORE_DPD |
|     7 | OP_PARAMETER_TIMELAPSE_INTERVAL       | 0             | The interval (in s) between entering DPD and waking again to take the next timelapse image (0 inhibits) |
|     8 | OP_PARAMETER_INTERVAL_BEFORE_DPD      | 1000          | The interval (in ms) between when all FreeRTOS task activity ceases and the AI processor entering DPD.|
|     9 | OP_PARAMETER_LED_BRIGHTNESS_PERCENT   | 5             | Flash LED duty cycle (brightness) in percent (approximately, 0 means 'dim', not 'off') |
|    10 | OP_PARAMETER_CAMERA_ENABLED           | 1             | Camera and NN system disabled, 1 = Camera and NN system enabled |
|    11 | OP_PARAMETER_MD_INTERVAL              | 0             | Interval (ms) between frames in motion detect mode (0 inhibits motion detection)|
|    12 | OP_PARAMETER_FLASH_DURATION           | 100           | Duration (ms) that LED flash is on (RP camera only)                 |
|    13 | OP_PARAMETER_FLASH_LED                | 0             | LED bit mask: visible LED used = 1, infra-red LED used =2, none = 0              |
|    14 | OP_PARAMETER_MODEL_PROJECT            | 0             | Model project ID used for the NN model (0 disables NN)|
|    15 | OP_PARAMETER_MODEL_VERSION            | 0             | Model version number used for the NN model |
|    16 | OP_PARAMETER_MODEL_THRESHOLD          | 18            | Logit threshold for detection (0-127) |
|    17 | OP_PARAMETER_MD_SENSITIVITY           | 1             | Motion Detection Sensitivity: 0=off, 1=low, 2=medium, 3=high |
|    18 | OP_PARAMETER_TEST_MODE_BITS           | 0             | To manage test configurations: bit or bits indicate a test function |
|    19 | OP_PARAMETER_IMAGES_COUNT     		| 0             | Count of images in the current image folder. Use this to decide to create a new image folder. |
|    20 | OP_PARAMETER_IMAGES_FILE_INDEX 		| 0             | Count of image folders |
|    21 | OP_PARAMETER_MD_FLASH_LED 			| 2             | LED used to illuminate motion-detection frames while asleep: 0 = none, 1 = visible, 2 = IR |
|    22 | OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT | 50         | Brightness of the motion-detection illumination (percent; 16 hardware levels). Default raised from 5 - too dim for night MD |
|    23 | OP_PARAMETER_AE_DARK_THRESHOLD 		| 65            | AE Mean (0-255) below this means the scene is dark and the flash is needed |
|    24 | OP_PARAMETER_AE_CHECK_INTERVAL 		| 15            | Interval (minutes) between periodic AE light-level checks. 0 disables |
|    25 | OP_PARAMETER_AE_FLASH_STATE 			| 0             | Last AE flash decision (0/1). Runtime state - leave as 0 |
|    26 | OP_PARAMETER_SLOT_SWITCH 				| 1             | Automatic light-based camera image switching: 0 = off (manual `switchslot` only), 1 = automatic (night image in the dark, colour image in daylight; reboots at the next sleep) |
|    27 | OP_PARAMETER_WB_RED_GAIN 				| 286           | Software white-balance RED gain, Q8.8 (256 = 1.0x, 0 = correction off). RP3 colour camera only |
|    28 | OP_PARAMETER_WB_BLUE_GAIN 				| 326           | Software white-balance BLUE gain, Q8.8 (256 = 1.0x, 0 = correction off). RP3 colour camera only |
|    29 | OP_PARAMETER_CAM_AE_ENABLE 			| 1             | RP camera auto-exposure: 0 = off (init-table exposure), 1 = on. Highlight-metered loop steps sensor exposure (8-5000 lines) then analog gain (to 16x) toward the target - see `ae.c` |
|    30 | OP_PARAMETER_CAM_AE_TARGET 			| 110           | Auto-exposure target: raw bright-quartile (p75) luma, 0-250 (0 = built-in default 95). Bright parts of the scene render just below white after the tone curve |
|    31 | OP_PARAMETER_CAM_WB_MODE 				| 1             | RP camera white balance: 0 = off (hardware JPEG), 1 = auto (warmth-biased grey-world measured per frame), 2 = manual op27/op28. Auto falls back to manual for flash-lit or too-dark frames - see `img_correct.c` |
|    32 | OP_PARAMETER_CAM_RESOLUTION 			| 0             | RP camera capture resolution: 0 = 640x480 (normal pipeline), 1 = 1280x960 single JPEG via the CPU pipeline - requires the NN off (op14 = 0); see [hires-capture.md](hires-capture.md) |
|    33 | OP_PARAMETER_MD_BLOCK_NUM_MAX 		| 0             | Global-motion rejection: skip the capture when an MD wake shows motion in MORE than this many of the 256 grid blocks. A whole-scene shift (camera knock/pan, lighting change) lights most of the grid; an animal lights a small local cluster. 0 disables; try 100-180. Complements the sensor's MD_BLOCK_NUM_TH, which is the *minimum* blocks needed to trigger. Checked at wake, before the capture starts - the wake itself cannot be prevented, but the false capture/save/upload is |

## More Details

For more details of how the Operational Parameters are used, see [Operational_Parameters.md](https://github.com/wildlifeai/Seeed_Grove_Vision_AI_Module_V2/blob/dev/_Documentation/Operational_Parameters.md)

## GPS location

This can be set by the app and needs to be saved while the AI processor is in deep sleep.
This is done with a line starting "G " followed by a string. Examples follow.

```
G 36°49'55" S 174°47'51" E 31 Above
G 36°49'55.5" S 174°47'51.8" E 31.2 Above
G 36°49'55.68" S 174°47'51.83" E 31.234 Above
G 51°30'26.123" N 0°7'39.456" W 11.5 Above
G 90°0'0" S 180°0'0" W 0 Above
G 0°0'0" N 0°0'0" E 8848.86 Above
G 27°59'17.28" N 86°55'30.12" E 8848.86 Above
G 35°41'22.2" N 139°41'30.5" E 40 Below

The reasoning behind each:

Line 1–3: Auckland roughly, progressively adding decimal places to seconds — good for checking the fractional parsing at different precisions
Line 4: London — tests a near-zero longitude (single digit degrees, W direction)
Line 5: Extreme corner case — maximum lat/lon values, all zeros for seconds
Line 6: Null Island (0,0) with a non-zero altitude — isolates altitude parsing from coordinate parsing
Line 7: Mount Everest summit — realistic high altitude with decimal seconds in both coordinates
Line 8: Tests Below sea level, e.g. Dead Sea area — the only case that exercises the alt->ref == 1 path

```

## Deployment ID

Originally the 128-bit deployment ID was saved in eight 16-bit operational parameter values. 
Since 1 April 2026 there is an ability to save and restore this as a single line string beginning 
with "I " with the string following. Example:

```
I 12345678-0000-0000-0000-000000abc666
```

The earlier OP_PARAMETER_DEPLOYMENT_ID_CHUNK_1 - OP_PARAMETER_DEPLOYMENT_ID_CHUNK_8
block has been removed.


## LED Flash operation

The flash is driven by the AE light sensor: the HM0360 auto-exposure registers are read
after each capture (and periodically - see OP_PARAMETER_AE_CHECK_INTERVAL), and the flash
operates when the scene is dark. The time-of-day and always-on modes have been removed.

| Case                       | Setting |
|----------------------------|---------|
| Capture flash off          | OP_PARAMETER_FLASH_LED = 0 |
| Capture flash by AE sensor | OP_PARAMETER_FLASH_LED = 1 (visible) or 2 (IR); brightness = OP_PARAMETER_LED_BRIGHTNESS_PERCENT |
| MD illumination            | OP_PARAMETER_MD_FLASH_LED / OP_PARAMETER_MD_FLASH_BRIGHTNESS_PERCENT (also gated by the AE dark decision) |

Tuning: OP_PARAMETER_AE_DARK_THRESHOLD (dark below this AE Mean value; default 65) and
OP_PARAMETER_AE_CHECK_INTERVAL (minutes between light checks when no timelapse runs).
See _Documentation/AE_Light_Sensor_Roadmap.md in the firmware repository.
