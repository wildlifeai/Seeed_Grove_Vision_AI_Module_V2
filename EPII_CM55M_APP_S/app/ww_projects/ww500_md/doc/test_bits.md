# Test Bits
#### CGP - 26 March 2026

I have introduced simpler control over some modes that are present for testing - this allows a test to be invoked
without having to recompile the source code.

## Operational Parameter `OP_PARAMETER_TEST_MODE_BITS`

This is one of the vkaues that can be set in the CONFIG.TXT file, and also can be set by the app
using the `AI setop 18 <n>` command. OP_PARAMETER_TEST_MODE_BITS is index 18. 

Each bit in the operational parameter can enable one test. At present the tests are as described below.

## Test bits

To invoke a test, set a bit according to the table below

| Bit | Bit Name      | Test     |
|-----|-----------|----------|
| 0   | TEST_BIT_TONE_MAPPING  | HM0360 Tone mapping    |
| 1   | TEST_BIT_SAVE_BMP  | BMP file creation          |
| 2   | TEST_BIT_FLASH_BRIGHTNESS  | LED Flash Brightness   |
| 3   | TEST_BIT_SKIP_FILE_CREATION  | Skip image file creation    |

#### HM0360 Tone mapping

This programs different values in the HM0360 registers that map input to output brightness.
See the data sheet section 4.10. The effect can be to increase the brightness of low-brightness pixels.
This might be most easily seen when part of the image is bright and part is not - for example
when taking pictures inside a room with a window (outside being brightly lit).

There are 4 sets of register settings: default, low, medium, high:
```
typedef enum {
	TONE_MAPPING_FLAT,
	TONE_MAPPING_LOW,
	TONE_MAPPING_MEDIUM,
	TONE_MAPPING_HIGH,
	TONE_MAPPING_NUMBER		// This is not one of the options - it serves to define the number of options (=4)
} TONE_CONFIG_E;
```

These can be set by the function `cisdp_sensor_set_tone()`.

The test calls this function with each of the 4 possible values, changing after each image is taken.

To see the effect, set Set OP_PARAMETER_NUM_PICTURES to 4 and the device will take 4 images each with a different setting.

Running the test with OP_PARAMETER_NUM_PICTURES > 4 has no further effect: the setting stays at TONE_MAPPING_HIGH.

My feeling is that TONE_MAPPING_LOW is probably best - this is the default setting.

At the time of writing the different settings can't be changed except during this test, but with would be easy enough
to make this an operation paarmeter setting - ask. 


#### BMP file creation

This test creates a grey-scale .bmp image (no compression). It allows comparison between the quality of 
jpeg images comapred to uncompressed images.

When invoked, the test saves successive images as jpeg and bmp.

To see the effect, set Set OP_PARAMETER_NUM_PICTURES an even number. The test can be combined with 
the HM0360 Tone mapping test - in which case set OP_PARAMETER_NUM_PICTURES = 8 and you wil get a bmp and a jpeg image at each of the tone settings.

I think that bmp images are certainly better quality, though the files are about 20 times larger.

At the time of writing the bmp file type can't be saved except during this test, 
but with would be easy enough to make this an operation paarmeter setting - ask. 

#### LED Flash Brightness

This allows testing to the flash LED at different brightness levels. This in turn makes it possible to determine
the appropriate flash setting for different scenes - near and far.

When set, successive images are taken with different levels of flash brightness, from minimum to maximum.

Set OP_PARAMETER_NUM_PICTURES to 7 to see all options,
and set OP_PARAMETER_FLASH_LED to 1 or 2 (visible ot IR flash).

#### Skip image file creation  

I added this to speed up the rate at which the motion detection bits and auto-exposure values are sent to the app via BLE.

The neural network is still invoked, and the red or green LEDs flash to show the result of the analysis.

When enabled the generation of files, including EXIF generation, is skipped. It appears that images are taken and reported via
BLE at about 5Hz.

Consider making OP_PARAMETER_NUM_PICTURES = a large number (I tested with 60)
 and OP_PARAMETER_PICTURE_INTERVAL = 1
